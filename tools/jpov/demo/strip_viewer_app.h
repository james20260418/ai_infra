// JPOV 半透明条带查看器（strip viewer）— 渲染核心 App
//
// 场景 = 天光 + 灰色地面 + 四块参考方块 + **一批三条带（一次 draw call）**
//        + **玻璃圆环（四组点 → 四条带，另一个 batch）**：
//   ① 波带（宽、柔和边缘、带纵向淡出）
//   ② 螺旋拖尾（窄、绕行进）
//   ③ 弧形扇面（宽度沿长度张开）
//   ④ 圆环：上下环面 + 内外立壁，带菲涅耳边反（玻璃质感）
//
// 三条带在屏幕空间部分交叉（交叉处能看出不透明度叠加），用来验收
// 「WBO 覆盖集 + over 合成」；三块参考方块用来验收「天光真的照在物体上」；
// 圆环用来验收 fresnel（参数／预设见 §19 与 kFresnelPresets）。
//
// 交互：右键拖拽 = 绕行、滚轮 = 推拉；面板可调视角外的一切（形状/材质/剖面/开关）。
// 拍摄：--capture <dir>（固定视角与相位，逐张对比图）。

#ifndef JPOV_DEMO_STRIP_VIEWER_APP_H_
#define JPOV_DEMO_STRIP_VIEWER_APP_H_

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/demo/skylight_scene.h"
#include "tools/jpov/demo/view_config.h"
#include "tools/jpov/demo/viewer_app.h"
#include "tools/jpov/interface/strip_types.h"
#include "tools/jpov/interface/ui.h"

namespace jpov_strip {

// 向量小工具（本文件私有）。归一化在零向量时回退，避免 NaN 进几何。
inline jpov::Vec3f NormalizeOr(const jpov::Vec3f& v, const jpov::Vec3f& fallback) {
    const float n = v.Norm();
    if (n < 1e-6f) {
        return fallback;
    }
    return jpov::Vec3f{v.x() / n, v.y() / n, v.z() / n};
}

inline jpov::Vec3f Cross(const jpov::Vec3f& a, const jpov::Vec3f& b) {
    return jpov::Vec3f{a.y() * b.z() - a.z() * b.y(),
                       a.z() * b.x() - a.x() * b.z(),
                       a.x() * b.y() - a.y() * b.x()};
}

// ── 形状参数（面板滑条与拍摄默认共用同一处状态）──
//
// 默认值 = gold 图所用的构图，改动默认值会让 gold 变红（有意为之）。
struct RibbonShape {
    float wave_amplitude = 0.55f;   // 波带 Y 方向振幅（米）
    float helix_turns = 1.5f;       // 螺旋绕行圈数
    float arc_angle = 1.9f;         // 弧形扫过的圆心角（弧度）
    float arc_yaw = 0.0f;           // 弧形整体绕世界 Y 轴旋转（弧度）
};

struct RibbonVisibility {
    bool wave = true;
    bool helix = true;
    bool arc = true;
};

// 默认视角/天光（viewer 与 gold test 共用；单点定义，避免两处漂移）。
struct ViewDefaults {
    double phi = 0.24;
    double theta = 0.60;
    double R = 5.6;
    float sun_elev_deg = 34.0f;
    float sun_azim_deg = 40.0f;
    float turbidity = 2.4f;
};

inline constexpr ViewDefaults kViewDefaults{};

// 圆环在场景里的默认位置：让开三块参考方块与三条带。
// 放原点会被螺旋条带穿过、波带也从环体里斜穿过去（能看，但看不清边反）。
inline jpov::StripRingShape MakeSceneRingShape() {
    jpov::StripRingShape shape;
    shape.center_x = -1.70f;
    shape.center_y = 1.20f;
    shape.center_z = 1.00f;
    return shape;
}

// ── 菲涅耳预设（实验用的几种参数配置）──
//
// 面板 Combo 与 --capture 共用这一份表：避免「截图里是 A、面板上写着 B」。
// 每项只改数值、不改公式 —— 公式见 interface/strip_types.h:StripFresnel 的头注释。
struct FresnelPreset {
    const char* name;
    jpov::StripFresnel params;
};

inline constexpr FresnelPreset kFresnelPresets[] = {
    {"物理玻璃 f0=0.04 pow5",
     {.f0 = 0.04f, .power = 5.0f, .strength = 1.0f, .graze_alpha = 1.0f,
      .rim_color = {0.85f, 0.93f, 1.00f}}},
    {"柔边塑料 f0=0.04 pow2",
     {.f0 = 0.04f, .power = 2.0f, .strength = 1.0f, .graze_alpha = 0.85f,
      .rim_color = {1.00f, 1.00f, 1.00f}}},
    {"厚玻璃 f0=0.10 pow4 x1.4",
     {.f0 = 0.10f, .power = 4.0f, .strength = 1.4f, .graze_alpha = 1.0f,
      .rim_color = {0.72f, 0.88f, 1.00f}}},
    {"镜面边 f0=0.16 pow6 x2.2",
     {.f0 = 0.16f, .power = 6.0f, .strength = 2.2f, .graze_alpha = 1.0f,
      .rim_color = {1.00f, 1.00f, 1.00f}}},
    {"窄边反 f0=0.03 pow8 x1.2",
     {.f0 = 0.03f, .power = 8.0f, .strength = 1.2f, .graze_alpha = 0.95f,
      .rim_color = {0.90f, 0.96f, 1.00f}}},
};
inline constexpr int kFresnelPresetCount =
    static_cast<int>(sizeof(kFresnelPresets) / sizeof(kFresnelPresets[0]));

// 边反颜色预设（面板 Combo）。冷白/纯白/暖白/冰蓝/金 —— 都是线性 HDR 值。
struct RimColorPreset {
    const char* name;
    jpov::Vec3f rgb;
};
inline constexpr RimColorPreset kRimColorPresets[] = {
    {"冷白", {0.85f, 0.93f, 1.00f}},
    {"纯白", {1.00f, 1.00f, 1.00f}},
    {"暖白", {1.00f, 0.93f, 0.82f}},
    {"冰蓝", {0.62f, 0.85f, 1.00f}},
    {"金",   {1.00f, 0.78f, 0.35f}},
};
inline constexpr int kRimColorPresetCount =
    static_cast<int>(sizeof(kRimColorPresets) / sizeof(kRimColorPresets[0]));

// ── 条带生成（纯函数，gold 与 viewer 共用同一份实现，避免"看起来差不多"）──

// 从参数化中心线 + 逐点半宽生成 Strip16。
//
// center_fn(s, phase) → 世界空间中心点（s ∈ [0,1]）
// half_left_fn(s) / half_right_fn(s) → 两侧半宽（米）
// alpha_fn(s) → 纵向 alpha 乘子
//
// 侧向基：side = normalize(cross(tangent, up_hint))；切向取中心差分（端点单侧），
// 与设计文档 §2.1 一致。up_hint 与切向共线时退到 (0,0,1)（避免 NaN）。
template <typename CenterFn, typename HalfLeftFn, typename HalfRightFn, typename AlphaFn>
void BuildRibbon(float phase, CenterFn center_fn, HalfLeftFn half_left_fn,
                 HalfRightFn half_right_fn, AlphaFn alpha_fn, jpov::Strip16* out) {
    CHECK(out != nullptr) << "BuildRibbon: out 不能为空";
    const int n = jpov::kStripSampleCount;
    const float ds = 0.5f / static_cast<float>(n - 1);

    for (int i = 0; i < n; ++i) {
        const float s = static_cast<float>(i) / static_cast<float>(n - 1);
        const jpov::Vec3f c = center_fn(s, phase);

        // 切向：内部中心差分，端点单侧差分。
        const float s0 = (i == 0) ? s : s - ds;
        const float s1 = (i == n - 1) ? s : s + ds;
        const jpov::Vec3f a = center_fn(s0, phase);
        const jpov::Vec3f b = center_fn(s1, phase);
        jpov::Vec3f t = NormalizeOr(b - a, {0.0f, 0.0f, 1.0f});

        jpov::Vec3f up = {0.0f, 1.0f, 0.0f};
        jpov::Vec3f side = Cross(t, up);
        if (side.Norm() < 1e-4f) {
            side = Cross(t, {0.0f, 0.0f, 1.0f});
        }
        side = NormalizeOr(side, {1.0f, 0.0f, 0.0f});

        out->left[static_cast<size_t>(i)] = c + side * half_left_fn(s);
        out->right[static_cast<size_t>(i)] = c - side * half_right_fn(s);
        out->alpha[static_cast<size_t>(i)] = alpha_fn(s);
    }
    out->point_count = static_cast<uint32_t>(n);
}

// ① 波带：沿 +X 前进，Y 方向正弦起伏，带内轻微前后摆动。
inline void BuildWaveRibbon(float phase, float width_scale, float amplitude,
                            jpov::Strip16* out) {
    const float kLen = 7.0f;
    auto center = [kLen, amplitude](float s, float ph) {
        const float x = (s - 0.5f) * kLen;
        return jpov::Vec3f{x, 0.85f + amplitude * std::sin(1.7f * s * 3.14159f + ph),
                           -0.45f + 0.30f * std::cos(1.1f * s * 3.14159f + ph)};
    };
    auto half_l = [width_scale](float s) { return (0.35f + 0.10f * s) * width_scale; };
    auto half_r = [width_scale](float /*s*/) { return 0.35f * width_scale; };
    // 尾部淡出：验证「逐点 alpha」这条通路真的进了混合。
    auto alpha = [](float s) {
        const float fade_in = std::min(1.0f, s / 0.08f);
        const float fade_out = std::min(1.0f, (1.0f - s) / 0.22f);
        return fade_in * fade_out;
    };
    BuildRibbon(phase, center, half_l, half_r, alpha, out);
}

// ② 螺旋拖尾：绕竖轴上升，宽度恒定（窄）。
inline void BuildHelixRibbon(float phase, float width_scale, float turns,
                             jpov::Strip16* out) {
    auto center = [turns](float s, float ph) {
        const float ang = 2.0f * 3.14159f * turns * s + ph;
        const float r = 1.05f - 0.25f * s;
        return jpov::Vec3f{r * std::sin(ang) + 0.2f, 0.30f + 2.20f * s,
                           r * std::cos(ang) - 0.6f};
    };
    auto half_w = [width_scale](float /*s*/) { return 0.22f * width_scale; };
    auto alpha = [](float /*s*/) { return 1.0f; };
    BuildRibbon(phase, center, half_w, half_w, alpha, out);
}

// ③ 弧形扇面：竖直方向扫一段弧，宽度沿弧长张开（剑气式）；yaw 绕 Y 轴整体转向。
inline void BuildArcRibbon(float phase, float width_scale, float angle, float yaw,
                           jpov::Strip16* out) {
    auto center = [angle, yaw](float s, float ph) {
        const float ang = -1.00f + angle * s + ph;   // 起点固定 -1rad，扫角=angle
        const float r = 2.40f;
        const float x = r * std::sin(ang) + 0.5f;
        const float z = r * std::cos(ang) - 1.0f;
        const float c = std::cos(yaw);
        const float sn = std::sin(yaw);
        return jpov::Vec3f{x * c + z * sn, 1.30f - 0.90f * s, -x * sn + z * c};
    };
    auto half_l = [width_scale](float s) { return (0.06f + 0.55f * s * s) * width_scale; };
    auto half_r = [width_scale](float s) { return (0.04f + 0.12f * s) * width_scale; };
    auto alpha = [](float s) { return std::min(1.0f, (1.0f - s) / 0.35f); };
    BuildRibbon(phase, center, half_l, half_r, alpha, out);
}

// 宽度方向的 alpha 剖面（9 knot）：柔边 = 两端衰减到 0；硬边 = 全 1。
// 抽成常量：三条带 / 射流 / 纯色批的「搬移」都从这里取，避免同一组数字散在三个地方漂移。
inline constexpr std::array<float, 9> kSoftWidthProfile = {
    0.0f, 0.35f, 0.80f, 1.0f, 1.0f, 1.0f, 0.80f, 0.35f, 0.0f};
inline constexpr std::array<float, 9> kHardWidthProfile = {
    1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};

// 三条带装进**一个 batch** → 一次 draw call。
// 返回 false 表示三条都被关掉了（调用方不要发空 batch）。
inline bool BuildDemoBatch(const RibbonShape& shape, const RibbonVisibility& vis,
                           float phase, float width_scale, float opacity,
                           bool soft_edges, uint32_t texture_id,
                           jpov::TranslucentStripBatch* out) {
    CHECK(out != nullptr) << "BuildDemoBatch: out 不能为空";
    out->strips.clear();
    // 三条带各自的不透明度走**逐条带 tint**（per instance），而不是改写逐点 alpha：
    // 重叠处要能分辨、又不想为每条带单独发一次 draw call，这正是那个通道的用途。
    if (vis.wave) {
        jpov::Strip16 strip;
        BuildWaveRibbon(phase, width_scale, shape.wave_amplitude, &strip);
        out->strips.push_back(strip);
        out->strip_tints.push_back(jpov::Color{1.0f, 1.0f, 1.0f, 1.00f});
    }
    if (vis.helix) {
        jpov::Strip16 strip;
        BuildHelixRibbon(phase, width_scale, shape.helix_turns, &strip);
        out->strips.push_back(strip);
        out->strip_tints.push_back(jpov::Color{1.0f, 1.0f, 1.0f, 0.62f});
    }
    if (vis.arc) {
        jpov::Strip16 strip;
        BuildArcRibbon(phase, width_scale, shape.arc_angle, shape.arc_yaw, &strip);
        out->strips.push_back(strip);
        out->strip_tints.push_back(jpov::Color{1.0f, 1.0f, 1.0f, 0.45f});
    }
    if (out->strips.empty()) {
        return false;
    }
    out->texture_id = texture_id;
    out->tint = jpov::Color{1.0f, 1.0f, 1.0f, opacity};
    // 「柔边」= 宽度两端衰减到 0（几何折角被吃掉，视觉上变软）；硬边 = 全 1。
    out->width_profile = soft_edges ? kSoftWidthProfile : kHardWidthProfile;
    return true;
}

// ── ④ 火箭射流（模拟推进器）：锥形发光条带 ──
//
// 这条带一次演示三件东西：
//   ① 加法集 + **HDR 增益**：亮度可以超过 1，才吃得到 bloom / tone map 的余量；
//   ② **逐点颜色**（沿长度）：白 → 暖白 → 橙 → 红 → 暗红，尾部同时按逐点 alpha 淡出；
//   ③ **9-knot 宽度剖面**：中心实、两侧虚（射流的横向软边）。
// 用**两条交叉的条带**（同一个 batch）：单条带是平面，侧看会消失，交叉之后各角度都像个射流。
struct JetShape {
    jpov::Vec3f nozzle{2.55f, 2.35f, 0.85f};   // 喷口位置
    jpov::Vec3f dir{0.18f, -1.0f, 0.10f};      // 喷射方向（内部归一化）
    float length = 2.05f;                      // 射流长度（米）
    float width0 = 0.10f;                      // 喷口处半宽
    float width1 = 0.34f;                      // 尾部半宽（扩散）
};

// 射流颜色：s = 0（喷口）→ 1（尾端）。白热 → 暖白 → 橙 → 红 → 暗红。
inline jpov::Color JetColorAt(float s) {
    struct Stop {
        float s;
        jpov::Color c;
    };
    static constexpr Stop kStops[] = {
        {0.00f, {1.00f, 1.00f, 1.00f, 1.0f}},
        {0.18f, {1.00f, 0.93f, 0.66f, 1.0f}},
        {0.45f, {1.00f, 0.55f, 0.12f, 1.0f}},
        {0.75f, {0.85f, 0.17f, 0.05f, 1.0f}},
        {1.00f, {0.30f, 0.03f, 0.02f, 1.0f}},
    };
    const float t = std::clamp(s, 0.0f, 1.0f);
    for (size_t i = 1; i < sizeof(kStops) / sizeof(kStops[0]); ++i) {
        if (t <= kStops[i].s) {
            const Stop& a = kStops[i - 1];
            const Stop& b = kStops[i];
            const float f = (t - a.s) / (b.s - a.s);
            return jpov::Color{a.c.r + (b.c.r - a.c.r) * f,
                               a.c.g + (b.c.g - a.c.g) * f,
                               a.c.b + (b.c.b - a.c.b) * f, 1.0f};
        }
    }
    return kStops[sizeof(kStops) / sizeof(kStops[0]) - 1].c;
}

// 沿中心线 + 侧向半宽生成一条射流条带，同时填逐点颜色。
inline void BuildJetStrip(const JetShape& shape, const jpov::Vec3f& side,
                          jpov::Strip16* out, jpov::StripPointColors* colors) {
    CHECK(out != nullptr) << "BuildJetStrip: out 不能为空";
    CHECK(colors != nullptr) << "BuildJetStrip: colors 不能为空";
    const jpov::Vec3f dir = NormalizeOr(shape.dir, {0.0f, -1.0f, 0.0f});
    for (int i = 0; i < jpov::kStripSampleCount; ++i) {
        const size_t k = static_cast<size_t>(i);
        const float s = static_cast<float>(i) / static_cast<float>(jpov::kStripSampleCount - 1);
        const jpov::Vec3f center = shape.nozzle + dir * (shape.length * s);
        // 扩散：s^0.8 让喷口附近收得更紧（视觉上更像被挤压出来的射流）。
        const float half = shape.width0 + (shape.width1 - shape.width0) * std::pow(s, 0.8f);
        out->left[k] = center + side * half;
        out->right[k] = center - side * half;
        // 逐点 alpha：前 15% 满，之后线性淡出到 0（尾部消失而不是被截断）。
        out->alpha[k] = std::min(1.0f, (1.0f - s) / 0.85f);
        (*colors)[k] = JetColorAt(s);
    }
    out->point_count = static_cast<uint32_t>(jpov::kStripSampleCount);
}

// 射流批：两条交叉条带（同一个 batch ⇒ 一次 draw call），加法集 + HDR 增益 + 中心实两侧虚剖面。
inline jpov::TranslucentStripBatch BuildJetBatch(const JetShape& shape, float gain,
                                                 jpov::StripStyle style) {
    const jpov::Vec3f dir = NormalizeOr(shape.dir, {0.0f, -1.0f, 0.0f});
    // 与 dir 不平行的辅助向量（dir 接近竖直时用 +X，否则用 +Y）⇒ 两条侧向互相垂直。
    const jpov::Vec3f helper =
        (std::fabs(dir.y()) > 0.9f) ? jpov::Vec3f{1.0f, 0.0f, 0.0f}
                                    : jpov::Vec3f{0.0f, 1.0f, 0.0f};
    const jpov::Vec3f side_a = NormalizeOr(Cross(dir, helper), {1.0f, 0.0f, 0.0f});
    const jpov::Vec3f side_b = NormalizeOr(Cross(dir, side_a), {0.0f, 0.0f, 1.0f});

    jpov::TranslucentStripBatch batch;
    batch.texture_id = 0;
    batch.tint = jpov::Color{1.0f, 1.0f, 1.0f, 1.0f};   // 颜色全在逐点颜色里
    batch.blend = jpov::StripBlend::kEmissive;
    batch.additive_gain = gain;
    batch.style = style;
    // 中心实、两侧虚：射流的横截面自然衰减（逐片元求值，15 段也不会有阶梯）。
    batch.width_profile = kSoftWidthProfile;
    for (const jpov::Vec3f& side : {side_a, side_b}) {
        jpov::Strip16 strip;
        jpov::StripPointColors colors{};
        BuildJetStrip(shape, side, &strip, &colors);
        batch.strips.push_back(strip);
        batch.strip_point_colors.push_back(colors);
    }
    return batch;
}

class StripViewerApp : public JPOV {
public:
    using JPOV::JPOV;

    uint32_t ground_mesh_ = 0;
    jpov::PBRMaterial ground_mat_;
    uint32_t box_mesh_ = 0;                 // 背景参考物（验收天光用）
    jpov::PBRMaterial box_mat_low_gloss_;
    jpov::PBRMaterial box_mat_high_gloss_;
    jpov::PBRMaterial box_mat_metal_;
    uint32_t ribbon_tex_ = 0;               // 0 = 纹理未加载（退回纯色）

    jpov_viewer::ViewConfig view_;
    jpov_skylight::SkyDegrees deg_;

    // ── 面板旋钮 ──
    RibbonShape shape_;
    RibbonVisibility vis_;
    float opacity_ = 0.92f;          // 整批不透明度
    float width_scale_ = 1.0f;       // 条带宽度整体缩放
    bool use_texture_ = true;        // false = 纯色（不采样纹理）
    jpov::Color solid_color_ = {0.18f, 0.78f, 0.95f, 1.0f};   // 纯色模式颜色
    bool soft_edges_ = true;         // 宽度剖面：柔边 / 硬边
    bool animate_ = false;           // 形状动画（gold 用 phase = 0）
    bool auto_orbit_ = false;        // 自动绕行（不拖鼠标也能看各角度）
    bool smaa_ = false;              // SMAA 后处理（看边缘质量）
    bool show_strips_ = true;        // false = 不发条带命令（A/B 对照）
    bool boxes_visible_ = true;      // 背景参考方块（只看圆环时可关掉，免得挡住环体）
    // true = 平滑 pass（kSmooth：C1 重建 + **解析法线**）；false = 折线 pass（弦 + 折面法线）。
    // 默认平滑：圆环边反与射流软边都只有在这条 pass 上才是最终观感；
    // 折线 pass 仍然可切（A/B 用，gold 也锁的是它）。
    bool smooth_strips_ = true;
    // 圆环（玻璃壳）单独一个平滑开关（Danis 2026-10-22 要求）：
    // 四条 tube 的观感要和三条带/射流分开 A/B —— 圆环是闭合曲面 + 硬边交接，
    // 和拖尾那种"该柔"的东西不是同一个判断，绑在一个开关上没法单独满意。
    bool ring_smooth_ = true;
    bool arc_additive_ = false;      // 弧形改用**加法集**（发光拖尾），其余仍走覆盖集
    bool soft_depth_ = false;        // 条带与几何相交处的软过渡（per batch 选项）
    float soft_depth_m_ = 0.15f;     // 软深度过渡距离（米）
    float phase_ = 0.0f;             // 当前相位（animate_ 时逐帧推进）

    // ── 圆环（玻璃壳）──
    jpov::StripRingShape ring_shape_ = MakeSceneRingShape();
    bool ring_visible_ = true;              // 四条带 = 顶/底环面 + 内/外立壁
    jpov::Vec3f ring_color_{0.70f, 0.85f, 1.00f};   // 玻璃膜的颜色（线性 HDR）
    float ring_opacity_ = 0.12f;            // 薄膜的不透明度（边反由 fresnel 抬起来）

    // ── 火箭射流（加法集 + HDR 增益 + 逐点颜色 + 中心实两侧虚剖面）──
    JetShape jet_shape_;
    bool jet_visible_ = true;
    float jet_gain_ = 3.0f;                 // HDR 增益（>1 才有 bloom 余量）
    bool bloom_ = true;                     // 辉光（把 HDR 高亮模糊加回）

    // ── 菲涅耳（玻璃边反）──
    // 默认开：这个 viewer 现在的主场景就是那块玻璃圆环，不开就只是层淡蓝膜。
    bool fresnel_enabled_ = true;
    int fresnel_preset_ = 0;                // kFresnelPresets 下标
    int rim_color_preset_ = 0;              // kRimColorPresets 下标
    jpov::StripFresnel fresnel_ = kFresnelPresets[0].params;

    // 装配场景静态资源（Init 之后、Run/RunOnce 之前调用一次）。
    void Setup(const std::string& texture_path);

    // 视角/天光回到默认（gold 与面板「重置视角」共用）。
    void ResetViewToDefaults() {
        view_ = jpov_viewer::ViewConfig{};
        view_.phi = kViewDefaults.phi;
        view_.theta = kViewDefaults.theta;
        view_.R = kViewDefaults.R;
        deg_.sun_elev_deg = kViewDefaults.sun_elev_deg;
        deg_.sun_azim_deg = kViewDefaults.sun_azim_deg;
        deg_.turbidity = kViewDefaults.turbidity;
        phase_ = 0.0f;
        ring_shape_ = MakeSceneRingShape();
    }

    void SetShowPanel(bool show) { show_panel_ = show; }

    // UI 文本宽度回调（中文字符宽度用真实字体量）。
    void InstallTextMeasure() { ui_.SetTextMeasure(&StripViewerApp::TextWidth, this); }

    void OneIteration(int64_t frame_count, const jpov::InputSnapshot& input,
                      const jpov::WindowInfo& winfo,
                      jpov::RenderCommandList* cmds) override {
        (void)frame_count;
        cmds->camera.fbo_3d_width_ = jpov_viewer::kViewerWidth;
        cmds->camera.fbo_3d_height_ = jpov_viewer::kViewerHeight;

        if (show_panel_) {
            float dx = 0.0f;
            float dy = 0.0f;
            float scroll = 0.0f;
            if (input.right.IsDrag()) {
                dx = input.mouse_dx;
                dy = input.mouse_dy;
            }
            if (input.scroll_delta != 0.0f) {
                scroll = input.scroll_delta;
            }
            jpov_viewer::ApplyInput(&view_, dx, dy, scroll,
                                    static_cast<int>(winfo.width),
                                    static_cast<int>(winfo.height));
        }
        if (auto_orbit_) {
            view_.theta += 0.006;   // 约 17°/s @60fps：够慢，能看清条带转向
        }
        if (animate_) {
            phase_ += 0.02f;
        }

        cmds->camera.position = view_.Position();
        // 三条带都在 y≈0.3..2.7（离地悬空），所以看向原点会让画面整体偏上；
        // 目标点抬高到 1.15m 才把条带放在画面中央。
        cmds->camera.target = {0.0f, 1.15f, 0.0f};
        cmds->camera.up = {0.0f, 1.0f, 0.0f};
        cmds->camera.fov = 55.0f;
        cmds->camera.near = 0.05f;
        cmds->camera.far = 200.0f;

        // 天光：天色与物体受光同源（见 skylight_scene.h 的约定）。
        const jpov_skylight::SkyLighting light =
            jpov_skylight::MakeSkyLighting(deg_, /*tricolor_ambient=*/true);
        cmds->sky = light.sky;
        cmds->sun = light.dir_light;
        cmds->ambient = light.ambient;
        cmds->tone_mapping = true;
        if (smaa_) {
            cmds->smaa = jpov::SmaaConfig{};
            cmds->smaa->enabled = true;
        }
        if (bloom_) {
            // 阈值 1.0 = 只让「超过白点」的 HDR 部分发光（射流靠 additive_gain 超过它）。
            cmds->bloom = jpov::BloomConfig{};
            cmds->bloom->enabled = true;
            cmds->bloom->intensity = 0.8f;
            cmds->bloom->threshold = 1.0f;
            cmds->bloom->levels = 4;
        }

        cmds->DrawObject3D(ground_mesh_, ground_mat_,
                           /*center*/ {0.0f, 0.0f, 0.0f},
                           /*up*/     {0.0f, 1.0f, 0.0f},
                           /*front*/  {0.0f, 0.0f, 1.0f},
                           /*scale*/  1.0f);

        // 背景三块参考方块（低反/高光/金属）：让「天光真的照在物体上」可验，
        // 同时为条带提供可对照的前后遮挡——条带在半透明的同时仍吃场景深度。
        if (boxes_visible_) {
            cmds->DrawObject3D(box_mesh_, box_mat_low_gloss_, {-2.0f, 0.5f, -2.6f},
                               {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, /*scale*/ 1.0f);
            cmds->DrawObject3D(box_mesh_, box_mat_high_gloss_, {0.0f, 0.5f, -2.6f},
                               {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, /*scale*/ 1.0f);
            cmds->DrawObject3D(box_mesh_, box_mat_metal_, {2.0f, 0.5f, -2.6f},
                               {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, /*scale*/ 1.0f);
            // 第四块「穿过条带的方块」：波带从它的体积里穿过去，专门用来验收**软深度**
            // （硬判据 = 相交处一条硬切边；软深度 = 一条按距离淡出的过渡带）。
            cmds->DrawObject3D(box_mesh_, box_mat_low_gloss_, {0.35f, 0.62f, -0.55f},
                               {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, /*scale*/ 1.5f);
        }

        if (show_strips_) {
            EmitStripBatches(cmds);
        }

        if (show_panel_) {
            DrawPanel(input);
            ui_.End();
            ui_.Emit(cmds);
        }
    }

private:
    // 组装并提交本帧的条带批。
    //
    // 覆盖集与加法集是**不同的附件**（不同 pass），所以「弧形用加法」时会把弧
    // 单独拆成第二个 batch —— 一个 batch 就是一次 draw call，语义边界在材质/混合。
    void EmitStripBatches(jpov::RenderCommandList* cmds) {
        const uint32_t tex = use_texture_ ? ribbon_tex_ : 0;

        auto build = [this, tex](const RibbonVisibility& vis) {
            jpov::TranslucentStripBatch batch;
            const float opacity = use_texture_ ? opacity_ : 1.0f;
            if (!BuildDemoBatch(shape_, vis, phase_, width_scale_, opacity, soft_edges_,
                                tex, &batch)) {
                return batch;   // strips 为空，调用方跳过
            }
            if (!use_texture_) {
                // 纯色接口：不采样纹理，颜色 = solid_color_（alpha 由 opacity_ 控）。
                // MakeSolidStripBatch 会**重建整个 batch**（只保留颜色与条带），所以
                // 「逐条带浓度」和「宽度剖面」这两项必须显式搬过去 —— 否则：
                //   * 漏搬 strip_tints ⇒ 三条带又变回同样不透明；
                //   * 漏搬 width_profile ⇒ 面板上「柔边剖面」明明勾着，一切纯色就变硬边。
                // （这两项都是 viewer 侧的状态，不属于 MakeSolidStripBatch 的语义。）
                jpov::Color color = solid_color_;
                color.a = opacity_;
                std::vector<jpov::Strip16> strips = std::move(batch.strips);
                std::vector<jpov::Color> tints = std::move(batch.strip_tints);
                const std::array<float, 9> profile = batch.width_profile;
                batch = jpov::MakeSolidStripBatch(color, std::move(strips));
                batch.strip_tints = std::move(tints);
                batch.width_profile = profile;
            }
            if (soft_depth_) {
                batch.soft_depth_m = soft_depth_m_;
            }
            // 边反（fresnel）**只给玻璃圆环**：它是"玻璃材质"的观感参数，
            // 三条带/射流是发光拖尾，套上边反只会让拖尾边缘发白（Danis 2026-10-22 指出）。
            // 库层面 StripFresnel 仍是批级参数（谁想用谁设），这里是 viewer 的取舍。
            batch.style = smooth_strips_ ? jpov::StripStyle::kSmooth
                                         : jpov::StripStyle::kPolyline;
            return batch;
        };

        RibbonVisibility coverage_vis = vis_;
        if (arc_additive_) {
            coverage_vis.arc = false;
        }
        jpov::TranslucentStripBatch coverage = build(coverage_vis);
        if (!coverage.strips.empty()) {
            coverage.blend = jpov::StripBlend::kCoverage;
            cmds->DrawTranslucentStrips(std::move(coverage));
        }

        if (arc_additive_ && vis_.arc) {
            RibbonVisibility only_arc;
            only_arc.wave = false;
            only_arc.helix = false;
            only_arc.arc = true;
            jpov::TranslucentStripBatch additive = build(only_arc);
            if (!additive.strips.empty()) {
                additive.blend = jpov::StripBlend::kEmissive;
                cmds->DrawTranslucentStrips(std::move(additive));
            }
        }

        if (ring_visible_) {
            EmitRingBatch(cmds);
        }
        if (jet_visible_) {
            cmds->DrawTranslucentStrips(
                BuildJetBatch(jet_shape_, jet_gain_,
                              smooth_strips_ ? jpov::StripStyle::kSmooth
                                             : jpov::StripStyle::kPolyline));
        }
    }

    // 圆环（玻璃壳）：四组点 → 四条带 → **一个批**（一次 draw call）。
    //
    // 用纯色（不采样条带纹理）：贴图上的彩色横条会把边反盖成花纹，看不出玻璃；
    // 宽度剖面也用硬边（默认全 1）—— 玻璃壳的轮廓就是硬的，柔边会把边反直接抹平。
    void EmitRingBatch(jpov::RenderCommandList* cmds) {
        // 面板上「外半径」与「壁厚」是两个独立滑条，任何时刻都可能出现 wall ≥ outer
        // （形状校验会 CHECK 崩）。所以在**使用处**夹一次；形状层的校验仍然严格 ——
        // 不把非法输入合法化，只是不让滑条的瞬时状态把 viewer 崩掉。
        jpov::StripRingShape shape = ring_shape_;
        shape.wall_thickness = std::min(shape.wall_thickness, shape.outer_radius * 0.95f);
        const std::array<jpov::Strip16, jpov::kStripRingPartCount> parts =
            jpov::MakeStripRing(shape);
        jpov::TranslucentStripBatch ring;
        ring.texture_id = 0;   // 纯色
        ring.tint = jpov::Color{ring_color_.x(), ring_color_.y(), ring_color_.z(),
                                ring_opacity_};
        ring.blend = jpov::StripBlend::kCoverage;
        ring.style = ring_smooth_ ? jpov::StripStyle::kSmooth
                                  : jpov::StripStyle::kPolyline;
        if (fresnel_enabled_) {
            ring.fresnel = fresnel_;
        }
        if (soft_depth_) {
            ring.soft_depth_m = soft_depth_m_;
        }
        ring.strips.assign(parts.begin(), parts.end());
        cmds->DrawTranslucentStrips(std::move(ring));
    }

    static float TextWidth(const char* text, float font_size, const char* /*alias*/,
                           void* userdata) {
        StripViewerApp* app = static_cast<StripViewerApp*>(userdata);
        return app->MeasureTextWidth(std::string(), text ? text : "", font_size);
    }

    void DrawPanel(const jpov::InputSnapshot& input) {
        const float w = static_cast<float>(jpov_viewer::kViewerWidth);
        const float h = static_cast<float>(jpov_viewer::kViewerHeight);
        jpov::UiTheme theme = jpov::UiTheme::Default(kFontSize);
        theme.font_alias = jpov_viewer::kViewerFontAlias;
        ui_.Begin(input, theme, w, h, 1000.0f / jpov_viewer::kViewerFps);

        // 三列布局：控件已到 30 个，两列会超出 720 高。
        const float kColW = 0.28f * w;
        const float kRowH = 28.0f;
        const float kSpacing = 8.0f;
        const float kLeftX = 20.0f;
        const float kMidX = kLeftX + kColW + 16.0f;
        const float kRightX = kMidX + kColW + 16.0f;
        const float kTopY = 20.0f;
        int row_left = 0;
        int row_mid = 0;
        int row_right = 0;
        auto left_rect = [&]() {
            const jpov::UiRect r{{kLeftX, kTopY + static_cast<float>(row_left) * (kRowH + kSpacing)},
                                 {kColW, kRowH}};
            row_left += 1;
            return r;
        };
        auto mid_rect = [&]() {
            const jpov::UiRect r{{kMidX, kTopY + static_cast<float>(row_mid) * (kRowH + kSpacing)},
                                 {kColW, kRowH}};
            row_mid += 1;
            return r;
        };
        auto right_rect = [&]() {
            const jpov::UiRect r{{kRightX, kTopY + static_cast<float>(row_right) * (kRowH + kSpacing)},
                                 {kColW, kRowH}};
            row_right += 1;
            return r;
        };

        // ── 左列：光 / 材质 / 后处理 ──
        ui_.SliderFloat("太阳仰角 °", &(deg_.sun_elev_deg), left_rect(), 0.0f, 90.0f, 0);
        ui_.SliderFloat("不透明度", &opacity_, left_rect(), 0.0f, 1.0f, 2);
        ui_.SliderFloat("条带宽度", &width_scale_, left_rect(), 0.2f, 2.5f, 2);
        ui_.Checkbox("使用纹理（关 = 纯色）", &use_texture_, left_rect());
        ui_.Checkbox("柔边剖面", &soft_edges_, left_rect());
        ui_.Checkbox("SMAA 抗锯齿", &smaa_, left_rect());
        ui_.Checkbox("Bloom 辉光（HDR）", &bloom_, left_rect());
        ui_.Checkbox("显示条带（A/B）", &show_strips_, left_rect());
        ui_.Checkbox("条带平滑（三条带/射流）", &smooth_strips_, left_rect());
        ui_.Checkbox("弧形用加法集", &arc_additive_, left_rect());
        ui_.Checkbox("软深度", &soft_depth_, left_rect());
        ui_.SliderFloat("软深度距离 m", &soft_depth_m_, left_rect(), 0.005f, 0.30f, 3);
        ui_.Checkbox("显示参考方块", &boxes_visible_, left_rect());

        // ── 中列：形状 / 动画 / 显隐 / 圆环 ──
        ui_.SliderFloat("波带振幅", &(shape_.wave_amplitude), mid_rect(), 0.0f, 1.2f, 2);
        ui_.SliderFloat("螺旋转数", &(shape_.helix_turns), mid_rect(), 0.5f, 3.0f, 2);
        ui_.SliderFloat("弧形开角", &(shape_.arc_angle), mid_rect(), 0.6f, 2.6f, 2);
        ui_.SliderFloat("弧形朝向", &(shape_.arc_yaw), mid_rect(), -1.6f, 1.6f, 2);
        ui_.Checkbox("形状动画", &animate_, mid_rect());
        ui_.Checkbox("自动绕行", &auto_orbit_, mid_rect());
        ui_.Checkbox("显示波带", &(vis_.wave), mid_rect());
        ui_.Checkbox("显示螺旋", &(vis_.helix), mid_rect());
        ui_.Checkbox("显示弧形", &(vis_.arc), mid_rect());
        ui_.Checkbox("显示射流（火箭推进器）", &jet_visible_, mid_rect());
        ui_.SliderFloat("射流 HDR 增益", &jet_gain_, mid_rect(), 1.0f, 8.0f, 2);
        ui_.Checkbox("显示圆环（玻璃壳）", &ring_visible_, mid_rect());
        ui_.Checkbox("圆环平滑（玻璃壳）", &ring_smooth_, mid_rect());
        ui_.SliderFloat("圆环外半径 m", &(ring_shape_.outer_radius), mid_rect(), 0.2f, 2.5f, 2);
        ui_.SliderFloat("圆环壁厚 m", &(ring_shape_.wall_thickness), mid_rect(), 0.02f, 0.60f, 2);
        ui_.SliderFloat("圆环高度 m", &(ring_shape_.height), mid_rect(), 0.20f, 3.00f, 2);
        ui_.SliderFloat("圆环不透明度", &ring_opacity_, mid_rect(), 0.0f, 1.0f, 2);
        const bool reset_clicked = ui_.Button("重置视角/形状", mid_rect());
        if (reset_clicked) {
            ResetViewToDefaults();
            shape_ = RibbonShape{};
            vis_ = RibbonVisibility{};
            arc_additive_ = false;
            smooth_strips_ = true;
            ring_smooth_ = true;
            soft_depth_ = false;
            soft_depth_m_ = 0.15f;
            ring_visible_ = true;
            boxes_visible_ = true;
            jet_visible_ = true;
            jet_gain_ = 3.0f;
            bloom_ = true;
            ring_opacity_ = 0.12f;
            fresnel_enabled_ = true;
            fresnel_preset_ = 0;
            rim_color_preset_ = 0;
            fresnel_ = kFresnelPresets[0].params;
        }

        // ── 右列：菲涅耳（玻璃边反）──
        ui_.Checkbox("菲涅耳边反", &fresnel_enabled_, right_rect());
        std::vector<const char*> preset_names;
        preset_names.reserve(static_cast<size_t>(kFresnelPresetCount));
        for (const FresnelPreset& p : kFresnelPresets) {
            preset_names.push_back(p.name);
        }
        if (ui_.Combo("预设", &fresnel_preset_, preset_names, right_rect())) {
            // 选预设 = 整组参数覆盖（含边反颜色）；之后再拖滑条就进入自定义态。
            fresnel_ = kFresnelPresets[fresnel_preset_].params;
        }
        ui_.SliderFloat("F0 正入射反射", &(fresnel_.f0), right_rect(), 0.0f, 0.30f, 2);
        ui_.SliderFloat("掠射指数", &(fresnel_.power), right_rect(), 0.5f, 8.0f, 1);
        ui_.SliderFloat("边反强度", &(fresnel_.strength), right_rect(), 0.0f, 3.0f, 2);
        ui_.SliderFloat("掠射不透明度", &(fresnel_.graze_alpha), right_rect(), 0.0f, 1.0f, 2);
        std::vector<const char*> rim_names;
        rim_names.reserve(static_cast<size_t>(kRimColorPresetCount));
        for (const RimColorPreset& p : kRimColorPresets) {
            rim_names.push_back(p.name);
        }
        if (ui_.Combo("边反颜色", &rim_color_preset_, rim_names, right_rect())) {
            fresnel_.rim_color = kRimColorPresets[rim_color_preset_].rgb;
        }

        // 相机/当前形态信息（底部一行）：便于口头描述/复现某个角度。
        // 下标先钳一次：Combo 只保证本帧之后的 fresnel_preset_ 合法，别赌它。
        const int preset_idx = std::clamp(fresnel_preset_, 0, kFresnelPresetCount - 1);
        char info[256];
        std::snprintf(info, sizeof(info),
                      "phi %.2f  theta %.2f  R %.2f   |   %s   blend: %s%s   |   "
                      "fresnel: %s   ring: %s   jet: %s   ring_style: %s",
                      view_.phi, view_.theta, view_.R,
                      smooth_strips_ ? "smooth" : "polyline",
                      arc_additive_ ? "arc=add" : "all=coverage",
                      soft_depth_ ? "  soft_depth=on" : "",
                      fresnel_enabled_ ? kFresnelPresets[preset_idx].name : "off",
                      ring_visible_ ? "on" : "off",
                      jet_visible_ ? (bloom_ ? "on+bloom" : "on") : "off",
                      ring_smooth_ ? "smooth" : "polyline");
        ui_.Text(info, jpov::UiRect{{kLeftX, h - 46.0f}, {w - 40.0f, 24.0f}});
    }

    static constexpr float kFontSize = 16.0f;
    bool show_panel_ = true;
    jpov::Ui ui_;
};

inline void ApplyDefaultView(StripViewerApp* app) {
    CHECK(app != nullptr) << "ApplyDefaultView: app 不能为空";
    app->ResetViewToDefaults();
}

inline void StripViewerApp::Setup(const std::string& texture_path) {
    ground_mesh_ = RegisterMesh(jpov_skylight::MakeGroundQuad());
    ground_mat_ = jpov_skylight::GroundMaterial();
    box_mesh_ = RegisterMesh(jpov::MeshData::MakeBox(0.5f, 0.5f, 0.5f));
    box_mat_low_gloss_ = jpov_skylight::MaterialLowGloss();
    box_mat_high_gloss_ = jpov_skylight::MaterialHighGloss();
    box_mat_metal_ = jpov_skylight::MaterialMetal();
    if (!texture_path.empty()) {
        jpov::TextureOptions opts;
        // 不开 mipmap：本纹理是「横向 4 条色带」的手绘测试卡（1318×158），
        // 条带宽度只占屏幕几十像素 → u 方向压缩上百倍，mip 会把相邻色带（横向）
        // 混成灰。色带在 u 方向本来就是常量，不需要 mip；关掉后颜色才饱和。
        opts.mipmap = false;
        opts.repeat = false;
        ribbon_tex_ = RegisterTexture(texture_path, opts);
    }
}

}  // namespace jpov_strip

#endif  // JPOV_DEMO_STRIP_VIEWER_APP_H_
