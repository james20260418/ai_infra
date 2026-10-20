// JPOV WBO 共享 GLSL —— 权重函数与全屏解析的唯一来源
//
// 为什么单独抽出来（2026-10-20 重构）：
//   同一套 McGuire WBO 数学原先在三处各存一份（particles / volume_fire /
//   object3d 玻璃 shader），解析（resolve）也各写一份。两份副本只要有一次「只改一边」，
//   就会出现「粒子和体积火的同一层看起来不一样」这种极难定位的问题。
//
// 现在的分工：
//   - **权重**：`kWboWeightGlsl` 是唯一权威文本；object3d 的 PBR shader 里仍有一份
//     内联副本（那份 shader 是单个大字符串，注入需要拆串，改动面太大），
//     由 `test/wbo:wbo_weight_contract_test` 做**文本契约校验** —— 两边不一致就红。
//   - **解析**：`WboCompositeFs()` / `WboCompositeVs()`，粒子/体积火（以及将来的条带）
//     共用同一个 program 源码，只有 `uAdditive` 一个分支开关。

#ifndef JPOV_SRC_WBO_WBO_SHADER_H_
#define JPOV_SRC_WBO_WBO_SHADER_H_

namespace jpov {

// McGuire & Bavoil 2013 的权重函数（WBO 的全部魔法都在这里）。
//
//   weight = max(min(1, maxRGB·α), α) · clamp(0.03/(1e-5+(z/5)^4), 1e-2, 3e3)
//
// 前半「颜色因子」：亮/不透明片元权重大，暗且稀薄的小（防颜色污染与 firefly）；同时
//   `max(..., α)` 保证**暗但实**的介质（黑烟）仍能进入加权平均，不至于被亮片元挤掉。
// 后半「深度因子」：近处权重大、远处急速下降，逼近真实 over 的近处优先。
//   ⚠️ 论文里的 200 是「世界单位尺度」调参；JPOV 以米计，用 200 会让 (z/200)^4 ≪ 1e-5
//   地板 → 权重退化成常数 → 两层透明体前后不分、颜色一样（WBO order-blind）。
//   所以固定用 ~5m 尺度。
//
// ⚠️ 与 object3d_renderer.h 里的 `weight_wbo` 是**同一份数学**，由契约测试守着。
//    改这里必须同步改那边（或反过来），否则 `test/wbo:wbo_weight_contract_test` 变红。
inline constexpr const char* kWboWeightGlsl = R"glsl(
//@WBO_WEIGHT_BEGIN
float WeightWbo(float z, float a, vec3 c) {
    float mx = max(max(c.r, c.g), c.b);
    float w = max(min(1.0, mx * a), a);
    float df = clamp(0.03 / (1e-5 + pow(z / 5.0, 4.0)), 1e-2, 3e3);
    return w * df;
}
//@WBO_WEIGHT_END
)glsl";

// 全屏解析的顶点着色器：由 gl_VertexID 生成 4 个角（GL_TRIANGLE_STRIP）。
inline constexpr const char* kWboCompositeVs = R"glsl(
#version 330 core
out vec2 vUV;
void main() {
    vec2 c = vec2((gl_VertexID == 1 || gl_VertexID == 3) ? 1.0 : -1.0,
                  (gl_VertexID >= 2) ? 1.0 : -1.0);
    vUV = c * 0.5 + 0.5;
    gl_Position = vec4(c, 0.0, 1.0);
}
)glsl";

// 全屏解析的片元着色器。
//   uAdditive=0：覆盖集 → C = accum.rgb/accum.a、cov = 1−reveal，输出**预乘**色；
//                调用方用 (GL_ONE, GL_ONE_MINUS_SRC_ALPHA) 做 over。
//   uAdditive=1：加法集 → 直接输出 Σ(色·α)，调用方用 (GL_ONE, GL_ONE) 求和。
//
// 注意覆盖集的早退：accum.a <= 1e-7 时直接输出 0（否则 accum.rgb/accum.a 会出 NaN，
// NaN 一旦混进 HDR 就会污染整个后处理链）。
inline constexpr const char* kWboCompositeFs = R"glsl(
#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uAccum;
uniform sampler2D uReveal;
uniform sampler2D uAdd;
uniform int uAdditive;   // 1 = 加法集求和（blend ONE,ONE）；0 = 覆盖集 over
void main() {
    if (uAdditive == 1) {
        FragColor = vec4(texture(uAdd, vUV).rgb, 0.0);
        return;
    }
    vec4 accum = texture(uAccum, vUV);
    float reveal = texture(uReveal, vUV).r;
    if (accum.a <= 1e-7) {
        FragColor = vec4(0.0);
        return;
    }
    vec3 color = accum.rgb / accum.a;
    float cov = clamp(1.0 - reveal, 0.0, 1.0);
    FragColor = vec4(color * cov, cov);   // 预乘；over
}
)glsl";

}  // namespace jpov

#endif  // JPOV_SRC_WBO_WBO_SHADER_H_
