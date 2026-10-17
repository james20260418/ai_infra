// JPOV ParticleRenderer —— 广告牌粒子画进**加权混合 OIT（WBO）分级纹理**（与 fog 平级）
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
//   pos(3) / misc(4: 宽,高,滚转,帧) / color(4) / vel(3)。flipbook 相邻帧混合。
// FS：采样 flipbook → 与**不透明场景深度**手动作深度测试（WBO 缓冲无深度附件）→ 按语义分两支：
//   遮挡（烟）：att0 = vec4(色·α, α)·w，att1 = α，att2 = z·α·w（WBO over 代数）。
//   加性（火/激光）：att0 = vec4(发射, 发射权重)、att1 不写（cov=0）、att2 = z·发射权重。
//   weight_wbo = max(min(1, maxRGB·α), α) · clamp(0.03/(1e-5+(z/5)^4), 1e-2, 3e3)（同 #176）。
//
// 合成：Composite() 全屏把半透 WBO 解析（C=accum.rgb/accum.a，覆盖=1−reveal）并以 over 混合
//   写进**当前绑定的目标**（HDR），随后由 fog 合成雾+玻璃 → 再后续。

#ifndef JPOV_SRC_PARTICLES_PARTICLE_RENDERER_H_
#define JPOV_SRC_PARTICLES_PARTICLE_RENDERER_H_

#include "tools/jpov/interface/camera.h"
#include "tools/jpov/interface/render_command.h"
#include "tools/jpov/src/instance_buffer.h"
#include "tools/jpov/src/shader_manager.h"
#include "tools/jpov/src/texture_manager.h"
#include "tools/jpov/src/wbo_buffer.h"

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
        float sp = length(aVel);
        vec3 u = (sp > 1e-4) ? (aVel / sp) : uCamUp;
        vec3 r = cross(u, normalize(toCam));
        if (length(r) < 1e-4) { r = uCamRight; }
        right = normalize(r);
        up = normalize(cross(right, u));
    }
    float c = cos(aMisc.z);
    float s = sin(aMisc.z);
    vec3 rr = right * c + up * s;
    vec3 uu = -right * s + up * c;
    vec2 size = aMisc.xy;
    if (uStretched == 1) {
        size.y *= (1.0 + length(aVel) * uStretchScale);
    }
    vec3 world = aPos + (corner.x * size.x) * rr + (corner.y * size.y) * uu;
    gl_Position = uViewProj * vec4(world, 1.0);
}
)glsl";

    // WBO 片元：flipbook 采样 + 场景深度手动测试 + 三附件输出（同 #176）。
    static constexpr const char* kFsWbo = R"glsl(
#version 330 core
in vec2 vUV;
in vec4 vColor;
flat in float vFrame;
uniform sampler2D uTex;
uniform int uHasTex;
uniform int uTilesX;
uniform int uTilesY;
uniform int uFlipbook;
uniform int uFrameBlend;
uniform int uAdditive;   // 1 = 加法（黑底无 alpha）→ 用亮度当覆盖
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
    vec4 tex;
    if (uHasTex == 0) {
        tex = vec4(1.0);
    } else if (uFlipbook == 1 && uTilesX >= 1 && uTilesY >= 1) {
        float total = float(uTilesX * uTilesY);
        float f0 = floor(vFrame);
        vec4 c0 = texture(uTex, CellUV(vUV, f0));
        if (uFrameBlend == 1) {
            float f1 = mod(f0 + 1.0, total);
            vec4 c1 = texture(uTex, CellUV(vUV, f1));
            tex = mix(c0, c1, vFrame - f0);
        } else {
            tex = c0;
        }
    } else {
        tex = texture(uTex, vUV);
    }
    vec4 col = vColor * tex;

    // 与不透明场景深度手动作深度测试（被几何遮挡 → 丢弃）。两种语义共用。
    vec2 uv = gl_FragCoord.xy / uViewport;
    float scene_z = ZView(texture(uSceneDepth, uv).r);
    float z = ZView(gl_FragCoord.z);
    if (z > scene_z + 0.02) {
        discard;
    }

    if (uAdditive == 1) {
        // ── 加性：纯发射，不遮挡 ──
        // e = 发射（RGB 已含贴图/粒子色）；黑底贴图天然为 0，无需「亮度当 alpha」的近似。
        vec3 e = col.rgb * col.a;
        float wgt = max(max(e.r, e.g), e.b);   // 发射权重（供合成端算 z̄）
        if (wgt <= 1e-5) {
            discard;
        }
        FragAccum = vec4(e, wgt);      // accum.rgb = Σ发射，accum.a = Σ权重（**不归一化**）
        FragReveal = 1.0;              // att1 混合为 (ZERO, ONE) ⇒ 不改变 reveal（cov=0）
        FragDepthAccum = z * wgt;
        return;
    }

    // ── 遮挡：烟 / 尘（WBO over 累积）──
    float a = col.a;
    if (a <= 1.0 / 255.0) {
        discard;
    }
    float w = WeightWbo(z, a, col.rgb);
    FragAccum = vec4(col.rgb * a, a) * w;
    FragReveal = a;
    FragDepthAccum = z * a * w;
}
)glsl";

    // 半透明目标分辨率系数（MVP：1/2）。将来扩为分级。
    static constexpr float kScale = 0.5f;

    // 初始化：编译两个 program + 建空 VAO。
    void Init(ShaderManager* shader_mgr);
    void Finalize();

    // 帧首：确保两张 1/2 WBO FBO（加性 / 遮挡）存在并 clear，重置本帧内容标志。
    // Pre-condition: Init 已调用。调用后方可多次 Draw（累加）。
    void Begin(int full_w, int full_h);

    // 把一批粒子按命令的 blend 语义**累加**进对应的 1/2 缓冲（加性 → wbo_add_，
    // alpha → wbo_over_；不 clear）。
    // Pre-condition: Init 已调用；cmd.particles 非空（空则 no-op）。
    void Draw(const ParticleRenderCommand& cmd,
              TextureManager& texture_mgr,
              const Camera& camera,
              const float view_proj[16],
              unsigned int scene_depth_tex,
              int full_w, int full_h);

    // 把本帧的非空缓冲作为透明层追加到集合（加性层 / 遮挡层各至多一个）。
    // 两层都空 ⇒ 不追加任何东西。顺序无关（合成器按代表深度排序）。
    void AppendLayers(TransparentLayerSet* layers /*output*/) const;

    bool has_content() const { return has_add_ || has_over_; }

private:
    ShaderManager* shader_mgr_ = nullptr;
    unsigned int prog_draw_ = 0;
    unsigned int vao_ = 0;
    // 两个 1/2 分辨率 WBO 缓冲：加性（发射，不遮挡）与遮挡（烟/尘）。
    // 分开的理由见 transparent_layer.h：加性合成可交换 ⇒ 一层不丢信息；
    // 遮挡层有覆盖语义 ⇒ 不能和发射混在同一个 reveal 上。
    WboBuffer wbo_add_;
    WboBuffer wbo_over_;
    bool has_add_ = false;
    bool has_over_ = false;

    InstanceBuffer pos_buf_{kParticlePosSpec};
    InstanceBuffer misc_buf_{kParticleMiscSpec};
    InstanceBuffer color_buf_{kParticleColorSpec};
    InstanceBuffer vel_buf_{kParticleVelSpec};
    bool initialized_ = false;
};

}  // namespace jpov

#endif  // JPOV_SRC_PARTICLES_PARTICLE_RENDERER_H_
