// JPOV VolumeFireRenderer —— 逐像素 raymarch 体积火 → 写进 WBO 透明纹理（与粒子/玻璃同套代数）
//
// 定位（tools/jpov/docs/jpov_volumetric_fire_design.md）：
//   把一团 `.jpovvol`（R=density / G=temperature[K]）沿视线做**体渲染积分**，输出
//   `(预乘发射 L, 透射率 1−α, 期望深度 z_repr)`，作为**一个薄层**写进本渲染器自持的
//   WBO 缓冲（accum/reveal/depth），再 Composite 解析 over 到当前目标（HDR）。
//   与 ParticleRenderer / #176 透明玻璃共用同一套 McGuire WBO 代数。
//
// 画法：画该团的世界 AABB **盒**（8 角由 gl_VertexID 生成；**剔除正面** → 每像素恰好
//   光栅化一个盒面 → FS 只跑一次）。FS：从像素反构世界光线 → 与盒求交 [t_near,t_far] →
//   沿盒内步进采 3D 体纹理（trilinear + 帧间 lerp）→ emission-absorption 累积。
//
// 质量/性能旋钮：steps（采样数）、jitter（条带→蓝噪声）、early-out（T<ε 停）。

#ifndef JPOV_SRC_VOLUME_FIRE_VOLUME_FIRE_RENDERER_H_
#define JPOV_SRC_VOLUME_FIRE_VOLUME_FIRE_RENDERER_H_

#include <cstdint>
#include <string>
#include <unordered_map>

#include "tools/jpov/interface/camera.h"
#include "tools/jpov/interface/volume_fire_types.h"
#include "tools/jpov/src/shader_manager.h"
#include "tools/jpov/src/volume_manager.h"

namespace jpov {

class VolumeFireRenderer {
public:
    VolumeFireRenderer();
    ~VolumeFireRenderer();

    VolumeFireRenderer(const VolumeFireRenderer&) = delete;
    VolumeFireRenderer& operator=(const VolumeFireRenderer&) = delete;

    // 顶点着色器：由 gl_VertexID 生成 AABB 盒的 36 个顶点（12 三角形）。
    // 角点索引用位编码：bit0→x、bit1→y、bit2→z（min/max）。
    static constexpr const char* kVs = R"glsl(
#version 330 core
uniform mat4 uViewProj;
uniform vec3 uBoxMin;
uniform vec3 uBoxMax;
uniform int uFullscreen;  // 1 = 全屏 quad（调试，替代盒光栅化）
out vec3 vWorld;
const int kIdx[36] = int[36](
   4,6,2, 4,2,0,   1,3,7, 1,7,5,
   0,1,5, 0,5,4,   6,7,3, 6,3,2,
   2,3,1, 2,1,0,   4,5,7, 4,7,6);
void main() {
    if (uFullscreen == 1) {
        vec2 c = vec2((gl_VertexID == 1 || gl_VertexID == 3) ? 1.0 : -1.0,
                      (gl_VertexID >= 2) ? 1.0 : -1.0);
        gl_Position = vec4(c, 0.0, 1.0);
        vWorld = vec3(0.0);
        return;
    }
    int i = kIdx[gl_VertexID];
    vec3 p = vec3((i & 1) == 1 ? uBoxMax.x : uBoxMin.x,
                  (i & 2) == 2 ? uBoxMax.y : uBoxMin.y,
                  (i & 4) == 4 ? uBoxMax.z : uBoxMin.z);
    vWorld = p;
    gl_Position = uViewProj * vec4(p, 1.0);
}
)glsl";

    // 片元着色器：raymarch。输出 WBO 三附件（accum/reveal/depth）。
    static constexpr const char* kFs = R"glsl(
#version 330 core
in vec3 vWorld;
uniform vec3 uCamPos;
uniform vec3 uCamForward;
uniform mat4 uInvViewProj;   // 逆视图投影（从像素反构光线，避免面片插值误差）
uniform vec3 uBoxMin;
uniform vec3 uBoxMax;
uniform sampler2DArray uVol;   // 层 = 帧；层内 (u=x, v=(z + y)/nz)
uniform vec3 uVolDim;          // (nx, ny, nz)
uniform int  uNFrames;
uniform float uSigma;          // 消光尺度（1/m）
uniform float uEmission;       // 发射尺度
uniform float uTempScale;      // G(∈[0,1]) → 开尔文
uniform vec3  uSmokeAlbedo;    // 烟的反照率
uniform float uSmokeScatter;   // 烟散射强度（0 = 关，烟不可见）
uniform vec3  uLightColor;     // 烟的受光辐射色（含强度）
uniform int   uChannels;       // 体纹理通道数（2 = 无烟；3 = 带 B=烟密度）
uniform float uFrame;          // 播放帧号（含小数）
uniform int  uSteps;
uniform int  uJitter;
uniform sampler2D uSceneDepth;
uniform vec2 uViewport;
uniform float uSeed;           // 每像素抖动种子基
uniform float uNear;
uniform float uFar;
layout(location = 0) out vec4 FragAccum;
layout(location = 1) out float FragReveal;
layout(location = 2) out float FragDepth;

// 黑体色（Tanner Helland 近似；与 SkyCommand::ColorTempToLinear 同一条曲线）。
vec3 Blackbody(float kelvin) {
    float t = clamp(kelvin, 1000.0, 40000.0) / 100.0;
    float r, g, b;
    if (t <= 66.0) { r = 255.0; }
    else { r = clamp(329.698727446 * pow(t - 60.0, -0.1332047592), 0.0, 255.0); }
    if (t <= 66.0) { g = clamp(99.4708025861 * log(t) - 161.1195681661, 0.0, 255.0); }
    else { g = clamp(288.1221695283 * pow(t - 60.0, -0.0755148492), 0.0, 255.0); }
    if (t >= 66.0) { b = 255.0; }
    else if (t <= 19.0) { b = 0.0; }
    else { b = clamp(138.5177312231 * log(t - 10.0) - 305.0447927307, 0.0, 255.0); }
    return vec3(r, g, b) / 255.0;
}

float Hash12(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

// 「火」与「烟」由**独立通道**区分：R=火密度（自发光）、B=烟密度（只吸收 + 受光散射）。
// 2 通道文件（无 B）⇒ 视为无烟。

// 世界点 → 体局部 [0,1]^3。
vec3 ToLocal(vec3 p) {
    return (p - uBoxMin) / max(uBoxMax - uBoxMin, vec3(1e-6));
}

// 体局部采样（trilinear(z) + 帧间 lerp）。p ∈ [0,1]^3。
vec4 SampleVol(vec3 p) {
    float nz = uVolDim.z;
    float izf = clamp(p.z, 0.0, 1.0) * max(nz - 1.0, 1.0);
    float iz0 = floor(izf);
    float iz1 = min(iz0 + 1.0, nz - 1.0);
    float fz = izf - iz0;
    float u = clamp(p.x, 0.0, 1.0);
    float y = clamp(p.y, 0.0, 1.0);
    float f0 = floor(uFrame);
    float f1 = mod(f0 + 1.0, float(uNFrames));
    float ff = uFrame - f0;
    vec2 uv0 = vec2(u, (iz0 + y) / nz);
    vec2 uv1 = vec2(u, (iz1 + y) / nz);
    vec4 zz0 = mix(texture(uVol, vec3(uv0, f0)), texture(uVol, vec3(uv1, f0)), fz);
    vec4 zz1 = mix(texture(uVol, vec3(uv0, f1)), texture(uVol, vec3(uv1, f1)), fz);
    return mix(zz0, zz1, ff);
}

// 窗口深度 [0,1] → 视空间深度（正，米）。
float ZView(float d) {
    float ndc = 2.0 * d - 1.0;
    return (2.0 * uNear * uFar) / ((uFar + uNear) - ndc * (uFar - uNear));
}

void main() {
    // 从像素（gl_FragCoord + 逆 VP）反构**唯一的**世界光线 —— 与光栅化了哪个 box 面无关，
    // 消除「不同面片插值出不同射线」在 box 面投影边界处的接缝。
    vec2 ndc = gl_FragCoord.xy / uViewport * 2.0 - 1.0;
    vec4 fw = uInvViewProj * vec4(ndc, 1.0, 1.0);
    vec3 rd = normalize(fw.xyz / fw.w - uCamPos);
    vec3 o = uCamPos;
    // 与盒 AABB 求交（slab 法）。
    vec3 inv = 1.0 / rd;
    vec3 t0 = (uBoxMin - o) * inv;
    vec3 t1 = (uBoxMax - o) * inv;
    vec3 tsm = min(t0, t1);
    vec3 tbg = max(t0, t1);
    float tn = max(max(tsm.x, tsm.y), tsm.z);
    float tf = min(min(tbg.x, tbg.y), tbg.z);
    tn = max(tn, 0.0);
    if (tf <= tn) { discard; }

    // 被不透明几何遮挡：ray 截到场景深度（视空间）。
    float scene_z = ZView(texture(uSceneDepth, gl_FragCoord.xy / uViewport).r);
    float ray_to_fwd = max(dot(rd, uCamForward), 1e-4);
    float tf_scene = scene_z / ray_to_fwd;   // 场景深度对应的 t
    tf = min(tf, tf_scene);
    if (tf <= tn) { discard; }

    int steps = max(uSteps, 1);
    float dt = (tf - tn) / float(steps);
    float jit = (uJitter == 1) ? Hash12(gl_FragCoord.xy + vec2(uSeed, uSeed * 1.7)) : 0.5;

    float T = 1.0;
    vec3 L = vec3(0.0);
    float num = 0.0;   // ∫ z·σ·T dt
    float den = 0.0;   // ∫ σ·T dt
    for (int k = 0; k < steps; ++k) {
        float t = tn + (float(k) + jit) * dt;
        vec3 p = o + t * rd;
        vec4 v = SampleVol(ToLocal(p));
        float fire_dens = v.r;
        float smoke_dens = (uChannels >= 3) ? v.b : 0.0;
        float tempK = v.g * uTempScale;
        float sigma = (fire_dens + smoke_dens) * uSigma;
        float a = 1.0 - exp(-sigma * dt);
        vec3 emis = Blackbody(tempK) * uEmission;
        float z_view = t * ray_to_fwd;
        float wgt = sigma * T * dt;
        L += T * a * emis;
        // 烟受均匀照明单次散射——没有这一项，烟在纯黑底上不可见。
        if (uSmokeScatter > 0.0 && smoke_dens > 0.0) {
            L += T * uSmokeScatter * (smoke_dens * uSigma) * uSmokeAlbedo * uLightColor * dt;
        }
        num += z_view * wgt;
        den += wgt;
        T *= (1.0 - a);
        if (T < 1e-3) { break; }
    }
    float alpha = 1.0 - T;
    if (alpha <= 1.0 / 255.0) { discard; }
    float z_repr = (den > 1e-9) ? (num / den) : (tn * ray_to_fwd);
    // WBO 覆盖：accum=色·α、A=α（单层权重抵消）；reveal=α；depth=z·α。
    FragAccum = vec4(L, alpha);
    FragReveal = alpha;
    FragDepth = z_repr * alpha;
}
)glsl";

    // 合成：全屏把 WBO 解析并以预乘 over 写进当前目标（HDR）。
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
    if (accum.a <= 1e-7) { FragColor = vec4(0.0); return; }
    vec3 color = accum.rgb / accum.a;
    float cov = clamp(1.0 - reveal, 0.0, 1.0);
    FragColor = vec4(color * cov, cov);
}
)glsl";

    void Init(ShaderManager* shader_mgr);
    void Finalize();

    // 帧首：确保 TAB 存在并 clear（accum=0/reveal=1/depth=0）。
    void Begin(int w, int h);

    // 画一团体积火进 TAB（累加）。体积纹理由 VolumeManager 持有（按 cmd.volume_id 查）。
    // Pre-condition: Init 已调用。
    void Draw(const VolumeFireCommand& cmd, VolumeManager& volume_mgr, const Camera& camera,
              const float view_proj[16],
              unsigned int scene_depth_tex, int w, int h);

    // 把本帧 WBO 解析并 over 合成进当前绑定的目标（HDR）。无内容则 no-op。
    void Composite(int w, int h);

    // 调试：把 raymarch 的中间结果（WBO accum=加权色 L、alpha=覆盖 1−reveal）导出为 PNG
    // （每帧一对 `<dir>/dump_NNN_accum.png` / `_alpha.png`）。空串=关。
    void SetDebugDump(const std::string& dir) { debug_dump_dir_ = dir; }

    bool has_content() const { return has_content_; }

private:
    void EnsureWbo(int w, int h);
    void DestroyWbo();
    void DumpWbo(int w, int h);

    ShaderManager* shader_mgr_ = nullptr;
    unsigned int prog_draw_ = 0;
    unsigned int prog_comp_ = 0;
    unsigned int vao_ = 0;
    unsigned int wbo_fbo_ = 0;
    unsigned int accum_tex_ = 0, reveal_tex_ = 0, depth_tex_ = 0;
    int wbo_w_ = 0, wbo_h_ = 0;
    bool has_content_ = false;
    bool initialized_ = false;
    std::string debug_dump_dir_;
    int dump_seq_ = 0;
};

}  // namespace jpov

#endif  // JPOV_SRC_VOLUME_FIRE_VOLUME_FIRE_RENDERER_H_
