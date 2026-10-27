// JPOV 水皮（water skin）pass —— GLSL
//
// 一块**水平正方形的 quad**（边长由 uniform 缩放，默认 10 km²）+ 解析式菲涅耳。
// 位置：froxel（点状雾）合成**之后**、高亮 / tone map 之前（见 Renderer::Render）。
//
// 透射（本版刻意最简单）：**固定管线 alpha 混合**
//     out = water_color·α + dst·(1 − α)
// dst 就是帧缓冲里已经画好的不透明场景颜色 —— 所以本 pass 不采样场景颜色、
// 也不需要自己的 FBO（与 horizon_fog / froxel 同一套"就地合成"的写法）。
//
// α 与颜色都由 Schlick 菲涅耳给（N = (0,1,0)，双面 ⇒ 用 |N·V|）：
//     cosθ = |V.y|                      // V = 相机 → 片元（指向相机）
//     F    = f0 + (1 − f0)·(1 − cosθ)^power
//     α    = mix(body_alpha, 1, F)      // 正对视线看得穿，掠射处收敛成亮带
//     color= mix(body_color, rim_color, F)
//
// 深度：HDR 目标**没有深度附件**（场景深度由 MRT#1 逐片元写入），所以这里手动比：
//   场景深度 z 与水面自身 z 都用相机 near/far 线性化（与 strip pass 的 ZView 同一份公式），
//   水面比不透明场景**远**超过 2 cm 就丢弃 —— 水皮不能盖到挡在它前面的东西上。
//
// 已知边界（本版有意不做）：
//   * 没有吸收/散射（水下物体只是被一层淡色膜按 α 混掉，不随深度变蓝）；
//   * 没有反射探针（rim_color 是常量，不是天空 cubemap）；
//   * 没有波浪（quad 是纯平面，没有逐顶点位移）。

#ifndef JPOV_SRC_WATER_WATER_SKIN_SHADER_H_
#define JPOV_SRC_WATER_WATER_SKIN_SHADER_H_

namespace jpov {

// quad 的局部坐标在 ±1（x/z），由 uHalfExtent / uLevelY 展开成世界空间的正方形。
inline constexpr const char* kWaterSkinVs = R"glsl(
#version 330 core
layout(location = 0) in vec3 aPos;   // x,z ∈ {-1,+1}，y = 0
uniform mat4  uVP;
uniform float uHalfExtent;           // 正方形半边长（米）
uniform float uLevelY;               // 水面世界高度（米）
out vec3 vWorld;
void main() {
    vec3 p = vec3(aPos.x * uHalfExtent, uLevelY, aPos.z * uHalfExtent);
    vWorld = p;
    gl_Position = uVP * vec4(p, 1.0);
}
)glsl";

inline constexpr const char* kWaterSkinFs = R"glsl(
#version 330 core
in vec3 vWorld;
out vec4 FragColor;

uniform sampler2D uSceneDepth;   // MRT#1 场景深度（gl_FragCoord.z，1.0 = 背景/远平面）
uniform vec2  uViewport;         // 3D FBO 尺寸（把 gl_FragCoord 换成 [0,1] uv）
uniform float uNear;
uniform float uFar;
uniform vec3  uCamPos;           // 相机世界位置
uniform vec3  uBodyColor;        // 本体色（线性 HDR）
uniform float uBodyAlpha;        // 正对视线时的覆盖率 [0,1]
uniform vec3  uRimColor;         // 掠射处的反射色（线性 HDR）
uniform float uF0;               // 正入射反射率（水 ≈ 0.02）
uniform float uPower;            // Schlick 指数（物理值 5）

// 窗口深度 → 视空间距离（与 strip pass 的 ZView 同一份公式）。
float ZView(float d) {
    float ndc = 2.0 * d - 1.0;
    float denom = (uFar + uNear) - ndc * (uFar - uNear);
    // 退化相机守卫（near ≈ 0 / near ≈ far）：0/0 会变 NaN 并污染整帧。
    if (!(abs(denom) > 1e-6)) {
        return max(uNear, 1e-4);
    }
    return (2.0 * uNear * uFar) / denom;
}

void main() {
    // 手动深度测试：水面在这块不透明几何后面就整片丢弃（否则水会盖到挡在它前面的东西上）。
    float scene_z = ZView(texture(uSceneDepth, gl_FragCoord.xy / uViewport).r);
    float z = ZView(gl_FragCoord.z);
    if (z > scene_z + 0.02) {
        discard;
    }

    vec3 V = normalize(uCamPos - vWorld);
    float cos_theta = abs(V.y);                       // N = (0,1,0)，双面
    float F = uF0 + (1.0 - uF0) * pow(1.0 - cos_theta, uPower);

    vec3 color = mix(uBodyColor, uRimColor, F);
    float alpha = clamp(mix(uBodyAlpha, 1.0, F), 0.0, 1.0);
    FragColor = vec4(color, alpha);
}
)glsl";

}  // namespace jpov

#endif  // JPOV_SRC_WATER_WATER_SKIN_SHADER_H_
