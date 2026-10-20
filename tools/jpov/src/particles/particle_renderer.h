// JPOV ParticleRenderer —— 广告牌粒子画进**加权混合 OIT（WBO）分级纹理**（与 fire_fog 平级）
//
// 定位（Danis 2026-10-14 定）：用户给出粒子的 flipbook + 一票信息（ParticleRenderCommand），
// 本渲染器把结果画进 **WBO 半透明累积纹理**（覆盖集 accum/reveal + 加法集 add），
// 供后续**合成并交给 froxel**。
// 与内置 object3d 透明（#176，1× 玻璃 FBO）**共用同一套 McGuire WBO 代数**。
//
// ⭐ 为什么要 WBO + 分级：半透明 overdraw 在 sprite 栈内**无解**（透明不写深度 → early-Z 失效）。
//   对策：① 渲进**低分辨率**目标（成本∝res²）——本版先做 **1/2**；② WBO 顺序无关（免排序）。
//   将来可扩到多级（1/2、1/4）并按需路由（见 docs/jpov_fragment_overdraw_design.md）。
//
// 画法：instanced 广告牌（VS 由 gl_VertexID 生成面片四角），per-instance attr loc0..3：
//   pos(3) / misc(4: 宽,高,滚转,帧) / color(4) / vel(3)。flipbook 相邻帧混合。
// FS：采样 flipbook → 与**不透明场景深度**手动作深度测试（玻璃 FBO 无深度缓冲）→
//   输出三个附件（覆盖集两路 + 加法集一路）：
//     att0 accum : vec4(色·α, α)·w      （blend ONE,ONE）
//     att1 reveal: α                    （blend ZERO,ONE_MINUS_SRC_COLOR）
//     att2 add   : 色·α（真加法时）      （blend ONE,ONE）
//   weight_wbo = max(min(1, maxRGB·α), α) · clamp(0.03/(1e-5+(z/5)^4), 1e-2, 3e3)（同 #176）。
//   （曾有 depth 附件 = z_view·α·w，但 Composite 从未采样它 —— 纯带宽浪费，已删。）
//
// 深度判据两种模式（ParticleRenderCommand::soft_depth_m）：
//   关闭：`z > scene_z + 2cm` 即丢弃 —— 粒子与几何相交处是一条硬切边。
//   开启：按 d = scene_z − z（视空间米）线性淡出 α，d ∈ (0, 过渡距离) 内平滑过渡。
//   两种模式**都**发生在 WBO 写入之前，故覆盖率解析 1−Π(1−α) 始终自洽。
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
    } else if (uAlign == 4) {
        // 自适应 kViewVerticalBlend：侧视 ≈ vertical（roll 锁定），俯视 → view。
        // t = |dot(toCam, world_up)|：0 = 平视、1 = 正俯视；只在俯视 25° 内切换，
        // 避免过渡区基向量频繁变化导致的抖动。
        vec3 n = normalize(toCam);
        float t = abs(dot(n, uWorldUp));
        float w = smoothstep(0.55, 0.95, t);
        vec3 r_v = cross(uWorldUp, n);
        if (length(r_v) < 1e-4) { r_v = uCamRight; }
        r_v = normalize(r_v);
        vec3 u_v = normalize(cross(n, r_v));
        right = normalize(mix(r_v, uCamRight, w));
        up = normalize(mix(u_v, uCamUp, w));
        // 混合后 right/up 不再严格垂直 → 去掉 up 的 right 分量（否则卡片被剪切）。
        up = normalize(up - dot(up, right) * right);
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

    // 片元：UV 扰动 + flipbook 采样 + 场景深度手动测试（硬判据 / 软深度）+ 三路输出
    //   （覆盖集 accum/reveal，加法集 add；三种混合路径由 uBlendPath 选）。
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
uniform sampler2D uSceneDepth;
uniform vec2 uViewport;   // 本 pass 的像素尺寸（1/2 分辨率）
uniform float uNear;
uniform float uFar;
uniform int uSoftDepthEnabled;   // 1 = 软深度（按场景深度差淡出 α），0 = 旧硬判据
uniform float uSoftDepthM;       // 淡出过渡距离（视空间米；仅 enabled 时有意义，须 > 0）
uniform int uBlendPath;          // 0 = α 覆盖 / 1 = 加法近似（亮度当覆盖）/ 2 = 真加法累积
layout(location = 0) out vec4 FragAccum;   // 覆盖集：Σ(色·α·w, α·w)
layout(location = 1) out float FragReveal; // 覆盖集：Π(1−α)
layout(location = 2) out vec4 FragAdd;     // 加法集：Σ(色·α)（黑底=0 → 天然透明）
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
    // 覆盖路径的 α（近似模式下用亮度冒充）；真加法则用「颜色贡献」判空。
    float a_cov = col.a;
    vec3 contrib = col.rgb * col.a;
    if (uBlendPath == 1) {
        // 旧近似：加性纹理常无 alpha 语义（黑底=透明）→ 用亮度当覆盖；否则黑会变黑板。
        a_cov = clamp(max(max(col.rgb.r, col.rgb.g), col.rgb.b), 0.0, 1.0);
    }
    if (uBlendPath == 2) {
        // 真加法：黑底 ⇒ 贡献 0 ⇒ 天然透明，不需要亮度近似。
        if (max(max(contrib.r, contrib.g), contrib.b) <= 1.0 / 255.0) {
            discard;
        }
    } else if (a_cov <= 1.0 / 255.0) {
        discard;
    }
    // 与不透明场景深度手动作深度测试（玻璃 FBO 无深度缓冲，故用不了 GL depth test）。
    vec2 uv = gl_FragCoord.xy / uViewport;
    float scene_z = ZView(texture(uSceneDepth, uv).r);
    float z = ZView(gl_FragCoord.z);
    float fade = 1.0;
    if (uSoftDepthEnabled == 1) {
        // 软深度：0 < (scene_z − z) < 过渡距离 内线性淡出，相交处不再是硬切边。
        fade = clamp((scene_z - z) / uSoftDepthM, 0.0, 1.0);
    } else if (z > scene_z + 0.02) {
        discard;
    }
    if (uBlendPath == 2) {
        contrib *= fade;
        if (max(max(contrib.r, contrib.g), contrib.b) <= 1.0 / 255.0) {
            discard;
        }
        // 只写加法集；覆盖集写恒等值（accum 0 + reveal 0 ⇒ dst 不变）。
        FragAdd = vec4(contrib, 0.0);
        FragAccum = vec4(0.0);
        FragReveal = 0.0;
    } else {
        float a = a_cov * fade;
        if (a <= 1.0 / 255.0) {
            discard;
        }
        // 衰减必须发生在 WeightWbo 之前 → WBO 解析拿到的覆盖率才是衰减后的。
        float w = WeightWbo(z, a, col.rgb);
        FragAccum = vec4(col.rgb * a, a) * w;
        FragReveal = a;
        FragAdd = vec4(0.0);
    }
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
    unsigned int wbo_fbo_ = 0, accum_tex_ = 0, reveal_tex_ = 0, add_tex_ = 0;
    int wbo_w_ = 0, wbo_h_ = 0;
    bool has_content_ = false;
    bool has_alpha_ = false;      // 本帧画过覆盖集（走 over 合成）
    bool has_additive_ = false;   // 本帧画过加法集（走求和合成）

    InstanceBuffer pos_buf_{kParticlePosSpec};
    InstanceBuffer misc_buf_{kParticleMiscSpec};
    InstanceBuffer color_buf_{kParticleColorSpec};
    InstanceBuffer vel_buf_{kParticleVelSpec};
    bool initialized_ = false;
};

}  // namespace jpov

#endif  // JPOV_SRC_PARTICLES_PARTICLE_RENDERER_H_
