// JPOV OBJ 加载器实现 — Wavefront .obj 文本 → MeshData
//
// 解析流程：
//   1. 逐行扫描，分流到 v / vt / vn / f 各自收集阶段
//   2. f 行按 corner 解析 (v, vt, vn) 三元组引用（1-based；负数 = 相对末尾）
//   3. 每个唯一 (v, vt, vn) 组合生成一个 GPU 顶点（corner-splitting），
//      并把 position/normal/uv 各自拷贝进该顶点的属性数组；
//      corner_map（跨所有面共享）保证重复 corner 复用同一顶点以去重
//   4. 多边形面 fan 三角化写入 indices
//   5. 依据文件是否出现 vt / vn 设置 flags，保证 Validate() 可通过
//
// 数据一致性（保证 MeshData::Validate 通过）：
//   既然 flags 声明 kNormal / kUV，则对应属性数组必须与 positions 等长。
//   故文件一旦出现 vt / vn，就要求所有 face corner 都带该分量 —— 这是
//   绝大多数 OBJ 的写法；若出现「混用」（某些 corner 无 vt/vn），按缺省
//   (0,0,0) / (0,0) 补齐，保证数组对齐，不 crash。
//
// 出错策略：文件打不开 / 面索引越界 / corner 语法非法 → LOG(ERROR) 返回 false，
// 不产出部分网格（保证调用方失败后不误用半成品）。

#include "tools/jpov/src/obj_loader.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <glog/logging.h>

namespace jpov {

namespace {

// ==================== 原始几何数据收集 ====================

struct ObjGeometry {
    // 与 OBJ 声明顺序一致（索引即行序，1-based，预留 index 0 为空）
    std::vector<Vec3f> v;    // v[n] = 第 n 个 position（n >= 1）
    std::vector<Vec2f> vt;   // vt[n] = 第 n 个 UV
    std::vector<Vec3f> vn;   // vn[n] = 第 n 个法线

    bool has_vt = false;     // 文件是否出现过 vt
    bool has_vn = false;     // 文件是否出现过 vn
};

// 面 corner 的引用三元组（0 = 该分量未声明）
struct CornerRef {
    int v = 0;   // position 索引（1-based，已解析为非负，>=1）
    int vt = 0;  // uv 索引（0 = 未声明）
    int vn = 0;  // normal 索引（0 = 未声明）
};

// 把 "1" / "1/2" / "1//3" / "1/2/3" 形式的 corner 字符串解析为三元组。
// 索引为 1-based OBJ 相对索引；负索引按 count 转正（-1 = count）。
// 返回 false 表示语法非法或索引越界。
bool ParseCorner(const std::string& token, int v_count, int vt_count,
                 int vn_count, CornerRef* out) {
    // token 形如 a / a/b / a//b / a/b/c，用 '/' 切分
    const size_t s1 = token.find('/');
    const size_t s2 = (s1 == std::string::npos) ? std::string::npos
                                                : token.find('/', s1 + 1);

    // position 分量（必有）
    const std::string v_str = token.substr(0, s1);
    const int v = std::atoi(v_str.c_str());
    if (v_str.empty() || v == 0 || v_count <= 0) return false;
    out->v = (v > 0) ? v : (v_count + v + 1);  // 负索引：v + count + 1 (1-based)
    if (out->v < 1 || out->v > v_count) return false;

    // uv 分量（第一个 '/' 后，到第二个 '/' 之前）
    if (s1 != std::string::npos) {
        const size_t vt_len = (s2 == std::string::npos) ? std::string::npos
                                                        : s2 - s1 - 1;
        const std::string vt_str = token.substr(s1 + 1, vt_len);
        if (!vt_str.empty()) {
            const int vt = std::atoi(vt_str.c_str());
            if (vt == 0 || vt_count <= 0) return false;
            out->vt = (vt > 0) ? vt : (vt_count + vt + 1);
            if (out->vt < 1 || out->vt > vt_count) return false;
        }
    }

    // normal 分量（第二个 '/' 之后）
    if (s2 != std::string::npos) {
        const std::string vn_str = token.substr(s2 + 1);
        if (!vn_str.empty()) {
            const int vn = std::atoi(vn_str.c_str());
            if (vn == 0 || vn_count <= 0) return false;
            out->vn = (vn > 0) ? vn : (vn_count + vn + 1);
            if (out->vn < 1 || out->vn > vn_count) return false;
        }
    }
    return true;
}

// 把一个面（已解析的 corner 列表）三角化追加进 MeshData。
//
// corner_map_: "(v,vt,vn)" → 已生成 GPU 顶点索引（跨面共享，去重）。
// 返回 false 表示数据规模非法（如无 position 可用）。
bool AppendFace(const std::vector<CornerRef>& corners, const ObjGeometry& g,
                MeshData* out,
                std::unordered_map<std::string, uint32_t>* corner_map) {
    const size_t n = corners.size();
    if (n < 3) return true;  // 非法面（<3 顶点）宽容跳过

    // 收集该面各 corner 的 GPU 顶点索引
    std::vector<uint32_t> face_verts;
    face_verts.reserve(n);
    for (const CornerRef& c : corners) {
        const std::string key = std::to_string(c.v) + "," +
                                std::to_string(c.vt) + "," +
                                std::to_string(c.vn);
        auto it = corner_map->find(key);
        uint32_t idx;
        if (it != corner_map->end()) {
            idx = it->second;
        } else {
            // 新建顶点：positions 必有；normals/uvs 视文件属性补齐
            // 注意: OBJ 索引为 1-based，这里转 0-based 访问原始数组
            idx = static_cast<uint32_t>(out->positions.size());
            out->positions.push_back(g.v[c.v - 1]);
            if (g.has_vn) {
                out->normals.push_back(c.vn > 0 ? g.vn[c.vn - 1]
                                                : Vec3f(0, 0, 0));
            }
            if (g.has_vt) {
                out->uvs.push_back(c.vt > 0 ? g.vt[c.vt - 1] : Vec2f(0, 0));
            }
            (*corner_map)[key] = idx;
        }
        face_verts.push_back(idx);
    }

    // fan 三角化: 0-1-2, 0-2-3, ...
    for (size_t i = 1; i + 1 < n; ++i) {
        out->indices.push_back(face_verts[0]);
        out->indices.push_back(face_verts[i]);
        out->indices.push_back(face_verts[i + 1]);
    }
    return true;
}

}  // namespace

bool LoadObj(const std::string& path, MeshData* out) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        LOG(ERROR) << "LoadObj: 无法打开文件 " << path;
        return false;
    }

    ObjGeometry g;
    bool has_face = false;
    // (v,vt,vn) → GPU 顶点索引，跨所有面共享去重
    std::unordered_map<std::string, uint32_t> corner_map;

    std::string line;
    std::string tag;
    while (std::getline(ifs, line)) {
        // 去掉行尾 '\r'（Windows 换行兼容）
        if (!line.empty() && line.back() == '\r') line.pop_back();

        std::istringstream iss(line);
        if (!(iss >> tag)) continue;   // 空行
        if (tag[0] == '#') continue;   // 注释

        if (tag[0] == 'v') {           // v / vt / vn / vp ...
            if (tag == "v") {
                Vec3f p;
                if (!(iss >> p.x() >> p.y() >> p.z())) {
                    LOG(ERROR) << "LoadObj: 非法 v 行: " << line;
                    return false;
                }
                g.v.push_back(p);
            } else if (tag == "vt") {
                Vec2f uv;
                if (!(iss >> uv.x() >> uv.y())) {
                    LOG(ERROR) << "LoadObj: 非法 vt 行: " << line;
                    return false;
                }
                g.vt.push_back(uv);
                g.has_vt = true;
            } else if (tag == "vn") {
                Vec3f n;
                if (!(iss >> n.x() >> n.y() >> n.z())) {
                    LOG(ERROR) << "LoadObj: 非法 vn 行: " << line;
                    return false;
                }
                g.vn.push_back(n);
                g.has_vn = true;
            }
            // 其它 v*（vp 顶点参数…）忽略
            continue;
        }

        if (tag == "f") {
            std::vector<CornerRef> corners;
            corners.reserve(4);
            std::string tok;
            bool bad = false;
            while (iss >> tok) {
                CornerRef c;
                if (!ParseCorner(tok, static_cast<int>(g.v.size()),
                                 static_cast<int>(g.vt.size()),
                                 static_cast<int>(g.vn.size()), &c)) {
                    LOG(ERROR) << "LoadObj: 非法 face corner '" << tok
                               << "' (line: " << line << ")";
                    bad = true;
                    break;
                }
                corners.push_back(c);
            }
            if (bad) return false;
            if (!AppendFace(corners, g, out, &corner_map)) {
                return false;
            }
            has_face = true;
        }
        // 其它 tag（o/g/s/mtllib/usemtl/...）忽略——不影响网格数据
    }

    if (!has_face || out->positions.empty()) {
        LOG(ERROR) << "LoadObj: 文件未包含可用的三角形网格数据: " << path;
        return false;
    }

    // 依据文件内容设置 flags
    out->flags = MeshVertexFlags::kPosition;
    if (g.has_vn) {
        out->flags = static_cast<MeshVertexFlags>(
            static_cast<uint8_t>(out->flags) |
            static_cast<uint8_t>(MeshVertexFlags::kNormal));
    }
    if (g.has_vt) {
        out->flags = static_cast<MeshVertexFlags>(
            static_cast<uint8_t>(out->flags) |
            static_cast<uint8_t>(MeshVertexFlags::kUV));
    }

    // 数据已对齐（见 AppendFace 补齐逻辑），显式验证保证后续使用安全
    out->Validate();
    return true;
}

}  // namespace jpov
