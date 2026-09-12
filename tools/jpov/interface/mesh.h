// JPOV MeshData — 顶点级网格数据定义
//
// MeshData 是 CPU 端的网格数据载体，描述"这个 mesh 包含哪些顶点属性"
// （位置 / 法线 / UV / 骨骼蒙皮），每个属性一个独立 vector。
//
// 设计要点：
// - flags 位掩码声明 mesh 包含哪些属性，各属性 vector 的长度必须与
//   positions 一致（Validate() 校验）。
// - 骨骼蒙皮（kJoints）扩展为每个顶点 4 组 joint 索引 + 4 组权重，
//   即每顶点至多被 4 个关节影响。
// - GPUMesh（CPU → GPU 上传后的句柄）在 gpumesh.h 中定义，
//   本文件只关心 CPU 侧数据。
//
// 用法：
//   MeshData mesh;
//   mesh.flags = MeshVertexFlags::kPosition | MeshVertexFlags::kUV;
//   mesh.positions = {...};
//   mesh.uvs = {...};
//   mesh.indices = {...};
//   mesh.Validate();  // 校验数组对齐，非法输入 LOG(FATAL) crash
//
// 说明：
//   - indices（索引）不属于 flags 位掩码：indexed mesh 渲染必需，
//     non-indexed mesh 可留空（此时渲染按顶点序逐三角形）。

#ifndef JPOV_MESH_H_
#define JPOV_MESH_H_

#include <array>
#include <cstdint>
#include <vector>

#include <glog/logging.h>
#include "geom/common/vec.h"

namespace jpov {

// ==================== 类型别名 ====================

// 复用 geom 库的向量类型（与 render_command.h / camera.h 一致）
using Vec2f = geom::Vec2<float>;
using Vec3f = geom::Vec3<float>;

// 骨骼蒙皮每顶点固定 4 组 joint/weight（与 Danis 伪代码 int32_t[4]/float[4] 对应），
// 由 joint_indices / joint_weights 数组元素类型直接表达，无需运行时配置。

// ==================== 顶点属性位掩码 ====================

// Mesh 包含哪些顶点属性，位掩码可组合（| 运算）。
// kPosition 为必有项；其余按数据可用性声明。
enum class MeshVertexFlags : uint8_t {
    kNone     = 0,
    kPosition = 1 << 0,  // 必有
    kNormal   = 1 << 1,
    kUV       = 1 << 2,
    kJoints   = 1 << 3,  // 骨骼蒙皮（joint_indices + joint_weights）
    kTangent  = 1 << 4,  // 切线（法线映射 TBN 用，需 kNormal + kUV）
};

// 位掩码按位运算辅助：判断 flags 是否包含某属性。
// 用法：HasFlag(mesh.flags, MeshVertexFlags::kNormal)
inline bool MeshHasFlag(MeshVertexFlags flags, MeshVertexFlags flag) {
    return (static_cast<uint8_t>(flags) & static_cast<uint8_t>(flag)) != 0;
}

// ==================== CPU 侧网格数据 ====================

// CPU 端网格数据：逐属性数组 + 索引。
//
// 数组对齐规则（Validate() 强制）：
//   - normals / uvs / joint_indices / joint_weights 若非空，长度必须 == positions.size()
//   - positions 非空（至少 1 个顶点）
//   - positions.size() 必须能被子属性长度整除，保证每个顶点都有完整属性
//   - indices 可为空（non-indexed），非空时按 triangle list 语义使用
//   - 元素数量（vertex_count）上限以内置宏约束，超限 LOG(FATAL)
//
// Validate() 在数据就绪后显式调用一次；渲染前调用方可确保数据合法。
struct MeshData {
    // 声明这个 mesh 包含哪些属性
    MeshVertexFlags flags = MeshVertexFlags::kPosition;

    // 对应各 flag 的数据（长度须与 positions 一致）
    std::vector<Vec3f> positions;
    std::vector<Vec3f> normals;
    std::vector<Vec2f> uvs;
    std::vector<Vec3f> tangents;   // 切向量（逐顶点，法线映射 TBN 用；kTangent 时非空）
    std::vector<uint32_t> indices;

    // 骨骼专属（flags 包含 kJoints 时才有效）
    // 用 std::array 而非原始 C 数组：std::vector 容纳可拷贝元素，
    // 使 MeshData 可整体拷贝/移动（否则 vector<C数组> 不可拷贝）。
    std::vector<std::array<int32_t, 4>> joint_indices;  // 每顶点 4 个 joint 索引
    std::vector<std::array<float, 4>>   joint_weights;  // 每顶点 4 个 joint 权重

    // 顶点数（positions 长度）。
    // Pre-condition: 已调用 Validate()（或至少 positions 已填充）
    size_t VertexCount() const { return positions.size(); }

    // MakeBox: 构造一个局部坐标轴对齐盒（box），中心在局部原点。
    //
    // 只需三个尺寸（沿局部坐标轴的半宽），摆放位置/朝向交给渲染时的
    // DrawObject3D 的 center / up / front 决定（见 render_command.h）：
    //   - 局部 +Z 轴 = Draw 的 front 方向 → front_half_width
    //   - 局部 +Y 轴 = Draw 的 up 方向 → up_half_width
    //   - 局部 +X 轴 = Draw 的 left 方向（= cross(up, front)）→ left_half_width
    //
    // 生成的盒体：8 角点 = (±left_half_width, ±up_half_width, ±front_half_width)，
    // 6 个面、每面独立 4 顶点（共 24 顶点）+ 独立法线，12 个三角形（36 index）。
    // 顶点属性含 kPosition + kNormal。
    //
    // Pre-condition: 三个 half_width 均 > 0。
    static MeshData MakeBox(float front_half_width,
                            float up_half_width,
                            float left_half_width);

    // MakeOrientedBox: 构造一个**已带旋转与平移**的盒（中心在 origin 造好后整体变换）。
    //
    // 与 MakeBox 的唯一区别：MakeBox 只造局部轴对齐盒、朝向留给渲染时的
    // DrawObject3D(up/front)；本函数把朝向**烘进顶点**，因此可在一个 MeshData 里
    // 拼装多根不同朝向的几何（如骨架火柴人的每根骨——一个 Draw 只能给一个朝向，
    // 装不下十几根朝向各异的骨）。
    //
    // up / front 语义与 DrawObject3D 完全一致（见 render_command.h 的 DrawObject3D 注释），
    // 对应模型坐标系：
    //   - local +Y 轴 → up 方向
    //   - local +Z 轴 → front 方向
    //   - local +X 轴 → left 方向（= normalize(cross(up, front))，保证右手系）
    // 即三半宽与模型的对应关系同 MakeBox：front→+Z、up→+Y、left→+X。
    //
    // 变换顺序：顶点 = translation + R(up,front)·v_local；法线 = R(up,front)·n_local
    //   （法线只转不平移；R 为纯旋转 det=+1，故法线无需额外翻向）。
    //
    // Pre-condition: 三个 half_width 均 > 0；up / front 均非零且不平行（保证 cross 非退化）。
    static MeshData MakeOrientedBox(float front_half_width,
                                    float up_half_width,
                                    float left_half_width,
                                    const Vec3f& up, const Vec3f& front,
                                    const Vec3f& translation);

    // 校验各数组长度与 positions 一致、flag 声明与数据匹配。
    // 非法输入（长度不一致 / 缺 position / 骨骼 flag 但数据缺失）→ LOG(FATAL) crash。
    //
    // Pre-condition: 调用前应已设置 flags 并填充各数组。
    void Validate() const;
};

// Validate() 实现。
//
// 校验规则：
//   1. positions 非空（每个 mesh 至少 1 个顶点）。
//   2. 各可选属性数组（normals/uvs/joint_indices/joint_weights）非空时，
//      长度必须 == positions.size()。
//   3. flags 声明了某属性但对应数组为空 → crash（声明与数据不符）。
//   4. flags 未声明某属性但对应数组非空 → crash（多余数据，属误用）。
//   5. 骨骼 flag（kJoints）声明时 joint_indices / joint_weights 必须成对存在。
inline void MeshData::Validate() const {
    const size_t vcount = positions.size();

    // 1. 至少 1 个顶点
    CHECK_GT(vcount, 0u) << "MeshData::Validate: positions 不能为空";

    // 2. 位置属性必有（flags 必须声明 kPosition）
    CHECK(MeshHasFlag(flags, MeshVertexFlags::kPosition))
        << "MeshData::Validate: kPosition 为必有属性，flags 必须包含它";

    // 3. 各可选属性：非空数组长度必须与 positions 一致
    if (!normals.empty()) {
        CHECK_EQ(normals.size(), vcount)
            << "MeshData::Validate: normals.size() != positions.size()";
    }
    if (!uvs.empty()) {
        CHECK_EQ(uvs.size(), vcount)
            << "MeshData::Validate: uvs.size() != positions.size()";
    }
    if (!tangents.empty()) {
        CHECK_EQ(tangents.size(), vcount)
            << "MeshData::Validate: tangents.size() != positions.size()";
    }
    if (!joint_indices.empty()) {
        CHECK_EQ(joint_indices.size(), vcount)
            << "MeshData::Validate: joint_indices.size() != positions.size()";
    }
    if (!joint_weights.empty()) {
        CHECK_EQ(joint_weights.size(), vcount)
            << "MeshData::Validate: joint_weights.size() != positions.size()";
    }

    // 4. flags 声明与数据一致性
    if (MeshHasFlag(flags, MeshVertexFlags::kNormal)) {
        CHECK_EQ(normals.size(), vcount)
            << "MeshData::Validate: flags 声明 kNormal 但 normals 长度不一致或缺数据";
    } else {
        CHECK(normals.empty())
            << "MeshData::Validate: 未声明 kNormal 但 normals 非空";
    }

    if (MeshHasFlag(flags, MeshVertexFlags::kUV)) {
        CHECK_EQ(uvs.size(), vcount)
            << "MeshData::Validate: flags 声明 kUV 但 uvs 长度不一致或缺数据";
    } else {
        CHECK(uvs.empty())
            << "MeshData::Validate: 未声明 kUV 但 uvs 非空";
    }

    if (MeshHasFlag(flags, MeshVertexFlags::kTangent)) {
        CHECK_EQ(tangents.size(), vcount)
            << "MeshData::Validate: flags 声明 kTangent 但 tangents 长度不一致或缺数据";
        // TBN 需要 normal + uv 才能从纹理推导切线空间
        CHECK(MeshHasFlag(flags, MeshVertexFlags::kNormal))
            << "MeshData::Validate: flags 声明 kTangent 但未声明 kNormal";
        CHECK(MeshHasFlag(flags, MeshVertexFlags::kUV))
            << "MeshData::Validate: flags 声明 kTangent 但未声明 kUV";
    } else {
        CHECK(tangents.empty())
            << "MeshData::Validate: 未声明 kTangent 但 tangents 非空";
    }

    const bool joints_declared = MeshHasFlag(flags, MeshVertexFlags::kJoints);
    if (joints_declared) {
        // 5. 骨骼 flag 时 joint_indices / joint_weights 必须成对存在且对齐
        CHECK_EQ(joint_indices.size(), vcount)
            << "MeshData::Validate: flags 声明 kJoints 但 joint_indices 缺失或长度不一致";
        CHECK_EQ(joint_weights.size(), vcount)
            << "MeshData::Validate: flags 声明 kJoints 但 joint_weights 缺失或长度不一致";
    } else {
        CHECK(joint_indices.empty())
            << "MeshData::Validate: 未声明 kJoints 但 joint_indices 非空";
        CHECK(joint_weights.empty())
            << "MeshData::Validate: 未声明 kJoints 但 joint_weights 非空";
    }
}

inline MeshData MeshData::MakeBox(float front_half_width,
                                  float up_half_width,
                                  float left_half_width) {
    CHECK_GT(front_half_width, 0.0f) << "MakeBox: front_half_width 必须 > 0";
    CHECK_GT(up_half_width, 0.0f) << "MakeBox: up_half_width 必须 > 0";
    CHECK_GT(left_half_width, 0.0f) << "MakeBox: left_half_width 必须 > 0";

    const float fw = front_half_width;
    const float uw = up_half_width;
    const float lw = left_half_width;

    // 8 角点（局部轴对齐，中心原点）。局部坐标轴：+X=left, +Y=up, +Z=front。
    // idx 0..7 = (±l, ±u, ±f) 组合（sl/su/sf 分别是 ±1 缩放 lw/uw/fw 半宽）。
    auto corner = [&](int sl, int su, int sf) {
        return Vec3f(sl * lw, su * uw, sf * fw);
    };
    const Vec3f corners[8] = {
        corner(+1, +1, +1),  // 0: +l +u +f
        corner(-1, +1, +1),  // 1: -l +u +f
        corner(+1, -1, +1),  // 2: +l -u +f
        corner(-1, -1, +1),  // 3: -l -u +f
        corner(+1, +1, -1),  // 4: +l +u -f
        corner(-1, +1, -1),  // 5: -l +u -f
        corner(+1, -1, -1),  // 6: +l -u -f
        corner(-1, -1, -1),  // 7: -l -u -f
    };

    MeshData mesh;
    mesh.flags = static_cast<MeshVertexFlags>(
        static_cast<uint8_t>(MeshVertexFlags::kPosition) |
        static_cast<uint8_t>(MeshVertexFlags::kNormal));

    // 每个面独立 4 顶点（共 24）+ 独立法线 + 2 三角形（共 12）。
    auto addFace = [&mesh](const Vec3f& p0, const Vec3f& p1,
                          const Vec3f& p2, const Vec3f& p3,
                          const Vec3f& n) {
        const uint32_t base = static_cast<uint32_t>(mesh.positions.size());
        mesh.positions.push_back(p0);
        mesh.positions.push_back(p1);
        mesh.positions.push_back(p2);
        mesh.positions.push_back(p3);
        for (int i = 0; i < 4; ++i) {
            mesh.normals.push_back(n);
        }
        mesh.indices.push_back(base + 0);
        mesh.indices.push_back(base + 1);
        mesh.indices.push_back(base + 2);
        mesh.indices.push_back(base + 0);
        mesh.indices.push_back(base + 2);
        mesh.indices.push_back(base + 3);
    };

    const Vec3f pos_x( 1.0f,  0.0f,  0.0f);  // +l（+X）
    const Vec3f neg_x(-1.0f,  0.0f,  0.0f);  // -l（-X）
    const Vec3f pos_y( 0.0f,  1.0f,  0.0f);  // +u（+Y）
    const Vec3f neg_y( 0.0f, -1.0f,  0.0f);  // -u（-Y）
    const Vec3f pos_z( 0.0f,  0.0f,  1.0f);  // +f（+Z）
    const Vec3f neg_z( 0.0f,  0.0f, -1.0f);  // -f（-Z）

    // 6 个面（绕序 CCW，从面外侧看）。
    // +f 面（法线 +Z）
    addFace(corners[0], corners[1], corners[3], corners[2], pos_z);
    // -f 面（法线 -Z）
    addFace(corners[4], corners[6], corners[7], corners[5], neg_z);
    // +u 面（法线 +Y）
    addFace(corners[0], corners[4], corners[5], corners[1], pos_y);
    // -u 面（法线 -Y）
    addFace(corners[2], corners[3], corners[7], corners[6], neg_y);
    // +l 面（法线 +X）
    addFace(corners[0], corners[2], corners[6], corners[4], pos_x);
    // -l 面（法线 -X）
    addFace(corners[1], corners[5], corners[7], corners[3], neg_x);

    return mesh;
}

inline MeshData MeshData::MakeOrientedBox(float front_half_width,
                                          float up_half_width,
                                          float left_half_width,
                                          const Vec3f& up,
                                          const Vec3f& front,
                                          const Vec3f& translation) {
    const float u_len = std::sqrt(up.x()*up.x() + up.y()*up.y() + up.z()*up.z());
    const float f_len = std::sqrt(front.x()*front.x() + front.y()*front.y() +
                                  front.z()*front.z());
    CHECK_GT(u_len, 1e-8f) << "MakeOrientedBox: up 向量不能为零";
    CHECK_GT(f_len, 1e-8f) << "MakeOrientedBox: front 向量不能为零";

    const Vec3f upn(up.x()/u_len, up.y()/u_len, up.z()/u_len);
    const Vec3f frn(front.x()/f_len, front.y()/f_len, front.z()/f_len);

    // left = normalize(cross(up, front))，与 MakeBox / DrawObject3D 的局部 +X 一致。
    Vec3f left(upn.y()*frn.z() - upn.z()*frn.y(),
               upn.z()*frn.x() - upn.x()*frn.z(),
               upn.x()*frn.y() - upn.y()*frn.x());
    const float l_len = std::sqrt(left.x()*left.x() + left.y()*left.y() +
                                  left.z()*left.z());
    CHECK_GT(l_len, 1e-8f)
        << "MakeOrientedBox: up 与 front 平行（cross 退化），无法确定朝向";
    left = Vec3f(left.x()/l_len, left.y()/l_len, left.z()/l_len);

    // frn 正交化：减去在 upn 上的投影，使 (left, upn, frn) 成标准正交基。
    // 否则 up/front 略不垂直时会在盒上引入剪切（MakeBox 里同样的隐患）。
    const float proj = frn.x()*upn.x() + frn.y()*upn.y() + frn.z()*upn.z();
    Vec3f f_orth(frn.x() - proj*upn.x(), frn.y() - proj*upn.y(),
                 frn.z() - proj*upn.z());
    const float fo_len = std::sqrt(f_orth.x()*f_orth.x() + f_orth.y()*f_orth.y() +
                                   f_orth.z()*f_orth.z());
    CHECK_GT(fo_len, 1e-8f)
        << "MakeOrientedBox: up 与 front 近乎平行，正交化后退化";
    f_orth = Vec3f(f_orth.x()/fo_len, f_orth.y()/fo_len, f_orth.z()/fo_len);

    // 先造局部轴对齐盒，再逐个顶点/法线施加旋转 R = [left | upn | f_orth] 与平移。
    MeshData mesh = MakeBox(front_half_width, up_half_width, left_half_width);
    for (Vec3f& p : mesh.positions) {
        const Vec3f v(p.x(), p.y(), p.z());
        // R·v：局部坐标在 (left, upn, f_orth) 三轴上的分量线性组合。
        const Vec3f rotated(left.x()*v.x() + upn.x()*v.y() + f_orth.x()*v.z(),
                            left.y()*v.x() + upn.y()*v.y() + f_orth.y()*v.z(),
                            left.z()*v.x() + upn.z()*v.y() + f_orth.z()*v.z());
        p = Vec3f(rotated.x() + translation.x(),
                  rotated.y() + translation.y(),
                  rotated.z() + translation.z());
    }
    for (Vec3f& n : mesh.normals) {
        const Vec3f v(n.x(), n.y(), n.z());
        n = Vec3f(left.x()*v.x() + upn.x()*v.y() + f_orth.x()*v.z(),
                  left.y()*v.x() + upn.y()*v.y() + f_orth.y()*v.z(),
                  left.z()*v.x() + upn.z()*v.y() + f_orth.z()*v.z());
    }

    return mesh;
}

}  // namespace jpov

#endif  // JPOV_MESH_H_
