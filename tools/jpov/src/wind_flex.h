// JPOV 植被风 — 顶点标注 (bend, phase) 的派生（GL-free 纯函数）
//
// 作用：为一份**没有烘风数据**的网格（如外部 GLB）派生每顶点的风标注，写进
//   MeshData.wind_flex（并置 kWind flag）。见 docs/jpov_vegetation_wind_design.md §5。
//
//   bend  = clamp(|pos - base| / scale, 0, 1)   —— 离树基越远越软（基部≈0，梢部≈1）
//   phase = HashPhase(x,y,z) ∈ [0,1)（**秒**）  —— 确定性去同步（梢部滞后 + 叶丛错拍）
//
// 说明：
//   - **无层级**情况下的合理代理：整株基座硬、外围软。对小柳树（下垂枝、梢远）尤其合适。
//   - `base` 取包围盒「底面中心」，`scale` 取最长边 → 对整株尺度归一。
//   - phase 用**确定性哈希**（非随机数），保证同资产逐次一致（gold 友好）。

#ifndef JPOV_SRC_WIND_FLEX_H_
#define JPOV_SRC_WIND_FLEX_H_

#include <algorithm>
#include <cmath>
#include <cstdint>

#include <glog/logging.h>

#include "tools/jpov/interface/mesh.h"

namespace jpov {

// 树基与尺度（整株统一，逐 primitive 共用同一份，保证 trunk/leaves 一致）。
struct WindFlexParams {
    Vec3f base = {0.0f, 0.0f, 0.0f};   // 树基（底面中心）
    float scale = 1.0f;                // 特征尺度（最长边；必须 > 0）
};

// 由整株合并包围盒推出 base / scale。
//   base  = (盒中心 x, 盒底 y, 盒中心 z)
//   scale = max(三边长)
// Pre-condition: bounds_max 各分量 >= bounds_min（否则 CHECK）。
inline WindFlexParams WindFlexParamsFromBounds(const Vec3f& bounds_min,
                                               const Vec3f& bounds_max) {
    CHECK_GE(bounds_max.x(), bounds_min.x());
    CHECK_GE(bounds_max.y(), bounds_min.y());
    CHECK_GE(bounds_max.z(), bounds_min.z());
    WindFlexParams p;
    p.base = Vec3f(0.5f * (bounds_min.x() + bounds_max.x()), bounds_min.y(),
                   0.5f * (bounds_min.z() + bounds_max.z()));
    const float ex = bounds_max.x() - bounds_min.x();
    const float ey = bounds_max.y() - bounds_min.y();
    const float ez = bounds_max.z() - bounds_min.z();
    p.scale = std::max(ex, std::max(ey, ez));
    CHECK_GT(p.scale, 0.0f) << "WindFlexParamsFromBounds: 退化包围盒（scale=0）";
    return p;
}

// 确定性空间哈希 → [0,1)。同坐标恒同值；用于 phase 去同步。
inline float HashPhase01(float x, float y, float z) {
    // 量化到毫米级，避免浮点抖动造成同点异值。
    const uint32_t ix = static_cast<uint32_t>(static_cast<int32_t>(std::lround(x * 1000.0f)));
    const uint32_t iy = static_cast<uint32_t>(static_cast<int32_t>(std::lround(y * 1000.0f)));
    const uint32_t iz = static_cast<uint32_t>(static_cast<int32_t>(std::lround(z * 1000.0f)));
    uint32_t h = ix * 73856093u ^ iy * 19349663u ^ iz * 83492791u;
    h ^= h >> 13;
    h *= 0x85ebca6bu;
    h ^= h >> 16;
    h *= 0xc2b2ae35u;
    h ^= h >> 16;
    return static_cast<float>(h & 0x00FFFFFFu) / 16777216.0f;   // [0,1)
}

// 就地派生：写 mesh->wind_flex 并置 kWind flag。
// Pre-condition: mesh 非空；mesh->positions 非空；p.scale > 0。
// 若 mesh 已有 kRelax（布料），拒绝（二者互斥）—— 见 mesh.h。
inline void DeriveWindFlex(const WindFlexParams& p, MeshData* mesh) {
    CHECK(mesh != nullptr);
    CHECK_GT(p.scale, 0.0f);
    CHECK(!mesh->positions.empty());
    CHECK(!MeshHasFlag(mesh->flags, MeshVertexFlags::kRelax))
        << "DeriveWindFlex: mesh 已声明 kRelax（布料），与 kWind 互斥";

    const size_t n = mesh->positions.size();
    mesh->wind_flex.resize(n);
    for (size_t i = 0; i < n; ++i) {
        const Vec3f& v = mesh->positions[i];
        const float dx = v.x() - p.base.x();
        const float dy = v.y() - p.base.y();
        const float dz = v.z() - p.base.z();
        const float d = std::sqrt(dx * dx + dy * dy + dz * dz) / p.scale;
        const float bend = (d < 0.0f) ? 0.0f : ((d > 1.0f) ? 1.0f : d);
        const float phase = HashPhase01(v.x(), v.y(), v.z());   // 秒，[0,1)
        mesh->wind_flex[i] = Vec2f(bend, phase);
    }
    mesh->flags = static_cast<MeshVertexFlags>(
        static_cast<uint8_t>(mesh->flags) |
        static_cast<uint8_t>(MeshVertexFlags::kWind));
}

}  // namespace jpov

#endif  // JPOV_SRC_WIND_FLEX_H_
