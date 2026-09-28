// JPOV BurningRenderer —— 「燃烧」特效渲染（实验）
//
// 与「火焰（Flame）」区分（重要，见 docs/jpov_effect_pass_design.md）：
//   - **火焰 Flame**：一个**自足的发光体**，锚在**一个点**上（炉灶/火把/篝火/火盆）。
//     尺寸由用户给绝对量。已有实现见 effect/fire_render。
//   - **燃烧 Burning**：一个**物体的状态/表面现象**，火**长在物体表面**上
//     （木头/柱子/船/建筑在烧）。火源由**物体几何**驱动，且**跟着物体走**。
//
// 本文件是「燃烧」的渲染侧：接收「燃烧体」命令（物体几何 + 强度 + 风向），
// 在**物体表面**采样出火舌并绘制（含烟）。
//
// 视觉模型（实验初始版本，照 Danis 的描述 + 参考图）：
//   ① 底部一圈「根部火舌」（常燃、贴地）；
//   ② 沿竖直方向、贴着物体表面**往上蔓延**的火舌点（越高越稀）；
//   ③ 火舌**左右摆动**（比火焰那版更大），**稀薄、不规则**（不是匀质色块）；
//   ④ 顶部飘起**烟**（灰、半透明、慢速上升）。
//
// 归并约束：同一批 draw 只能有一个混合模式 → 火（加法/alpha）与烟（alpha）分开成批。

#ifndef JPOV_BURNING_RENDERER_H_
#define JPOV_BURNING_RENDERER_H_

#include <vector>

#include "tools/jpov/interface/camera.h"
#include "tools/jpov/interface/mesh.h"
#include "tools/jpov/interface/render_command.h"

namespace jpov {

class BurningRenderer {
public:
    // ---- GLSL shader 源码 ----
    //
    // 调用方用 ShaderManager 编译：
    //   shader_mgr.GetOrCreate("burning", {kBurningVs, kBurningFs});
    //
    // kBurningVs：顶点已在 CPU 端展开为**世界坐标**（billboard 在 CPU 算），
    //             顶点着色器只做 MVP 变换 + 透传 UV（与 fire 同构）。
    //             顶点布局：loc0 = vec3 世界坐标；loc1 = vec2 UV（v: 底 0 → 顶 1）。
    static constexpr const char* kBurningVs = R"glsl(
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUv;
uniform mat4 uMVP;
out vec2 vUv;
void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
    vUv = aUv;
}
)glsl";

    // kBurningFs：「燃烧」火舌。与火焰不同：
    //   - 更**稀薄**（噪声挖洞更狠 → 断续的舌状，不是实心团）；
    //   - **左右摆动更大**（横向偏移随高度 + 时间增大）；
    //   - 底部**常燃根部**，向上**逐渐稀疏**（靠每簇的 life 门槛）。
    static constexpr const char* kBurningFs = R"glsl(
#version 330 core
in vec2 vUv;
out vec4 FragColor;
uniform float uTime;
uniform float uSpeed;
uniform float uNoiseScale;
uniform vec3  uColorCore;
uniform vec3  uColorOuter;
uniform float uIntensity;
uniform float uDensity;    // 0..1：稀薄程度（越小越断续）

float hash12(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453123);
}
float vnoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    float a = hash12(i);
    float b = hash12(i + vec2(1.0, 0.0));
    float c = hash12(i + vec2(0.0, 1.0));
    float d = hash12(i + vec2(1.0, 1.0));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}
float fbm(vec2 p) {
    float v = 0.0, amp = 0.5;
    for (int i = 0; i < 3; ++i) { v += amp * vnoise(p); p *= 2.0; amp *= 0.5; }
    return v;
}

void main() {
    vec2 uv = vUv;                          // x:0→1; y:0→1（底→顶）
    float t = uTime * uSpeed;
    float y = uv.y;

    // 横向摆动：幅度随高度增大（越往上甩得越开）——比火焰那版更大。
    float sway = 0.10 * sin(t * 1.9 + y * 4.0)
               + 0.07 * sin(t * 3.3 + y * 7.0 + 1.7);
    float x = uv.x - 0.5 + sway * y;

    // 宽度包络：底部宽（根部）、顶部细。
    float w = 0.34 * (1.0 - 0.55 * y);
    float dx = abs(x);

    // 稀薄：噪声挖洞（uDensity 越小越断）。
    float n = fbm(vec2(uv.x * uNoiseScale + sway * 2.0,
                       uv.y * uNoiseScale * 0.55 - t * 1.5));
    float erode = smoothstep(0.55 - 0.35 * uDensity, 0.92, n);

    // 根部常燃：底部一圈始终有火（不依赖噪声门槛）。
    float rootMask = (1.0 - smoothstep(0.0, 0.16, y)) * (1.0 - smoothstep(0.0, w, dx));

    // 火舌：底宽顶细的剪影 × 噪声侵蚀；越高越稀（life 随高度衰减）。
    float tall = smoothstep(1.0, 0.15, y);          // 越高越弱
    float body = (1.0 - smoothstep(w * 0.30, w, dx)) * tall;
    float shape = max(rootMask, body * erode);

    float heat = clamp(shape * (0.5 + 0.9 * n) * (1.0 - 0.25 * y), 0.0, 1.0);
    vec3 tone = mix(uColorOuter, uColorCore, smoothstep(0.08, 0.80, heat));
    float a = clamp(shape * (0.40 + 0.60 * heat), 0.0, 1.0);
    FragColor = vec4(tone * pow(heat, 0.6) * uIntensity, a);
}
)glsl";

    // kSmokeVs：与 kBurningVs 同构（同一顶点布局），单独命名以便独立管理。
    static constexpr const char* kSmokeVs = kBurningVs;

    // kSmokeFs：烟。灰白、半透明、慢速上升、边缘软；同样被噪声侵蚀成絮状。
    static constexpr const char* kSmokeFs = R"glsl(
#version 330 core
in vec2 vUv;
out vec4 FragColor;
uniform float uTime;
uniform float uSpeed;
uniform float uNoiseScale;
uniform vec4  uColor;      // 烟的基色与最大不透明度
uniform float uIntensity;

float hash12(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453123);
}
float vnoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    float a = hash12(i);
    float b = hash12(i + vec2(1.0, 0.0));
    float c = hash12(i + vec2(0.0, 1.0));
    float d = hash12(i + vec2(1.0, 1.0));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}
float fbm(vec2 p) {
    float v = 0.0, amp = 0.5;
    for (int i = 0; i < 3; ++i) { v += amp * vnoise(p); p *= 2.0; amp *= 0.5; }
    return v;
}

void main() {
    vec2 uv = vUv;
    float t = uTime * uSpeed;
    float y = uv.y;

    // 烟往上飘 + 越上越宽、越淡。
    float sway = 0.18 * sin(t * 0.9 + y * 3.0) + 0.10 * sin(t * 1.7 + y * 5.0);
    float x = uv.x - 0.5 + sway * y;
    float w = 0.22 + 0.30 * y;                       // 上宽（扩散）
    float dx = abs(x) / w;

    float n = fbm(vec2(uv.x * uNoiseScale + sway,
                       uv.y * uNoiseScale * 0.6 - t * 0.7));
    float body = (1.0 - smoothstep(0.35, 1.0, dx));
    float shape = body * smoothstep(0.30, 0.85, n) * (1.0 - smoothstep(0.5, 1.0, y));
    float a = shape * uColor.a * uIntensity;
    FragColor = vec4(uColor.rgb, a);
}
)glsl";

    // ---- 绘制 ----
    //
    // 画一条「燃烧体」命令：把该物体分解成「表面火舌簇」（+ 烟簇），
    // 每簇在 CPU 端展开为圆柱 billboard quad，逐簇绘制。
    //
    // 参数说明见 interface/render_command.h 的 BurningCommand。
    // stream_vbo / fire_prog / smoke_prog / time 由 Renderer 提供。
    //
    // Pre-condition: cmd.strength >= 0 且 cmd.extent/height > 0
    static void DrawBurning(const BurningCommand& cmd, const Camera& cam,
                            unsigned int stream_vbo,
                            unsigned int fire_prog, unsigned int smoke_prog,
                            const float mvp[16], float time);

    // ---- 火舌簇采样（纯 CPU，GL-free，可单测）----
    //
    // 由「燃烧体」几何求出**表面火舌簇**的世界坐标列表：
    //   - 底部一圈（常燃根部）；
    //   - 沿竖直方向、贴表面往上蔓延的稀疏簇（越高越稀）；
    //   - 用确定性哈希（非 rand()）保证可复现。
    //
    // base/up/front：物体的世界摆放（与 Object3DCommand 同语义）。
    // half_extents：物体半尺寸（x/y/z）。
    // strength：0~1，控制簇数量与爬升高度。
    // seed：确定性种子（同 seed + 同参数 → 同结果）。
    static std::vector<Vec3f> SampleBurningPoints(
        const Vec3f& base, const Vec3f& up, const Vec3f& front,
        const Vec3f& half_extents, float strength, uint32_t seed);
};

}  // namespace jpov

#endif  // JPOV_BURNING_RENDERER_H_
