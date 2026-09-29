// JPOV VolumetricRenderer —— 体积特效渲染（预实验：真 3D 体积团）
//
// 定位：并列于 fire / burning / particle 的第四条特效线。**纯 CPU 模拟**不在
// 本文件（模拟层仍可复用 FireParticleEmitter），这里只把 VolumetricCommand
// 画成"真 3D 体积"。
//
// 与 billboard 的本质区别（这是本文件存在的理由）：
//   billboard 把形状存在**一片纸**里 → 从正上方看纸被"侧看"，整片塌成一条线。
//   本渲染器把形状存在**世界空间的 3D 密度场**里：
//     几何 = 相机朝向的**屏幕包围盒 quad**（只是"信封"）；
//     fragment = 反算世界射线 → ray-sphere 求交 → 沿射线 marching 采样
//                程序化 3D 密度场 → 用**场景线性深度**裁剪被挡部分。
//   ⇒ 俯视/侧视/绕行都一致，不穿帮。
//
// 密度场（世界空间，纯程序化，无资产纹理）：
//   球包络 × 绕 Y 轴旋涡扭转（越靠底越强）× 域扭曲(domain warp) fbm。
//   "形状族 = 参数组合"：软团=弱旋涡大包络 / 火舌=窄高 / 涡流=强旋涡弱域扭曲。
//
// 火是发光体 ⇒ 加法混合 ⇒ 天然免排序（本 MVP 只用加法）。

#ifndef JPOV_VOLUMETRIC_RENDERER_H_
#define JPOV_VOLUMETRIC_RENDERER_H_

#include "tools/jpov/interface/camera.h"
#include "tools/jpov/interface/render_command.h"

namespace jpov {

class VolumetricRenderer {
public:
    // 顶点：loc0 = vec3 包围盒角点的世界坐标。
    static constexpr const char* kVolumetricVs = R"glsl(
#version 330 core
layout(location = 0) in vec3 aPos;
uniform mat4 uVP;
out vec3 vWorld;
void main() {
    vWorld = aPos;
    gl_Position = uVP * vec4(aPos, 1.0);
}
)glsl";

    // 片元：逐像素 ray-sphere + marching 程序化 3D 密度场 + 场景深度裁剪。
    //   uSceneDepth: 场景**线性相机距离**（float 颜色纹理；见 Renderer::
    //                DrawSceneDepthPass —— llvmpipe 下不能采 depth texture）。
    //   uSwirl     : 绕 Y 轴旋涡强度。
    static constexpr const char* kVolumetricFs = R"glsl(
#version 330 core
in vec3 vWorld;
out vec4 FragColor;

uniform vec3  uCamPos;
uniform vec3  uCenter;
uniform float uRadius;
uniform float uHeat;
uniform float uTime;
uniform float uNoiseFreq;      // 噪声频率（纹理尺度：越大越细碎）
uniform int   uOctaves;        // fbm 倍频数（纹理复杂度）
uniform float uDensityThreshold;  // 密度阈值（越大越空透）
uniform float uSwirl;          // 绕 Y 轴旋涡强度
uniform float uIntensity;
uniform vec3  uColorCore;
uniform vec3  uColorOuter;
uniform vec2  uResolution;
uniform sampler2D uSceneDepth;   // .r = 线性相机距离

// ---- 3D value noise + fbm ----
float hash31(vec3 p) {
    p = fract(p * 0.3183099 + vec3(0.1, 0.2, 0.3));
    p *= 17.0;
    return fract(p.x * p.y * p.z * (p.x + p.y + p.z));
}
float vnoise3(vec3 x) {
    vec3 i = floor(x), f = fract(x);
    vec3 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(mix(hash31(i + vec3(0,0,0)), hash31(i + vec3(1,0,0)), u.x),
                   mix(hash31(i + vec3(0,1,0)), hash31(i + vec3(1,1,0)), u.x), u.y),
               mix(mix(hash31(i + vec3(0,0,1)), hash31(i + vec3(1,0,1)), u.x),
                   mix(hash31(i + vec3(0,1,1)), hash31(i + vec3(1,1,1)), u.x), u.y), u.z);
}
// oct：倍频数（1=只剩一团大低频，越简洁；5=细节最多）。上限常量 6，超出丢弃。
float fbm3(vec3 p, int oct) {
    float v = 0.0, a = 0.5;
    for (int i = 0; i < 6; ++i) {
        if (i >= oct) {
            break;
        }
        v += a * vnoise3(p);
        p = p * 2.03 + vec3(1.7, 9.2, 3.1);
        a *= 0.5;
    }
    return v;
}

// 涡流体密度（世界空间）：球包络 × 绕轴旋涡扭转 × 域扭曲 fbm。
float density(vec3 p) {
    vec3 q = p - uCenter;
    float R = uRadius;
    float env = smoothstep(1.0, 0.15, length(q) / R);
    if (env <= 0.0) {
        return 0.0;
    }
    float h = clamp(q.y / R * 0.5 + 0.5, 0.0, 1.0);
    float ang = uSwirl * (1.0 - h) / (length(q.xz) / R + 0.22) + uTime * 0.9;
    float s = sin(ang), c = cos(ang);
    vec3 qq = vec3(q.x * c - q.z * s, q.y, q.x * s + q.z * c);
    vec3 pw = qq * (uNoiseFreq / R) + vec3(0.0, -uTime * 1.1, 0.0);
    vec3 warp = vec3(fbm3(pw, uOctaves), fbm3(pw + vec3(5.2, 1.3, 2.7), uOctaves),
                     fbm3(pw + vec3(9.1, 4.4, 7.7), uOctaves)) - 0.5;
    float d = fbm3(pw + warp * 1.5, uOctaves);
    return env * max(0.0, d - uDensityThreshold) * (0.55 + 0.9 * uHeat);
}

void main() {
    vec3 ro = uCamPos;
    vec3 rd = normalize(vWorld - uCamPos);
    vec3 oc = ro - uCenter;
    float R = uRadius;
    float b = dot(oc, rd);
    float cc = dot(oc, oc) - R * R;
    float disc = b * b - cc;
    if (disc <= 0.0) {
        discard;
    }
    float sq = sqrt(disc);
    float t0 = max(0.0, -b - sq);
    float t1 = -b + sq;
    if (t1 <= t0) {
        discard;
    }
    // 场景深度裁剪：被不透明物体挡住的部分不画。
    vec2 uv = gl_FragCoord.xy / uResolution;
    float sceneT = texture(uSceneDepth, uv).r;
    float tmax = min(t1, sceneT);
    if (tmax <= t0) {
        discard;
    }

    const int N = 40;
    float dt = (tmax - t0) / float(N);
    vec3 acc = vec3(0.0);
    for (int i = 0; i < N; ++i) {
        float t = t0 + (float(i) + 0.5) * dt;
        vec3 p = ro + rd * t;
        float d = density(p);
        if (d > 0.0) {
            float heat = clamp(uHeat * (1.0 - 0.5 * length(p - uCenter) / R) * 1.4, 0.0, 1.0);
            vec3 tone = mix(uColorOuter, uColorCore, smoothstep(0.1, 0.8, heat));
            acc += tone * (d * dt * 7.0 * uIntensity);
        }
    }
    FragColor = vec4(acc, 1.0);   // 加法混合（GL_ONE, GL_ONE）
}
)glsl";

    // ---- 绘制 ----
    //
    // 画一条体积命令：CPU 端构造**相机朝向**的包围盒 quad（边长 ~2.5R，完整
    // 朝向相机：右=相机右、上=相机上），fragment 内做 ray-march。
    //   stream_vbo      : 复用的动态 VBO（Renderer 自带）。
    //   scene_depth_tex : 场景线性深度纹理（float 颜色纹理，.r=距离）。
    static void DrawVolumetric(const VolumetricCommand& cmd, const Camera& cam,
                               unsigned int stream_vbo, unsigned int prog,
                               const float mvp[16], int fbo_w, int fbo_h,
                               unsigned int scene_depth_tex, float time);
};

}  // namespace jpov

#endif  // JPOV_VOLUMETRIC_RENDERER_H_
