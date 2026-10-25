// JPOV 半透明条带：CPU 侧几何展开 + 输入校验（GL-free，可单测）
//
// 本文件只做两件事，都能在没有 GL context 的环境里跑：
//   ① ValidateStripBatch —— 批级/条带级 pre-condition，违规直接 CHECK 崩；
//   ② ExpandStripVertices / AppendStripIndicesForBlock —— 把 Strip16 展开成
//      「32 顶点 + 90 索引」的固定拓扑，供渲染器一次性上传。
//
// 为什么把展开抽出来：这是整条链路里唯一有分支语义的地方（点数塌缩、UV 约定、
// 索引绕序）。放进渲染器就只能靠出图肉眼验；抽出来可以纯 CPU 单测。
//
// 2026-10-22 追加：平滑 pass（kSmooth）的 CPU 前端也放在本文件
// （ComputeSmoothStrip / AppendSmoothStripData / AppendSmoothStripVertices）——
// 同样是纯数学，同样必须能单测（外扩量算少了只表现为「外侧缺一条亚像素」，出图看不出）。

#ifndef JPOV_SRC_STRIP_STRIP_GEOMETRY_H_
#define JPOV_SRC_STRIP_STRIP_GEOMETRY_H_

#include <algorithm>
#include <array>
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

// 一个展开后的条带顶点：位置（世界空间）+ 纹理 uv。
//
// **UV 约定（Danis 2026-10-21 定，与素材一致）**：
//   u = 沿条带长度方向的 s（第 col 列的 u = col / 15）→ 图片**从左到右**是 s 增长方向；
//   v = 横跨宽度：0 = right（图片底边）、1 = left（图片顶边）。
//   于是四个角对上素材：左下 right0、左上 left0、右下 right15、右上 left15。
//
// 内存布局**刻意**是 5 个连续 float（pos.xyz, u, v），
// 正好对上着色器的两个属性：loc0 = vec3 位置、loc1 = vec2(u, v)。
// 纵向 alpha 不在顶点里（2026-10-22 重构）：它并进了逐点颜色 `point_color[i].a`，
// 由 FS 按 u 从 per-instance 纹理里采样 —— 顶点属性因此从 6 float 降到 5 float。
struct StripVertex {
    Vec3f position;
    float u;
    float v;
};

static_assert(sizeof(StripVertex) == 5 * sizeof(float),
              "StripVertex 必须是紧凑的 5 float（VBO 布局直接按它上传）");

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
//   * strips 非空；blend / geometry 是合法枚举值；soft_depth_m 若启用必须 > 0；
//   * 每条 point_count ∈ [2, kStripSampleCount]；
//   * 前 point_count 个点的 left/right 坐标有限（NaN/Inf 一旦进 HDR 会污染整帧后处理）；
//   * 前 point_count 个逐点颜色：rgb 有限、∈ [0, kMaxStripColorValue]（**允许 HDR**）、
//     a ∈ [0,1]，且至少有一个 a > 0（防止 memset(0) 的整条隐形）；
//   * 每条 width_profile 的 9 个 knot ∈ [0,1]，且至少有一个 > 0；
//   * fresnel 各参数在合法范围。
//
// Pre-condition: 无（本函数就是拿来校验调用方输入的）。
inline void ValidateStripBatch(const TranslucentStripBatch& batch) {
    CHECK(!batch.strips.empty()) << "StripBatch: strips 不能为空";

    // 枚举防御：别让非法 cast 值落到「两个集合都不写」的静默路径。
    CHECK(batch.blend == StripBlend::kCoverage || batch.blend == StripBlend::kEmissive)
        << "StripBatch: blend 非法值 " << static_cast<int>(batch.blend);
    // geometry 同理：非法 cast 值会静默走折线 pass（用户以为开了平滑却不生效）。
    CHECK(batch.geometry == StripInterpolation::kPolyline ||
          batch.geometry == StripInterpolation::kSmooth)
        << "StripBatch: geometry 非法值 " << static_cast<int>(batch.geometry);
    if (batch.soft_depth_m.has_value()) {
        CHECK_GT(*batch.soft_depth_m, 0.0f)
            << "StripBatch: soft_depth_m 若启用必须 > 0（想关闭请置 nullopt）";
    }
    if (batch.fresnel.has_value()) {
        const StripFresnel& f = *batch.fresnel;
        CHECK(f.f0 >= 0.0f && f.f0 <= 1.0f)
            << "StripBatch: fresnel.f0 必须在 [0,1]，当前=" << f.f0;
        // 上限不是物理约束，是防御：pow(1−cos, 64) 已低到无法在 8bit 输出上看出来，
        // 再大只会吃掉 pow() 精度（NaN 也会在这里被 finite 检查拦下）。
        CHECK(f.power > 0.0f && f.power <= 64.0f)
            << "StripBatch: fresnel.power 必须在 (0,64]，当前=" << f.power;
        CHECK(f.strength >= 0.0f)
            << "StripBatch: fresnel.strength 不能为负，当前=" << f.strength;
        CHECK(f.graze_alpha >= 0.0f && f.graze_alpha <= 1.0f)
            << "StripBatch: fresnel.graze_alpha 必须在 [0,1]，当前=" << f.graze_alpha;
        CHECK(strip_internal::IsFinite(f.rim_color) && f.rim_color.x() >= 0.0f &&
              f.rim_color.y() >= 0.0f && f.rim_color.z() >= 0.0f)
            << "StripBatch: fresnel.rim_color 必须是有限非负值";
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
            const Color& c = s.point_color[k];
            // rgb 允许 HDR（> 1 = 发光/加法集的亮度），但必须有限、非负、不超过
            // 防御上限；越界比较对 NaN 恒为 false ⇒ NaN 也会在这里被拦下。
            CHECK(c.r >= 0.0f && c.r <= kMaxStripColorValue && c.g >= 0.0f &&
                  c.g <= kMaxStripColorValue && c.b >= 0.0f && c.b <= kMaxStripColorValue)
                << "Strip16[" << i << "]: point_color[" << k << "].rgb 必须在 [0,"
                << kMaxStripColorValue << "]，当前=(" << c.r << "," << c.g << "," << c.b
                << ")";
            CHECK(c.a >= 0.0f && c.a <= 1.0f)
                << "Strip16[" << i << "]: point_color[" << k << "].a 必须在 [0,1]，当前="
                << c.a;
            alpha_max = std::max(alpha_max, c.a);
        }
        CHECK_GT(alpha_max, 0.0f)
            << "Strip16[" << i << "]: 前 point_count 个点的 point_color.a 全为 0"
               "（该条带不可见）";

        float profile_max = 0.0f;
        for (int k = 0; k < kStripProfileKnots; ++k) {
            const float knot = s.width_profile[static_cast<size_t>(k)];
            CHECK(knot >= 0.0f && knot <= 1.0f)
                << "Strip16[" << i << "]: width_profile[" << k
                << "] 必须在 [0,1]，当前=" << knot;
            profile_max = std::max(profile_max, knot);
        }
        CHECK_GT(profile_max, 0.0f)
            << "Strip16[" << i << "]: width_profile 全为 0（该条带不可见）";
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
        out->push_back(StripVertex{strip.right[src], u, 0.0f});
        out->push_back(StripVertex{strip.left[src], u, 1.0f});
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

// ────────────────── 圆环/圆筒：四组点 → 四条带 ──────────────────
//
// 用途：用条带围出一个「玻璃圆筒」（Demo/验收 fresnel 用）。一个圆筒的外壳
// 正好是 4 条带，**四条带共用同四组点**（the 16 点拓扑天生就是「一圈」）：
//
//   up_outer / up_inner / down_outer / down_inner  ← 四组点，每组 16 个（绕一圈，
//   第 15 列与第 0 列重合 = 闭合，故一圈是 15 段）
//
//   ① 顶环面：left = up_outer，  right = up_inner    （u 绕圈，v 跨环宽）
//   ② 底环面：left = down_outer，right = down_inner
//   ③ 外立壁：left = up_outer，  right = down_outer  （u 绕圈，v 跨高度）
//   ④ 内立壁：left = up_inner，  right = down_inner
//
// 注意立壁的「长度方向」也是**绕圈**（16 列），宽度方向才是高度 —— 极容易想成
// 「高度分 16 段」，那样四条带的列数就对不上了（也画不出闭环）。

// 一个圆环的四组点（顺序固定，见上）。
struct Ring16 {
    std::array<Vec3f, kStripSampleCount> up_outer;
    std::array<Vec3f, kStripSampleCount> up_inner;
    std::array<Vec3f, kStripSampleCount> down_outer;
    std::array<Vec3f, kStripSampleCount> down_inner;
};

// 圆环形状参数（米/弧度，世界空间）。
struct StripRingShape {
    float outer_radius = 0.95f;   // 外半径
    float wall_thickness = 0.15f; // 壁厚（必须 0 < 壁厚 < outer_radius）
    float height = 1.25f;         // 圆筒高度
    float center_y = 1.05f;       // 圆筒中心的 y
    float center_x = 0.0f;        // 中心位置的 x/z（便于在场景里挪开）
    float center_z = 0.0f;
    float yaw = 0.0f;             // 绕 Y 轴朝向（弧度），不影响几何形状
};

// 四条带在本文件/调用方之间的固定下标（= BuildStripRing 的输出顺序）。
enum class StripRingPart : int {
    kTopFace = 0,
    kBottomFace = 1,
    kOuterWall = 2,
    kInnerWall = 3,
};
inline constexpr int kStripRingPartCount = 4;

// 由形状参数生成四组点。
// Pre-condition: outer_radius > 0、0 < wall_thickness < outer_radius、height > 0（CHECK 崩）。
inline Ring16 MakeRing16(const StripRingShape& shape) {
    CHECK_GT(shape.outer_radius, 0.0f) << "MakeRing16: outer_radius 必须 > 0";
    CHECK_GT(shape.wall_thickness, 0.0f) << "MakeRing16: wall_thickness 必须 > 0";
    CHECK_LT(shape.wall_thickness, shape.outer_radius)
        << "MakeRing16: wall_thickness 必须小于 outer_radius（否则内半径 ≤ 0）";
    CHECK_GT(shape.height, 0.0f) << "MakeRing16: height 必须 > 0";

    const float inner_radius = shape.outer_radius - shape.wall_thickness;
    Ring16 ring;
    const float y_up = shape.center_y + 0.5f * shape.height;
    const float y_down = shape.center_y - 0.5f * shape.height;
    const float cy = std::cos(shape.yaw);
    const float sy = std::sin(shape.yaw);
    for (int i = 0; i < kStripSampleCount; ++i) {
        // 第 15 列取 2π（= 第 0 列），保证闭合；一圈分 15 段。
        const float ang = 2.0f * 3.14159265358979323846f * static_cast<float>(i) /
                          static_cast<float>(kStripQuads);
        const float c = std::cos(ang);
        const float s = std::sin(ang);
        // 先绕 Y 轴转 yaw，再平移到 (center_x, ·, center_z)。
        auto at = [&](float radius, float y) {
            const float px = radius * c;
            const float pz = radius * s;
            return Vec3f{px * cy + pz * sy + shape.center_x, y,
                         -px * sy + pz * cy + shape.center_z};
        };
        ring.up_outer[static_cast<size_t>(i)] = at(shape.outer_radius, y_up);
        ring.up_inner[static_cast<size_t>(i)] = at(inner_radius, y_up);
        ring.down_outer[static_cast<size_t>(i)] = at(shape.outer_radius, y_down);
        ring.down_inner[static_cast<size_t>(i)] = at(inner_radius, y_down);
    }
    return ring;
}

// 由四组点拼出四条带（下标 = StripRingPart）。逐点颜色取默认值（白 + 不透明），
// point_count = kStripSampleCount（整圈都要画）；颜色/剖面由调用方按批或按实例填。
inline std::array<Strip16, kStripRingPartCount> BuildStripRing(const Ring16& ring) {
    std::array<Strip16, kStripRingPartCount> strips{};
    for (int part = 0; part < kStripRingPartCount; ++part) {
        strips[static_cast<size_t>(part)].point_count =
            static_cast<uint32_t>(kStripSampleCount);
    }
    for (int i = 0; i < kStripSampleCount; ++i) {
        const size_t k = static_cast<size_t>(i);
        Strip16& top = strips[static_cast<size_t>(StripRingPart::kTopFace)];
        Strip16& bottom = strips[static_cast<size_t>(StripRingPart::kBottomFace)];
        Strip16& outer = strips[static_cast<size_t>(StripRingPart::kOuterWall)];
        Strip16& inner = strips[static_cast<size_t>(StripRingPart::kInnerWall)];
        top.left[k] = ring.up_outer[k];
        top.right[k] = ring.up_inner[k];
        bottom.left[k] = ring.down_outer[k];
        bottom.right[k] = ring.down_inner[k];
        outer.left[k] = ring.up_outer[k];
        outer.right[k] = ring.down_outer[k];
        inner.left[k] = ring.up_inner[k];
        inner.right[k] = ring.down_inner[k];
    }
    return strips;
}

// 便捷入口：形状 → 四条带。
inline std::array<Strip16, kStripRingPartCount> MakeStripRing(
    const StripRingShape& shape) {
    return BuildStripRing(MakeRing16(shape));
}

// ────────────────── 平滑 pass（kSmooth）的 CPU 前端 ──────────────────
//
// 折线 pass 把 left/right 当**顶点**（段间是弦）；平滑 pass 把它们当**插值控制点**
// （段间是 C1 Hermite，FS 里按真实曲线反解参数并裁形）。设计见
// docs/jpov_strip3d_translucent_design.md §18。本段只放**纯数学**（GL-free）：
//   ① ComputeSmoothStrip        —— 中心/半宽矢量 + 切线 + 自动外扩量（整条带统一 scale）；
//   ② AppendSmoothStripData     —— 数据纹理的一行（每列 16 float，含逐点颜色）；
//   ③ AppendSmoothStripVertices —— 顶点（每顶点 6 float）。
// 渲染器只负责上传与 draw。
//
// 为什么必须能单测：外扩量算少了只表现为「弯的外侧缺一条亚像素」，出图肉眼看不出来
// （现有 demo 的三条带恰好都没走到会出问题的那种分支）。

// 平滑 pass 的顶点：pos(3) + u + v + strip_id。
// （纵向 alpha 已并进逐点颜色，由 FS 采样；顶点不再带 alpha。）
inline constexpr int kSmoothFloatsPerVertex = 6;

// 控制点数据纹理：每列 4 个 RGBA32F texel = 16 float = c.xyz, m.xyz, h.xyz, n.xyz + 逐点 rgba。
//
// 为什么把逐点颜色也塞进来（2026-10-25）：平滑 pass 本来就在这同一行里逐段取两次控制点，
// 把每列的颜色作为第二个 texel 一并取回（同一条 cache line），FS 用段内参数 t* 自己 mix，
// 于是那一次对 per-instance 纹理的**过滤采样**可以完全去掉（省一次 bilinear + LOD 计算）；
// 硬件双线性在列间也是线性插值 ⇒ 两条路的数学等价（仅浮点舍入不同）。
inline constexpr int kSmoothTexelsPerColumn = 4;
inline constexpr int kSmoothFloatsPerColumn = 4 * kSmoothTexelsPerColumn;

// 每段用来求「最大外凸量」的采样点数（t = i/(n+1)，i = 1..n）：最值近似 + 末尾余量。
inline constexpr int kSmoothMarginSamplesPerSegment = 7;

// 外扩倍数上限（相对最细一列的半宽）。再细的尖通常已是亚像素，不值得为它铺满屏幕。
inline constexpr float kSmoothMaxWidenScale = 8.0f;

namespace smooth_internal {

// 加减/数乘/点积用 geom::Vec3 自带的运算符与 Dot() 成员，这里只补 Hermite 需要的两个。
inline Vec3f Lerp(const Vec3f& a, const Vec3f& b, float t) {
    return Vec3f{a.x() + (b.x() - a.x()) * t, a.y() + (b.y() - a.y()) * t,
                 a.z() + (b.z() - a.z()) * t};
}

// 三次 Hermite（m0/m1 是 ∂P/∂t，t ∈ [0,1]）。
// **必须与 kSmoothFsMain 里的求值逐项一致**：CPU 用它算外扩量，FS 用它重建曲面。
inline Vec3f Hermite(const Vec3f& p0, const Vec3f& m0, const Vec3f& p1,
                     const Vec3f& m1, float t) {
    const float t2 = t * t;
    const float t3 = t2 * t;
    const float h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;
    const float h10 = t3 - 2.0f * t2 + t;
    const float h01 = -2.0f * t3 + 3.0f * t2;
    const float h11 = t3 - t2;
    return Vec3f{h00 * p0.x() + h10 * m0.x() + h01 * p1.x() + h11 * m1.x(),
                 h00 * p0.y() + h10 * m0.y() + h01 * p1.y() + h11 * m1.y(),
                 h00 * p0.z() + h10 * m0.z() + h01 * p1.z() + h11 * m1.z()};
}

}  // namespace smooth_internal

// 一条条带的平滑前端结果（纯数据）。
//
// 下标 col ∈ [0, kStripSampleCount)：不足 16 点时，尾部列与折线 pass 一样**塌缩**
// 到最后一个有效点（重复的列 = 零面积 quad，不产生片元）。
struct SmoothStripCompute {
    Vec3f center[kStripSampleCount];      // 过点（该列 left/right 的中点）
    Vec3f half[kStripSampleCount];        // 半宽矢量（**未外扩**，指向 left 一侧）
    Vec3f center_tan[kStripSampleCount];  // ∂center/∂t
    Vec3f half_tan[kStripSampleCount];    // ∂half/∂t
    float margin = 0.0f;                  // 需要外扩的绝对量（米），含 2% + 1mm 余量
    float scale = 1.0f;                   // 顶点外扩倍数（整条带统一，≥ 1）
};

// 计算一条条带的中心线 / 半宽矢量 / 切线与自动外扩倍数。
//
// Pre-condition: point_count ∈ [2, kStripSampleCount]（ValidateStripBatch 已保证）。
inline void ComputeSmoothStrip(const Strip16& strip, SmoothStripCompute* out /*output*/) {
    CHECK(out != nullptr) << "ComputeSmoothStrip: out 不能为空";
    CHECK_GE(strip.point_count, 2u) << "ComputeSmoothStrip: point_count 至少 2";
    CHECK_LE(strip.point_count, static_cast<uint32_t>(kStripSampleCount))
        << "ComputeSmoothStrip: point_count 不得大于 " << kStripSampleCount;

    // ① 控制点：中心 c（Hermite 的过点）与半宽矢量 h（未外扩）。
    const uint32_t last_real = strip.point_count - 1;
    for (int col = 0; col < kStripSampleCount; ++col) {
        const uint32_t src = std::min(static_cast<uint32_t>(col), last_real);
        const Vec3f& l = strip.left[src];
        const Vec3f& r = strip.right[src];
        out->center[col] = (l + r) * 0.5f;
        out->half[col] = (l - r) * 0.5f;
    }

    // ② 切线：内部用中心差分（相邻段共用同一列切线 ⇒ C1 的来源），
    //    **两端用完整的单侧差分**（权重 1）。
    //
    //    两端若也按 0.5 写，2 点条带的 Hermite 会被砍成"半速"参数化：曲线仍是直线，
    //    但 u 沿长度被不均匀拉伸（实测 16 点直条带端段偏 5.5% 段长、2 点条带偏 4%），
    //    而且「2 点 kSmooth == kPolyline」这个数学事实不成立了。
    const int last_real_i = static_cast<int>(last_real);
    for (int col = 0; col < kStripSampleCount; ++col) {
        const int c = std::min(col, last_real_i);   // 尾部塌缩列与最后一个有效列同切线
        const int im = std::max(c - 1, 0);
        const int ip = std::min(c + 1, last_real_i);
        const float w = (ip - im == 2) ? 0.5f : 1.0f;
        out->center_tan[col] = (out->center[ip] - out->center[im]) * w;
        out->half_tan[col] = (out->half[ip] - out->half[im]) * w;
    }

    // ③ 自动外扩量：真实曲线相对弦在**宽度方向上**的最大外凸量，按**绝对量（米）**算
    //    （拖尾收窄到 0 的段落用比例会失真）。片元只能削不能补 ⇒ 撑不够就少一条边。
    //
    //    ⚠️ 这里必须用**完整 Hermite** 求 sh，不能因为「本段 h0 == h1」就简化成常数 h0：
    //    FS 那边恒算完整 Hermite（含 h10·n0 + h11·n1），而 n 只看相邻列 ——
    //    局部等宽、但隔壁列在收窄（"肩部"）时 n ≠ 0，简化的 margin 会低估外凸量，
    //    收窄处外侧就缺一条（实测：半宽 0.2 的直条带、末列收到 0，两侧各缺 4.25 mm）。
    float margin = 0.0f;
    for (int seg = 0; seg < kStripQuads; ++seg) {
        for (int si = 1; si <= kSmoothMarginSamplesPerSegment; ++si) {
            const float t = static_cast<float>(si) /
                            static_cast<float>(kSmoothMarginSamplesPerSegment + 1);
            const Vec3f lc =
                smooth_internal::Lerp(out->center[seg], out->center[seg + 1], t);
            const Vec3f lh = smooth_internal::Lerp(out->half[seg], out->half[seg + 1], t);
            const float len = lh.Norm();
            if (len < 1e-5f) {
                continue;   // 退化段（宽度为 0）：没有"外侧"可言
            }
            const Vec3f w = lh * (1.0f / len);
            const Vec3f sc = smooth_internal::Hermite(
                out->center[seg], out->center_tan[seg], out->center[seg + 1],
                out->center_tan[seg + 1], t);
            const Vec3f sh = smooth_internal::Hermite(
                out->half[seg], out->half_tan[seg], out->half[seg + 1],
                out->half_tan[seg + 1], t);
            const float dl = (sc - lc).Dot(w);
            const float dhw = (sh - lh).Dot(w);
            // 左轨（v=1）外法向 = +w；右轨（v=0）外法向 = −w。
            margin = std::max(margin, std::max(dl + dhw, -dl + dhw));
        }
    }
    // 采样是最值近似，再留 2% + 1 mm 余量：宁可多撑一点（FS 会裁回去）。
    margin = std::max(margin, 0.0f) * 1.02f + 0.001f;

    // 统一 scale：按**最细**的一列算，保证每一列都撑得住；上限 kSmoothMaxWidenScale。
    // 为什么必须整条带统一（不能逐列）：FS 假定 position = lerp(c) + (2v−1)·lerp(h)，
    // 只有 scale 恒定才有 lerp(scale·h) == scale·lerp(h)；逐列不同会在收窄处露出缺口。
    float min_half = 1e30f;
    for (int col = 0; col < kStripSampleCount; ++col) {
        const float hl = out->half[col].Norm();
        if (hl > 1e-5f) {
            min_half = std::min(min_half, hl);
        }
    }
    out->margin = margin;
    out->scale = (min_half < 1e30f)
                     ? std::min(1.0f + margin / min_half, kSmoothMaxWidenScale)
                     : 1.0f;
}

// 追加一条条带的数据纹理行：kStripSampleCount × 16 float。
//
// 打包约定（**与 kSmoothFsMain 的解包逐字对应**，改一边必须改另一边）：
//   每列 16 float = c.xyz, m.xyz, h.xyz, n.xyz, point_color.rgba
//   → 每 4 个 texel 一组：(c.xyz, m.x)(m.yz, h.xy)(h.z, n.xyz)(rgba)
//
// 颜色用塌缩后的列（与几何一致）：尾部列与最后一个有效列同色。这些列构成零面积
// quad、不产生片元，所以取什么值对画面没影响，但**与几何用同一个 src 下标**可以让
// 「第 i 列的颜色 = 第 i 列的几何」这个不变量在整张纹理上成立，读代码时不用再分叉。
inline void AppendSmoothStripData(const Strip16& strip,
                                  const SmoothStripCompute& computed,
                                  std::vector<float>* out /*output*/) {
    CHECK(out != nullptr) << "AppendSmoothStripData: out 不能为空";
    CHECK_GE(strip.point_count, 2u) << "AppendSmoothStripData: point_count 至少 2";
    CHECK_LE(strip.point_count, static_cast<uint32_t>(kStripSampleCount));
    const uint32_t last_real = strip.point_count - 1;
    for (int col = 0; col < kStripSampleCount; ++col) {
        const Vec3f& c = computed.center[col];
        const Vec3f& m = computed.center_tan[col];
        const Vec3f& h = computed.half[col];
        const Vec3f& n = computed.half_tan[col];
        const Color& color =
            strip.point_color[std::min(static_cast<uint32_t>(col), last_real)];
        out->push_back(c.x());
        out->push_back(c.y());
        out->push_back(c.z());
        out->push_back(m.x());
        out->push_back(m.y());
        out->push_back(m.z());
        out->push_back(h.x());
        out->push_back(h.y());
        out->push_back(h.z());
        out->push_back(n.x());
        out->push_back(n.y());
        out->push_back(n.z());
        out->push_back(color.r);
        out->push_back(color.g);
        out->push_back(color.b);
        out->push_back(color.a);
    }
}

// 追加一条条带的顶点：kStripVerticesPerStrip × 6 float，列内**先 right 后 left**
// （与 ExpandStripVertices 的顶点顺序严格一致 —— 两个 pass 共用同一份静态索引）。
//
// 外扩沿半宽矢量做，并把外扩量**折进属性 v**（右 = 0.5 − scale/2、左 = 0.5 + scale/2），
// 于是 FS 里插值出来的 v 就是真实参数：position = lerp(c) + (2v−1)·lerp(h)。
inline void AppendSmoothStripVertices(const Strip16& strip,
                                      const SmoothStripCompute& computed,
                                      float strip_id,
                                      std::vector<float>* out /*output*/) {
    CHECK(out != nullptr) << "AppendSmoothStripVertices: out 不能为空";
    CHECK_GE(strip.point_count, 2u) << "AppendSmoothStripVertices: point_count 至少 2";
    CHECK_LE(strip.point_count, static_cast<uint32_t>(kStripSampleCount));

    const uint32_t last_real = strip.point_count - 1;
    const float v_right = 0.5f - 0.5f * computed.scale;
    const float v_left = 0.5f + 0.5f * computed.scale;
    for (int col = 0; col < kStripSampleCount; ++col) {
        const uint32_t src = std::min(static_cast<uint32_t>(col), last_real);
        const float u = kStripColumnU.v[col];
        const Vec3f& center = computed.center[col];
        for (int side = 0; side < 2; ++side) {
            const Vec3f& edge = (side == 0) ? strip.right[src] : strip.left[src];
            out->push_back(center.x() + (edge.x() - center.x()) * computed.scale);
            out->push_back(center.y() + (edge.y() - center.y()) * computed.scale);
            out->push_back(center.z() + (edge.z() - center.z()) * computed.scale);
            out->push_back(u);
            out->push_back(side == 0 ? v_right : v_left);
            out->push_back(strip_id);
        }
    }
}

}  // namespace jpov

#endif  // JPOV_SRC_STRIP_STRIP_GEOMETRY_H_
