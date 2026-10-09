// JPOV 穿衣工具 — base color 贴图 alpha 归一（纯函数 + 图片编解码声明）
//
// 需求（2026-10-09 Danis）：一键把**被选取的顶点所围三角形**（三个顶点都被选中的三角形）
//   覆盖到的 base color 贴图区域 alpha 置 1（不透明）——用于修补透明贴图被误抠出的「洞」
//   （alpha < alphaCutoff ⇒ cutout 渲染时被 discard）。画笔**未选取**时，作用于整张贴图。
//
// 分层：
//   - 本头文件的**纯函数**（NormalizeAlphaWholeImage / NormalizeAlphaInUvTriangles /
//     CollectSelectedTrianglesUv）只碰 MeshData / UV / RGBA 像素，无 GL、无 stb，可纯单测。
//   - DecodeImageRgba / EncodeRgbaToPng 的**实现**在 .cc（依赖 stb_image / stb_image_write），
//     本头只声明，避免把 stb 实现宏拖进多个 TU。
//
// UV 约定（与 gltf_loader 一致）：glTF TEXCOORD_0 原点 (0,0) = 图片左上角、V 向下；
//   texel (x,y) 的中心 = (u,v) = ((x+0.5)/W, (y+0.5)/H)。覆盖判据 = texel 中心落在 UV 三角形内。

#ifndef JPOV_CLOTHING_BASE_COLOR_ALPHA_H_
#define JPOV_CLOTHING_BASE_COLOR_ALPHA_H_

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include <glog/logging.h>

#include "tools/jpov/interface/mesh.h"  // jpov::MeshData / jpov::Vec2f

namespace jpov {
namespace clothing {

namespace base_color_alpha_detail {

// 三角形有向面积的两倍（z 分量）；符号表示绕序。
inline double TriArea2(const Vec2f& a, const Vec2f& b, const Vec2f& c) {
    return (static_cast<double>(b.x()) - a.x()) * (static_cast<double>(c.y()) - a.y()) -
           (static_cast<double>(b.y()) - a.y()) * (static_cast<double>(c.x()) - a.x());
}

// 点 p 是否在三角形 (a,b,c) 内（含边界；容差 eps 取 texel 空间的小量）。
inline bool PointInTriangle(const Vec2f& p, const Vec2f& a, const Vec2f& b,
                            const Vec2f& c, double eps) {
    const double area = TriArea2(a, b, c);
    if (std::fabs(area) < 1e-12) {
        return false;  // 退化三角形
    }
    const double s = (area > 0.0) ? 1.0 : -1.0;
    return s * TriArea2(a, b, p) >= -eps && s * TriArea2(b, c, p) >= -eps &&
           s * TriArea2(c, a, p) >= -eps;
}

}  // namespace base_color_alpha_detail

// 整图 alpha 归一：把全部 texel 的 alpha 置 255（不透明）。
// Pre-condition（不满足即 LOG(FATAL)）：rgba 非空；width/height > 0；rgba->size() == width*height*4。
inline void NormalizeAlphaWholeImage(int width, int height,
                                     std::vector<unsigned char>* rgba) {
    CHECK(rgba != nullptr);
    CHECK_GT(width, 0);
    CHECK_GT(height, 0);
    CHECK_EQ(rgba->size(), static_cast<size_t>(width) * static_cast<size_t>(height) * 4u)
        << "NormalizeAlphaWholeImage: rgba 尺寸应为 width*height*4";
    for (size_t i = 3; i < rgba->size(); i += 4) {
        (*rgba)[i] = 255u;
    }
}

// 区域 alpha 归一：把被给定 UV 三角形覆盖到的 texel 的 alpha 置 255（不透明）。
//   uv_tris 每 3 个 Vec2f 构成一个三角形（UV 坐标）。空列表 = no-op（不改任何像素）。
// Pre-condition（不满足即 LOG(FATAL)）：rgba 非空；width/height > 0；
//   rgba->size() == width*height*4；uv_tris.size() 是 3 的倍数。
inline void NormalizeAlphaInUvTriangles(const std::vector<Vec2f>& uv_tris, int width,
                                        int height,
                                        std::vector<unsigned char>* rgba) {
    CHECK(rgba != nullptr);
    CHECK_GT(width, 0);
    CHECK_GT(height, 0);
    CHECK_EQ(rgba->size(), static_cast<size_t>(width) * static_cast<size_t>(height) * 4u)
        << "NormalizeAlphaInUvTriangles: rgba 尺寸应为 width*height*4";
    CHECK_EQ(uv_tris.size() % 3, 0u)
        << "NormalizeAlphaInUvTriangles: uv_tris 应为 3 的倍数，got " << uv_tris.size();
    if (uv_tris.empty()) {
        return;  // 无区域：no-op（调用方应据此提示，而不是当作"整图"）
    }

    // UV → texel 空间（u*W、v*H）：texel (x,y) 中心 = (x+0.5, y+0.5)。
    auto to_texel = [width, height](const Vec2f& uv) {
        return Vec2f(uv.x() * static_cast<float>(width),
                     uv.y() * static_cast<float>(height));
    };
    const double eps = 1e-6;  // texel 空间容差（覆盖边界）

    for (size_t t = 0; t + 2 < uv_tris.size(); t += 3) {
        const Vec2f p0 = to_texel(uv_tris[t]);
        const Vec2f p1 = to_texel(uv_tris[t + 1]);
        const Vec2f p2 = to_texel(uv_tris[t + 2]);
        if (std::fabs(base_color_alpha_detail::TriArea2(p0, p1, p2)) < 1e-12) {
            continue;  // 退化三角形：无覆盖
        }
        const float min_x = std::min({p0.x(), p1.x(), p2.x()});
        const float max_x = std::max({p0.x(), p1.x(), p2.x()});
        const float min_y = std::min({p0.y(), p1.y(), p2.y()});
        const float max_y = std::max({p0.y(), p1.y(), p2.y()});
        const int x0 = std::max(0, static_cast<int>(std::floor(min_x)) - 1);
        const int x1 = std::min(width - 1, static_cast<int>(std::ceil(max_x)) + 1);
        const int y0 = std::max(0, static_cast<int>(std::floor(min_y)) - 1);
        const int y1 = std::min(height - 1, static_cast<int>(std::ceil(max_y)) + 1);
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                const Vec2f c(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f);
                if (!base_color_alpha_detail::PointInTriangle(c, p0, p1, p2, eps)) {
                    continue;
                }
                const size_t off = (static_cast<size_t>(y) * static_cast<size_t>(width) +
                                    static_cast<size_t>(x)) * 4u + 3u;
                (*rgba)[off] = 255u;
            }
        }
    }
}

// 收集「三个顶点都被选中」的三角形的 UV（每个三角形按序 push 3 个 Vec2f）。
//   - selected：该 primitive 的选中顶点 index（升序、去重——brush 选区约定）。
//   - mesh 无 UV（flags 不含 kUV 或 uvs 为空）→ 返回空（无法映射到纹理）。
//   - indices 为空 → 按 non-indexed 语义（每 3 个连续顶点一个三角形）。
//   - 只收录三个顶点都在 selected 里的三角形；退化/越界索引跳过。
inline std::vector<Vec2f> CollectSelectedTrianglesUv(
    const jpov::MeshData& mesh, const std::vector<uint32_t>& selected) {
    std::vector<Vec2f> out;
    if (selected.empty() || mesh.uvs.empty() ||
        !jpov::MeshHasFlag(mesh.flags, jpov::MeshVertexFlags::kUV)) {
        return out;
    }
    const size_t vcount = mesh.positions.size();
    auto all_selected = [&selected, vcount](uint32_t i) {
        return i < vcount && std::binary_search(selected.begin(), selected.end(), i);
    };
    auto emit = [&out, &mesh, vcount](uint32_t ia, uint32_t ib, uint32_t ic) {
        if (ia >= vcount || ib >= vcount || ic >= vcount) {
            return;
        }
        out.push_back(mesh.uvs[ia]);
        out.push_back(mesh.uvs[ib]);
        out.push_back(mesh.uvs[ic]);
    };

    if (mesh.indices.empty()) {
        for (size_t t = 0; t + 2 < vcount; t += 3) {
            const uint32_t ia = static_cast<uint32_t>(t);
            const uint32_t ib = static_cast<uint32_t>(t + 1);
            const uint32_t ic = static_cast<uint32_t>(t + 2);
            if (all_selected(ia) && all_selected(ib) && all_selected(ic)) {
                emit(ia, ib, ic);
            }
        }
    } else {
        for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
            const uint32_t ia = mesh.indices[t];
            const uint32_t ib = mesh.indices[t + 1];
            const uint32_t ic = mesh.indices[t + 2];
            if (all_selected(ia) && all_selected(ib) && all_selected(ic)) {
                emit(ia, ib, ic);
            }
        }
    }
    return out;
}

// ==================== 图片编解码（实现见 .cc，依赖 stb） ====================

// 解码一张图（外部路径 uri 或内存编码字节 bytes）为 RGBA8 像素（强制 4 通道）。
//   uri 非空 → 按文件解码；否则用 bytes 从内存解码（内嵌图）。
// 成功：填充 out_rgba（size = w*h*4）、out_w/out_h，返回 true。
// 失败（无图 / 解码失败）：LOG(ERROR) 并返回 false（输出不动）。
// Pre-condition: out_rgba / out_w / out_h 非空；uri 与 bytes 至少一个非空。
bool DecodeImageRgba(const std::string& uri, const std::vector<unsigned char>& bytes,
                     std::vector<unsigned char>* out_rgba, int* out_w, int* out_h);

// 把 RGBA8 像素编码为 PNG 字节流（stbi_write_png_to_func；stride = w*4）。
// 成功返回 true 并填充 out_png；失败 LOG(ERROR) 返回 false。
// Pre-condition: out_png 非空；rgba.size() == w*h*4；w/h > 0。
bool EncodeRgbaToPng(const std::vector<unsigned char>& rgba, int width, int height,
                     std::vector<unsigned char>* out_png);

}  // namespace clothing
}  // namespace jpov

#endif  // JPOV_CLOTHING_BASE_COLOR_ALPHA_H_
