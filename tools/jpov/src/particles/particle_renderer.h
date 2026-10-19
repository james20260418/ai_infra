// JPOV ParticleRenderer —— 广告牌粒子画进**加权混合 OIT（WBO）分级纹理**（与 fire_fog 平级）
//
// 定位（Danis 2026-10-14 定）：用户给出粒子的 flipbook + 一票信息（ParticleRenderCommand），
// 本渲染器把结果画进 **WBO 半透明累积纹理**（accum/reveal/depth），供后续**合成并交给 froxel**。
// 与内置 object3d 透明（#176，1× 玻璃 FBO）**共用同一套 McGuire WBO 代数**。
//
// ⭐ 为什么要 WBO + 分级：半透明 overdraw 在 sprite 栈内**无解**（透明不写深度 → early-Z 失效）。
//   对策：① 渲进**低分辨率**目标（成本∝res²）——本版先做 **1/2**；② WBO 顺序无关（免排序）。
//   将来可扩到多级（1/2、1/4）并按需路由（见 docs/jpov_fragment_overdraw_design.md）。
//
// 画法：instanced 广告牌（VS 由 gl_VertexID 生成面片四角），per-instance attr loc0..3：
//   pos(3) / misc(4: 半宽,半高,滚转,帧) / color(4) / vel(3)。flipbook 相邻帧混合。
//   朝向：AlignMode（kVertical 火；kVelocity 局部 Y = 速度 ⇒ stretched 沿速度拉条）。
// FS：采样 flipbook → 与**不透明场景深度**手动作深度测试（玻璃 FBO 无深度缓冲）→
//   输出 WBO 三附件（与 #176 object3d blend 同构）：
//     att0 accum : vec4(色·α, α)·w      （blend ONE,ONE）
//     att1 reveal: α                    （blend ZERO,ONE_MINUS_SRC_COLOR）
//     att2 depth : z_view·α·w           （blend ONE,ONE）
//   weight_wbo = max(min(1, maxRGB·α), α) · clamp(0.03/(1e-5+(z/5)^4), 1e-2, 3e3)（同 #176）。
//
// 合成：Composite() 全屏把半透 WBO 解析（C=accum.rgb/accum.a，覆盖=1−reveal）并以 over 混合
//   写进**当前绑定的目标**（HDR），随后由 fire_fog 合成雾+玻璃 → 再后续。

#ifndef JPOV_SRC_PARTICLES_PARTICLE_RENDERER_H_
#define JPOV_SRC_PARTICLES_PARTICLE_RENDERER_H_

#include "tools/jpov/interface/camera.h"
#include "tools/jpov/interface/render_command.h"
#include "tools/jpov/src/instance_buffer.h"
#include "tools/jpov/src/shader_manager.h"
#include "tools/jpov/src/texture_manager.h"

namespace jpov {

// per-instance 属性布局（本渲染器独立拥有）。
inline constexpr InstanceAttrSpec kParticlePosSpec{/*base_loc*/ 0, /*slot_count*/ 1,
                                                   /*slot_components*/ 3, /*stride*/ 3};
inline constexpr InstanceAttrSpec kParticleMiscSpec{/*base_loc*/ 1, /*slot_count*/ 1,
                                                    /*slot_components*/ 4, /*stride*/ 4};
inline constexpr InstanceAttrSpec kParticleColorSpec{/*base_loc*/ 2, /*slot_count*/ 1,
                                                     /*slot_components*/ 4, /*stride*/ 4};
inline constexpr InstanceAttrSpec kParticleVelSpec{/*base_loc*/ 3, /*slot_count*/ 1,
                                                   /*slot_components*/ 3, /*stride*/ 3};

class ParticleRenderer {
public:
    ParticleRenderer();
    ~ParticleRenderer();

    ParticleRenderer(const ParticleRenderer&) = delete;
    ParticleRenderer& operator=(const ParticleRenderer&) = delete;

    // 顶点着色器：gl_VertexID 生成面片四角 + 广告牌朝向 + 逐粒子滚转/拉伸。
    static constexpr const char* kVs = R"glsl(
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec4 aMisc;   // (width, height, rotation, frame)
layout(location = 2) in vec4 aColor;
layout(location = 3) in vec3 aVel;
uniform mat4 uViewProj;
uniform vec3 uCamRight;
uniform vec3 uCamUp;
uniform vec3 uCamPos;
uniform vec3 uWorldUp;
uniform int  uAlign;
uniform int  uStretched;
uniform float uStretchScale;
out vec2 vUV;
out vec4 vColor;
out vec3 vWorld;
flat out float vFrame;
void main() {
    vec2 corner = vec2((gl_VertexID == 1 || gl_VertexID == 3) ? 1.0 : -1.0,
                       (gl_VertexID >= 2) ? 1.0 : -1.0);
    vUV = corner * 0.5 + 0.5;
    vColor = aColor;
    vFrame = aMisc.w;
    vec3 toCam = uCamPos - aPos;
    vec3 right;
    vec3 up;
    if (uAlign == 0) {
        right = uCamRight;
        up = uCamUp;
    } else if (uAlign == 1) {
        vec3 d = normalize(toCam);
        vec3 r = cross(uWorldUp, d);
        if (length(r) < 1e-4) { r = uCamRight; }
        right = normalize(r);
        up = uWorldUp;
    } else if (uAlign == 2) {
        vec3 n = normalize(toCam);
        vec3 r = cross(uWorldUp, n);
        if (length(r) < 1e-4) { r = uCamRight; }
        right = normalize(r);
        up = normalize(cross(n, right));
    } else {
        // kVelocity：面片平面**包含**速度向量（局部 Y = 速度），法线朝相机。
        //   速度 ∥ 视线时 cross(u, toCam) 退化 → 逐级换参考轴（两次几乎同时退化概率可忽略）。
        float sp = length(aVel);
        vec3 u = (sp > 1e-4) ? (aVel / sp) : uCamUp;   // 局部 Y = 速度方向
        vec3 r = cross(u, normalize(toCam));           // ⟂ 速度 且 ⟂ 视线
        if (length(r) < 1e-4) {
            r = cross(u, uCamRight);
        }
        if (length(r) < 1e-4) {
            r = cross(u, uCamUp);
        }
        right = normalize(r);
        up = u;
    }
    float c = cos(aMisc.z);
    float s = sin(aMisc.z);
    vec3 rr = right * c + up * s;
    vec3 uu = -right * s + up * c;
    vec2 size = aMisc.xy;
    // 拉伸沿**滚转后的局部 Y**（uu）。kVelocity 下局部 Y = 速度 ⇒ 沿速度拉成条；
    // 其余对齐模式下局部 Y 是「相机上 / 世界上」，拉出来只是「高个儿 sprite」，不要当物理拉丝用。
    if (uStretched == 1) {
        size.y *= (1.0 + length(aVel) * uStretchScale);
    }
    vec3 world = aPos + (corner.x * size.x) * rr + (corner.y * size.y) * uu;
    vWorld = world;
    gl_Position = uViewProj * vec4(world, 1.0);
}
)glsl";

    // WBO 片元：flipbook 采样 + 场景深度手动测试 + 三附件输出（同 #176）。
    static constexpr const char* kFsWbo = R"glsl(
#version 330 core
in vec2 vUV;
in vec4 vColor;
in vec3 vWorld;
flat in float vFrame;
uniform sampler2D uTex;
uniform int uHasTex;
uniform int uTilesX;
uniform int uTilesY;
uniform int uFlipbook;
uniform int uFrameBlend;
uniform int uAdditive;      // 1 = 加法（现在也直接用 col.a 当覆盖）
uniform float uEdgeFeather; // 边缘羽化阈值（0=关闭）
uniform float uUvDistort;   // 形状扰动幅度（0=关闭）
uniform float uTime;        // 时间（秒）
uniform sampler2D uSceneDepth;
uniform vec2 uViewport;   // 本 pass 的像素尺寸（1/2 分辨率）
uniform float uNear;
uniform float uFar;
layout(location = 0) out vec4 FragAccum;
layout(location = 1) out float FragReveal;
layout(location = 2) out float FragDepthAccum;
vec2 CellUV(vec2 uv, float f) {
    float col = mod(f, float(uTilesX));
    float row = floor(f / float(uTilesX));
    float vrow = float(uTilesY - 1) - row;
    return vec2((uv.x + col) / float(uTilesX), (uv.y + vrow) / float(uTilesY));
}
// 窗口深度 [0,1] → 视空间深度（正，米）。
float ZView(float d) {
    float ndc = 2.0 * d - 1.0;
    return (2.0 * uNear * uFar) / ((uFar + uNear) - ndc * (uFar - uNear));
}
float WeightWbo(float z, float a, vec3 c) {
    float mx = max(max(c.r, c.g), c.b);
    float w = max(min(1.0, mx * a), a);
    float df = clamp(0.03 / (1e-5 + pow(z / 5.0, 4.0)), 1e-2, 3e3);
    return w * df;
}
void main() {
    // 形状扰动：每粒子相位（用世界坐标哈希）+ 时间 → UV 摆动。
    // 越靠上（vUV.y 大）扰动越大，火焰尖端会"撕"得更明显；根部基本不动。
    vec2 suv = vUV;
    if (uUvDistort > 0.0) {
        float ph = fract(sin(dot(vWorld.xy, vec2(12.9898, 78.233))) * 43758.5453) * 6.2831853;
        float amp = uUvDistort * (0.35 + 0.65 * suv.y);
        suv += amp * vec2(
            sin(suv.y * 7.0 + uTime * 2.6 + ph),
            cos(suv.x * 5.0 - uTime * 2.1 + ph * 1.3)) * 0.5;
    }
    vec4 tex;
    if (uHasTex == 0) {
        tex = vec4(1.0);
    } else if (uFlipbook == 1 && uTilesX >= 1 && uTilesY >= 1) {
        float total = float(uTilesX * uTilesY);
        float f0 = floor(vFrame);
        vec4 c0 = texture(uTex, CellUV(suv, f0));
        if (uFrameBlend == 1) {
            float f1 = mod(f0 + 1.0, total);
            vec4 c1 = texture(uTex, CellUV(suv, f1));
            tex = mix(c0, c1, vFrame - f0);
        } else {
            tex = c0;
        }
    } else {
        tex = texture(uTex, suv);
    }
    vec4 col = vColor * tex;
    // **预乘契约**：RGB = 颜色（可 HDR），A = 覆盖。
    // 加法与半透明都直接用 col.a 当覆盖；黑底贴图由素材把 alpha 写成 0（或走 kAlpha 通道）。
    float a = col.a;
    // 边缘羽化（0 = 关闭）：把低 alpha 区做一次软过渡，消掉采样造成的硬边。
    if (uEdgeFeather > 0.0) {
        a = smoothstep(0.0, max(uEdgeFeather, 1e-3), a);
    }
    if (a <= 1.0 / 255.0) {
        discard;
    }
    // 与不透明场景深度手动作深度测试（被几何遮挡 → 丢弃）。
    vec2 uv = gl_FragCoord.xy / uViewport;
    float scene_z = ZView(texture(uSceneDepth, uv).r);
    float z = ZView(gl_FragCoord.z);
    if (z > scene_z + 0.02) {
        discard;
    }
    float w = WeightWbo(z, a, col.rgb);
    FragAccum = vec4(col.rgb * a, a) * w;
    FragReveal = a;
    FragDepthAccum = z * a * w;
}
)glsl";

    // 合成：全屏把 1/2 WBO 解析并以 over 混合写进当前目标（预乘输出 + ONE,1−SRC_ALPHA）。
    static constexpr const char* kVsComp = R"glsl(
#version 330 core
out vec2 vUV;
void main() {
    vec2 c = vec2((gl_VertexID == 1 || gl_VertexID == 3) ? 1.0 : -1.0,
                  (gl_VertexID >= 2) ? 1.0 : -1.0);
    vUV = c * 0.5 + 0.5;
    gl_Position = vec4(c, 0.0, 1.0);
}
)glsl";
    static constexpr const char* kFsComp = R"glsl(
#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uAccum;
uniform sampler2D uReveal;
void main() {
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

    // 半透明目标分辨率系数（MVP：1/2）。将来扩为分级。
    static constexpr float kScale = 0.5f;

    // 初始化：编译两个 program + 建空 VAO。
    void Init(ShaderManager* shader_mgr);
    void Finalize();

    // 帧首：确保 1/2 WBO FBO 存在并 clear（accum=0/reveal=1/depth=0），重置本帧内容标志。
    // Pre-condition: Init 已调用。调用后方可多次 Draw（累加）。
    void Begin(int full_w, int full_h);

    // 把一批粒子以 WBO **累加**画进本渲染器的 1/2 分辨率 FBO（不 clear）。
    // Pre-condition: Init 已调用；cmd.particles 非空（空则 no-op）。
    void Draw(const ParticleRenderCommand& cmd,
              TextureManager& texture_mgr,
              const Camera& camera,
              const float view_proj[16],
              unsigned int scene_depth_tex,
              int full_w, int full_h);

    // 把本帧 WBO 结果解析并 over 合成进**当前绑定的目标**（HDR）。无粒子则 no-op。
    void Composite(int full_w, int full_h);

    bool has_content() const { return has_content_; }

private:
    void EnsureWbo(int w, int h);
    void DestroyWbo();

    ShaderManager* shader_mgr_ = nullptr;
    unsigned int prog_draw_ = 0;
    unsigned int prog_comp_ = 0;
    unsigned int vao_ = 0;
    unsigned int wbo_fbo_ = 0, accum_tex_ = 0, reveal_tex_ = 0, depth_tex_ = 0;
    int wbo_w_ = 0, wbo_h_ = 0;
    bool has_content_ = false;

    InstanceBuffer pos_buf_{kParticlePosSpec};
    InstanceBuffer misc_buf_{kParticleMiscSpec};
    InstanceBuffer color_buf_{kParticleColorSpec};
    InstanceBuffer vel_buf_{kParticleVelSpec};
    bool initialized_ = false;
};

}  // namespace jpov

#endif  // JPOV_SRC_PARTICLES_PARTICLE_RENDERER_H_
