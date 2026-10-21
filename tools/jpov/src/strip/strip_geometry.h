// JPOV 半透明条带：CPU 侧几何展开 + 输入校验（GL-free，可单测）
//
// 本文件只做两件事，都能在没有 GL context 的环境里跑：
//   ① ValidateStripBatch —— 批级/条带级 pre-condition，违规直接 CHECK 崩；
//   ② ExpandStripVertices / AppendStripIndicesForBlock —— 把 Strip16 展开成
//      「32 顶点 + 90 索引」的固定拓扑，供渲染器一次性上传。
//
// 为什么把展开抽出来：这是整条链路里唯一有分支语义的地方（点数塌缩、UV 约定、
// 索引绕序）。放进渲染器就只能靠出图肉眼验；抽出来可以纯 CPU 单测。

#ifndef JPOV_SRC_STRIP_STRIP_GEOMETRY_H_
#define JPOV_SRC_STRIP_STRIP_GEOMETRY_H_

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include <glog/logging.h>

#include "tools/jpov/interface/strip_types.h"

namespace jpov {

// 拓扑常量（共享网格 = 固定 16 列双轨）。
//   顶点：16 列 ×（right, left）= 32
//   四边形：相邻两列成 1 个 quad = 15
//   索引：每个 quad 2 个三角形 = 90
inline constexpr int kStripQuads = kStripSampleCount - 1;
inline constexpr int kStripVerticesPerStrip = 2 * kStripSampleCount;
inline constexpr int kStripIndicesPerStrip = kStripQuads * 6;

// 每列的 u（沿长度方向）是**常量表**：编译期算好，别在每帧每列做一次除法。
// （values: 0/15, 1/15, ..., 15/15）
struct StripColumnU {
    float v[kStripSampleCount];
    constexpr StripColumnU() : v{} {
        for (int i = 0; i < kStripSampleCount; ++i) {
            v[i] = static_cast<float>(i) / static_cast<float>(kStripQuads);
        }
    }
};
inline constexpr StripColumnU kStripColumnU{};

// 一个展开后的条带顶点：位置（世界空间）+ 纹理 uv + 纵向 alpha 乘子。
//
// **UV 约定（Danis 2026-10-21 定，与素材一致）**：
//   u = 沿条带长度方向的 s（第 col 列的 u = col / 15）→ 图片**从左到右**是 s 增长方向；
//   v = 横跨宽度：0 = right（图片底边）、1 = left（图片顶边）。
//   于是四个角对上素材：左下 right0、左上 left0、右下 right15、右上 left15。
//
// 内存布局**刻意**是 6 个连续 float（pos.xyz, u, v, alpha），
// 正好对上着色器的两个属性：loc0 = vec3 位置、loc1 = vec3(u, v, alpha)。
struct StripVertex {
    Vec3f position;
    float u;
    float v;
    float alpha;
};

static_assert(sizeof(StripVertex) == 6 * sizeof(float),
              "StripVertex 必须是紧凑的 6 float（VBO 布局直接按它上传）");

namespace strip_internal {

inline bool IsFinite(float v) {
    return std::isfinite(v);
}

inline bool IsFinite(const Vec3f& v) {
    return IsFinite(v.x()) && IsFinite(v.y()) && IsFinite(v.z());
}

}  // namespace strip_internal

// 批级 + 条带级 pre-condition 校验。违规 CHECK 崩（不静默容错）。
//
// 校验内容：
//   * strips 非空；
//   * 每条 point_count ∈ [2, kStripSampleCount]；
//   * 前 point_count 个点的 left/right 坐标有限（NaN/Inf 一旦进 HDR 会污染整帧后处理）；
//   * 前 point_count 个 alpha ∈ [0,1]，且至少有一个 > 0（防止 memset(0) 的整条隐形）；
//   * width_profile 9 个 knot ∈ [0,1]，且至少有一个 > 0；
//   * tint 的 rgba ∈ [0,1]；
//   * blend 是合法枚举值；soft_depth_m 若启用必须 > 0。
//
// Pre-condition: 无（本函数就是拿来校验调用方输入的）。
inline void ValidateStripBatch(const TranslucentStripBatch& batch) {
    CHECK(!batch.strips.empty()) << "StripBatch: strips 不能为空";

    CHECK(batch.tint.r >= 0.0f && batch.tint.r <= 1.0f &&
          batch.tint.g >= 0.0f && batch.tint.g <= 1.0f &&
          batch.tint.b >= 0.0f && batch.tint.b <= 1.0f &&
          batch.tint.a >= 0.0f && batch.tint.a <= 1.0f)
        << "StripBatch: tint 各分量必须在 [0,1]";

    float profile_max = 0.0f;
    for (int k = 0; k < 9; ++k) {
        const float knot = batch.width_profile[k];
        CHECK(knot >= 0.0f && knot <= 1.0f)
            << "StripBatch: width_profile[" << k << "] 必须在 [0,1]，当前=" << knot;
        profile_max = std::max(profile_max, knot);
    }
    CHECK_GT(profile_max, 0.0f) << "StripBatch: width_profile 全为 0（整批将不可见）";

    // 枚举防御：别让非法 cast 值落到「两个集合都不写」的静默路径。
    CHECK(batch.blend == StripBlend::kCoverage || batch.blend == StripBlend::kEmissive)
        << "StripBatch: blend 非法值 " << static_cast<int>(batch.blend);
    if (batch.soft_depth_m.has_value()) {
        CHECK_GT(*batch.soft_depth_m, 0.0f)
            << "StripBatch: soft_depth_m 若启用必须 > 0（想关闭请置 nullopt）";
    }

    for (size_t i = 0; i < batch.strips.size(); ++i) {
        const Strip16& s = batch.strips[i];
        CHECK_GE(s.point_count, 2u)
            << "Strip16[" << i << "]: point_count 至少 2（未填写时为 0）";
        CHECK_LE(s.point_count, static_cast<uint32_t>(kStripSampleCount))
            << "Strip16[" << i << "]: point_count 不得大于 " << kStripSampleCount;

        float alpha_max = 0.0f;
        for (uint32_t k = 0; k < s.point_count; ++k) {
            CHECK(strip_internal::IsFinite(s.left[k]) && strip_internal::IsFinite(s.right[k]))
                << "Strip16[" << i << "]: 第 " << k << " 个采样点坐标非有限值";
            const float a = s.alpha[k];
            CHECK(a >= 0.0f && a <= 1.0f)
                << "Strip16[" << i << "]: alpha[" << k << "] 必须在 [0,1]，当前=" << a;
            alpha_max = std::max(alpha_max, a);
        }
        CHECK_GT(alpha_max, 0.0f)
            << "Strip16[" << i << "]: 前 point_count 个 alpha 全为 0（该条带不可见）";
    }
}

// 把一条 Strip16 展开成 kStripVerticesPerStrip 个顶点，**追加**到 out。
//
// 塌缩规则：列 col 取采样点 src = min(col, point_count-1)。
// 于是不足 16 点时，尾部列全部坐在最后一个有效点 → 零面积 quad → 不产生片元。
// 注意塌缩**不改变 UV**：u 恒为 col/15（纹理由「16 格」约定沿长度等分），
// 所以短条带只用到纹理前 (point_count-1)/15 的一段（不拉伸图像）。
//
// Pre-condition: 调用方已跑过 ValidateStripBatch（本函数不做重复校验）。
inline void ExpandStripVertices(const Strip16& strip, std::vector<StripVertex>* out) {
    CHECK(out != nullptr) << "ExpandStripVertices: out 不能为空";
    // point_count == 0 会让下面的 point_count - 1 下溢成 4G，属于内存安全边界，
    // 这里再兜一次（批量路径已由 ValidateStripBatch 保证，本函数是公开入口）。
    CHECK_GE(strip.point_count, 2u) << "ExpandStripVertices: point_count 至少 2";
    CHECK_LE(strip.point_count, static_cast<uint32_t>(kStripSampleCount));

    const uint32_t last = strip.point_count - 1;
    for (int col = 0; col < kStripSampleCount; ++col) {
        const uint32_t src = (static_cast<uint32_t>(col) < strip.point_count)
                                 ? static_cast<uint32_t>(col)
                                 : last;
        // u = 沿长度（图片从左到右），v = 横跨宽度（0 = right / 1 = left）。
        const float u = kStripColumnU.v[col];
        // 先 right（v=0）后 left（v=1）——与索引模式严格对应。
        out->push_back(StripVertex{strip.right[src], u, 0.0f, strip.alpha[src]});
        out->push_back(StripVertex{strip.left[src], u, 1.0f, strip.alpha[src]});
    }
}

// 追加**一个条带块**的索引（90 个）；base_vertex = 该条带首个顶点的全局下标。
// 绕序：quad q 由顶点 (2q, 2q+1, 2q+2, 2q+3) = (right_q, left_q, right_{q+1}, left_{q+1})
// 组成，两个三角形 (0,1,2) + (1,3,2)。渲染时关剔除，故绕序不影响可见性，
// 但保持**一致**才能让将来开剔除时行为可预期。
inline void AppendStripIndicesForBlock(uint32_t base_vertex, std::vector<uint32_t>* out) {
    CHECK(out != nullptr) << "AppendStripIndicesForBlock: out 不能为空";
    for (int q = 0; q < kStripQuads; ++q) {
        const uint32_t r0 = base_vertex + static_cast<uint32_t>(2 * q);
        const uint32_t l0 = r0 + 1;
        const uint32_t r1 = r0 + 2;
        const uint32_t l1 = r0 + 3;
        out->push_back(r0);
        out->push_back(l0);
        out->push_back(r1);
        out->push_back(l0);
        out->push_back(l1);
        out->push_back(r1);
    }
}

}  // namespace jpov

#endif  // JPOV_SRC_STRIP_STRIP_GEOMETRY_H_
