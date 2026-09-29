// JPOV ParticleRenderer —— 粒子特效渲染（实验：三种「形状族」）
//
// 定位：并列于 fire / burning 的第三条特效线。**纯 CPU 模拟**（见
// effect/particle_fire/fire_particles.h），本文件只负责把 ParticleCommand
// 快照画成面片。
//
// 三种风格（同一 shader，靠 uStyle 分支；实验阶段先不为每族单开 program）：
//   kSoftPuff：软边圆团 —— 面积大、圆、半透明叠加（“烟变火”那类）。
//   kTongue  ：细长上尖火舌 —— 底宽顶尖、强摆动（像蜡烛/火炬的舌头）。
//   kVortex  ：涡流团 —— 被小尺度湍流卷起/撕碎（形状最不规则）。
//
// 三族共用一个顶点布局（world + uv），只是 fragment 的形状/颜色规律不同。

#ifndef JPOV_PARTICLE_RENDERER_H_
#define JPOV_PARTICLE_RENDERER_H_

#include "tools/jpov/interface/camera.h"
#include "tools/jpov/interface/render_command.h"

namespace jpov {

class ParticleRenderer {
public:
    // 顶点：loc0 = vec3 世界坐标；loc1 = vec2 UV（u:0→1 左→右；v:0→1 底→顶）。
    static constexpr const char* kParticleVs = R"glsl(
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

    // 片元：uStyle 选择形状族；uHeat 控颜色（低热=外焰色，高热=核心色）。
    static constexpr const char* kParticleFs = R"glsl(
#version 330 core
in vec2 vUv;
out vec4 FragColor;
uniform float uTime;        // 特效时钟 + 该粒子相位
uniform float uHeat;        // 0..1 当前热度
uniform float uAlpha;       // 0..1 额外不透明度乘子
uniform int   uStyle;       // 0=soft puff, 1=tongue, 2=vortex
uniform vec3  uColorCore;
uniform vec3  uColorOuter;

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
    float t = uTime;
    float m = 0.0;          // 形状掩码 0..1
    float heatMod = 1.0;    // 形状内部的热度调制

    if (uStyle == 0) {
        // ── 软边圆团：径向软衰减 × 缓慢噪声；中心更热、边缘更淡 ──
        vec2 d = (uv - 0.5) * 2.0;
        float r = length(d);
        float soft = 1.0 - smoothstep(0.15, 1.0, r);
        float n = fbm(uv * 2.6 + vec2(0.0, -t * 0.6));
        m = soft * (0.55 + 0.75 * n);
        heatMod = 1.0 - 0.55 * r;
    } else if (uStyle == 1) {
        // ── 细长上尖火舌：底宽顶尖 + 双频摆动 + 噪声挖洞 ──
        float y = uv.y;
        float sway = 0.18 * sin(t * 2.2 + y * 5.0) + 0.10 * sin(t * 3.7 + y * 9.0);
        float x = uv.x - 0.5 + sway * y;
        float w = 0.44 * (1.0 - 0.78 * y);
        float body = 1.0 - smoothstep(w * 0.35, w, abs(x));
        float n = fbm(vec2(uv.x * 3.2 + sway * 2.0, uv.y * 2.0 - t * 1.6));
        m = body * smoothstep(0.22, 0.80, n) * (1.0 - smoothstep(0.60, 1.0, y));
        heatMod = 1.0 - 0.70 * y;
    } else {
        // ── 涡流团：被卷起/撕碎（扭动大、破碎）──
        float y = uv.y;
        float swirl = 0.30 * sin(t * 1.3 + y * 3.0) + 0.20 * sin(t * 2.1 + y * 6.0 + 1.0);
        float x = uv.x - 0.5 + swirl * (0.35 + y);
        float r = length(vec2(x * 1.05, (y - 0.5) * 0.85));
        float soft = 1.0 - smoothstep(0.15, 0.98, r * 1.9);
        float n = fbm(vec2(uv.x * 3.0 + swirl, uv.y * 2.4 - t * 1.1));
        m = soft * smoothstep(0.28, 0.85, n * 1.15);
        heatMod = 1.0 - 0.50 * r;
    }

    float heat = clamp(uHeat * heatMod * 1.25, 0.0, 1.0);
    vec3 tone = mix(uColorOuter, uColorCore, smoothstep(0.05, 0.80, heat));
    float a = clamp(m * uAlpha, 0.0, 1.0);
    FragColor = vec4(tone * (0.35 + 0.95 * heat), a);
}
)glsl";

    // ---- 绘制 ----
    //
    // 画一条粒子命令：CPU 端展开为圆柱 billboard quad（宽=size，高=size*aspect）。
    static void DrawParticleQuad(const ParticleCommand& cmd, const Camera& cam,
                                 unsigned int stream_vbo, unsigned int prog,
                                 const float mvp[16], float time);
};

}  // namespace jpov

#endif  // JPOV_PARTICLE_RENDERER_H_
