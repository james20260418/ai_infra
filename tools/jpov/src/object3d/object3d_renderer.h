// JPOV Object3DRenderer — PBR 静态物体渲染 + Tile Forward 光照
//
// 管理 DrawObject3D 的 PBR shader、材质纹理绑定、点光源数据上传、
// 以及 CPU 端 tile culling。作为 Renderer 的内部组件，生命周期与 Renderer
// 相同。
//
// 与 SkeletonRenderer 的分工（两者互不依赖）：
//   本模块  —— 静态 Object3D 的 PBR + 点光源 tile culling + 拾取/高亮。
//   SkeletonRenderer（src/skeleton/）—— 蒙皮带骨实例的 PBR（太阳直射 + 环境光，
//   无点光源）+ 蒙皮阴影。蒙皮带骨渲染**不在本模块**，本模块不引用任何骨架类型。
//
// MeshManager 和 TextureManager 由 Renderer 共享传入（不持有所有权）。
//
// 用例：
//   Object3DRenderer o3d;
//   o3d.EnsureTileLighting(fbo_w, fbo_h, &tile_index_tex, ...);
//   o3d.UploadLightData(cmds, shader_mgr);
//   o3d.BuildTileLightIndices(cmds, fbo_w, fbo_h, tile_index_tex, ..., mvp);
//   o3d.DrawObject3D(cmd, cmds, mesh_mgr, texture_mgr, shader_mgr, mvp,
//                    tile_index_tex);

#ifndef JPOV_OBJECT3D_RENDERER_H_
#define JPOV_OBJECT3D_RENDERER_H_

#include <vector>

#include "tools/jpov/interface/render_command.h"
#include "tools/jpov/interface/camera.h"
#include "tools/jpov/src/mesh_manager.h"
#include "tools/jpov/src/shader_manager.h"
#include "tools/jpov/src/texture_manager.h"

namespace jpov {

class Object3DRenderer {
public:
    Object3DRenderer() = default;
    ~Object3DRenderer() = default;

    Object3DRenderer(const Object3DRenderer&) = delete;
    Object3DRenderer& operator=(const Object3DRenderer&) = delete;

    // ---- PBR 渲染所需的 GLSL shader 源码 ----
    //
    // 调用方用 ShaderManager 编译：
    //   shader_mgr.GetOrCreate("draw_object3d_pbr",
    //       {kMeshVs3dPBR, kMeshFs3dPBR})
    //   shader_mgr.GetOrCreate("draw_object3d_pbr_full",
    //       {kMeshVs3dPBRFull, kMeshFs3dPBR})
    //
    // kMeshVs3dPBR: 无 UV 版 vertex shader（mesh 不含 kUV 时使用）
    //   输入: vec3 aPos(loc=0), vec3 aNormal(loc=1)
    //   uniform: mat4 uMVP, mat4 uModel
    //   输出: vWorldPos, vWorldNormal, vTexCoord(0,0), vWorldTangent(0,0,0)
    static constexpr const char* kMeshVs3dPBR = R"glsl(
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
uniform mat4 uMVP;
uniform mat4 uModel;
out vec3 vWorldPos;
out vec3 vWorldNormal;
out vec2 vTexCoord;
out vec3 vWorldTangent;
void main() {
    vec4 world_pos = uModel * vec4(aPos, 1.0);
    vWorldPos = world_pos.xyz;
    vWorldNormal = normalize(mat3(transpose(inverse(uModel))) * aNormal);
    vTexCoord = vec2(0.0);
    vWorldTangent = vec3(0.0);
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)glsl";

    // kMeshVs3dPBRFull: 完整版 vertex shader（含 UV + tangent）
    //   输入: vec3 aPos(loc=0), vec3 aNormal(loc=1),
    //         vec2 aTexCoord(loc=2), vec3 aTangent(loc=5)
    static constexpr const char* kMeshVs3dPBRFull = R"glsl(
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aTexCoord;
layout(location = 5) in vec3 aTangent;
uniform mat4 uMVP;
uniform mat4 uModel;
out vec3 vWorldPos;
out vec3 vWorldNormal;
out vec2 vTexCoord;
out vec3 vWorldTangent;
void main() {
    vec4 world_pos = uModel * vec4(aPos, 1.0);
    vWorldPos = world_pos.xyz;
    vWorldNormal = normalize(mat3(transpose(inverse(uModel))) * aNormal);
    vWorldTangent = normalize(mat3(transpose(inverse(uModel))) * aTangent);
    vTexCoord = aTexCoord;
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)glsl";

    // kShadowVs: 阴影 pass 专用 vertex shader。
    //   输入: vec3 aPos(loc=0)。
    //   uShadowMVP      = proj*view*model（光空间裁剪，用于 gl_Position 近远裁剪）
    //   uShadowDepthMVP = DepthProj*view*model。vShadowDepth 输出其 z 分量：
    //                     即"相对主视锥中心的原始线性深度"（DepthProj 保证 w=1，
    //                     经 .z 直接得线深，不经 near/far 归一化，depth 0 点在主视锥中心）。
    static constexpr const char* kShadowVs = R"glsl(
#version 330 core
layout(location = 0) in vec3 aPos;
uniform mat4 uShadowMVP;
uniform mat4 uShadowDepthMVP;
out float vShadowDepth;
void main() {
    vec4 clip = uShadowMVP * vec4(aPos, 1.0);
    gl_Position = clip;
    vec4 dpos = uShadowDepthMVP * vec4(aPos, 1.0);
    vShadowDepth = dpos.z / dpos.w;   // 线性深度（DepthProj 下 w=1）
}
)glsl";

    // kShadowFs: 阴影 pass fragment shader。把线性深度写入颜色通道 .r。
    // 用 RGBA32F 颜色纹理存深度（而非 GL 深度缓冲），避开 headless/软渲染下
    // depth 纹理采样精度/格式不一致的问题（见 renderer.cc EnsureShadowFBO）。
    static constexpr const char* kShadowFs = R"glsl(
#version 330 core
in float vShadowDepth;
out vec4 FragColor;
void main() {
    FragColor = vec4(vShadowDepth, 0.0, 0.0, 1.0);
}
)glsl";

    // kMeshFs3dPBR: 统一 GGX PBR fragment shader（Tiled Forward）
    //   两版 vertex shader 共用此 frag。
    //   内置 tile culling（readTileLights → 只算本 tile 命中的光源）。
    //   uHas*Tex 标志控制各通道走纹理采样还是常值 fallback。
    //   需要绑定 uTileLightIndices sampler（TEXTURE0）。
    //
    //   下面的 #define 常量与 Object3DRenderer 的 kTileSize16_/
    //   kMaxLightsPerTile_/kMaxTotalLights_/kLightIndexSentinel_ 一一对应：
    //   改任一处必须同时改另一处，否则 tile 编码/解码错位。
    static constexpr const char* kMeshFs3dPBR = R"glsl(
#version 330 core

#define TILE_SIZE 16
#define MAX_LIGHTS_PER_TILE 16
#define MAX_TOTAL_LIGHTS 255
#define LIGHT_INDEX_SENTINEL 255u

in vec3 vWorldPos;
in vec3 vWorldNormal;
in vec2 vTexCoord;
in vec3 vWorldTangent;
out vec4 FragColor;

struct Light {
    vec3 position;
    vec3 color;
    float radius;           // 衰减半径
    float physicalRadius;   // 光源球体物理半径（0 = 点光源）
    float intensity;        // 亮度标量（乘 color，见 LIGHT_INTENSITY.md）
};

uniform Light uLights[MAX_TOTAL_LIGHTS];
uniform int uTotalLights;
uniform vec3 uCameraPos;
uniform float uCameraNear;   // 相机近平面距离（级联 0 的 near，级联过渡权重用）

uniform vec3  uBaseColor;
uniform sampler2D uBaseColorTex;
uniform int   uHasBaseColorTex;
uniform float uMetallic;
uniform sampler2D uMetallicTex;
uniform int   uHasMetallicTex;
uniform float uRoughness;
uniform sampler2D uRoughnessTex;
uniform int   uHasRoughnessTex;
uniform vec3  uEmissive;
uniform sampler2D uEmissiveTex;
uniform int   uHasEmissiveTex;
uniform float uAO;
uniform sampler2D uAoTex;
uniform int   uHasAoTex;
uniform sampler2D uNormalTex;
uniform int   uHasNormalTex;
uniform float uNormalScale;

uniform sampler2D uTileLightIndices;
// uTileCulling=1（默认）：片元经 tile 索引纹理只算本 tile 命中的光源（16×16 tile culling）。
// uTileCulling=0：关闭 tile culling，片元遍历全部点光源（经典前向光照，无 tile 分界线伪影）。
uniform int uTileCulling;

// 太阳平行光（DirectionalLight）+ 级联阴影（CSM）。
// uHasSun=1 时施加直射 GGX 光照（diffuse+specular），并按片元距相机距离选级联，
// 采样对应 uShadowMap[c] 做 PCF 阴影；最后按距相机距离淡出影子强度。
uniform int   uHasSun;
uniform vec3  uSunDir;       // 光传播方向（归一化），从光源指向场景
uniform vec3  uSunColor;     // 光颜色
uniform float uSunIntensity;
uniform int   uCascadeCount;          // 级联段数
uniform float uCascadeRanges[5];      // 各级联 far 距离（严格递增；末项=总阴影距离）
uniform sampler2D uShadowMap[5];      // 各级联光空间深度贴图（TEXTURE7+i，.r = 线性深度，相对主视锥中心）
uniform mat4  uShadowVP[5];           // 各级联光空间 ViewProj（用于把 world_pos 投影到 shadow map uv)
uniform mat4  uShadowDepthVP[5];      // 各级联光空间线性深度矩阵（DepthProj*view，.z = 相对主视锥中心的线深）
uniform float uShadowTexel[5];        // 各级联 1.0/shadow map 尺寸（PCF 纹素步长）
uniform int   uShadowBiasOverride;      // 0=自动几何推导（默认）；1=用手工 uShadowBiasCascade
uniform float uShadowTexelWorld[5];     // 每级联单纹素世界边长（米），自动偏置用
uniform float uShadowBiasCascade[5];    // override 的等效单纹素世界边长（米），仅 override=1 时用

// ---- PCF 核 与 深度偏置 联动常量（改核半径，偏置自动跟随）----
const int   kPcfRadiusT    = 1;                                 // 核半径（纹素）：3×3 → 1
const int   kPcfTapCountT  = (2*kPcfRadiusT+1) * (2*kPcfRadiusT+1);
const float kBiasSafety    = 1.5;                               // 满径安全裕度
const float kBiasK         = kBiasSafety * float(kPcfRadiusT);  // 自动偏置系数
const float kMinShadowBias = 0.01;                              // 全局兜底（米）
// 级联权重早退阈值：权重 (wlo·whi) 不大于此值即跳过该级联的 PCF 采样。
// 与 computeSunShadow 末尾的 `wsum > 1e-5` 同量级——远小于该量的权重对
// 最终 shadow 的贡献低于浮点有效精度，跳过不改变可见结果。
const float kShadowWeightEps = 1e-5;
uniform float uShadowFadeStart;       // 影子淡出起点（距相机）
uniform float uShadowFadeEnd;         // 影子淡出终点（此距离后无影子）

// 全局环境光（AmbientLight）：无方向、无影子，照亮背阳面。
uniform vec3  uAmbientColor;      // 环境光色调（RGB）
uniform float uAmbientIntensity;  // 环境光亮度标量（乘 color）

const float PI = 3.14159265;

// 阴影因子：对世界坐标在指定级联里采样深度贴图，返回 [0,1]，1=完全受照，0=完全在影子里。
// shadow map 存的是**相对主视锥中心的原始线性深度**（米，见 kShadowVs）；
// 主 pass 用 uShadowDepthVP[c]（DepthProj*view）把 world_pos 重投到同一线性深度，
// 两端同源一致、不经 near/far 归一化。
// ⚠️ PCF：核 = (2·kPcfRadiusT+1)² 采样（kPcfRadiusT=1 → 3×3），平均 soft shadow。
// depth bias（自动推导，见 ShadowConfig::cascade_bias）：
//     bias_c = max(kMinShadowBias, kBiasK · texelW_c · tanθ)
//     texelW_c = uShadowTexelWorld[c]（该级联单纹素世界边长，米；自动）
//             或 uShadowBiasCascade[c]（override：手工等效边长）
//     tanθ = sqrt(1-(N·Ld)²)/max(N·Ld,1e-3)，Ld = 深度轴方向（从 uShadowDepthVP 的 z 行取，
//            即阴影 pass 实际用的光传播方向的反向 —— 与深度比较同源；不能用 uSunDir，
//            因为阴影 pass 会对近平行的光方向做偏置以避开 lookAt 退化）
//   推导：平坦接收面在一个纹素足迹内的光轴深度偏离 = texelWorld·tanθ
//         （足迹被拉长为 t/cosθ，深度梯度 sinθ，相乘 = t·tanθ）；
//         PCF 会采到核半径个纹素外，kBiasK 已含核半径与裕度。
//   ⚠️ 2026-09-17 教训：旧式 bias_base*(1-NdotL) 在垂直光（NdotL→1）下被乘成 0、
//   退化为 minBias=0.01，而远级联单纹素大（C2 13.2cm/C3 46.6cm/C4 72.6cm），
//   平坦地面深度误差 texelWorld·tanθ 超过 0.01 → 地面自阴影 acne
//   （表现为地平线下方一条随级联纹素呈大格子的暗带）。
// ⚠️ GLSL 330 桌面版禁止非编译期常量的 sampler 数组索引，故各级联必须拆成
// 独立函数（或 if/else 全展开），不能 shadowFactorCascade(c, ...) 里动态取
// uShadowMap[c]。这里按 kMaxCascades=5 手写全展开。
// 阴影因子（单级联 C0）：输出 {shadow, covered}。
// covered=1 表示世界坐标落在该级联 shadow map 的 uv 覆盖内（有效采样）；
// covered=0 表示不在（uv 越界）—— 此时不贡献 shadow，由 computeSunShadow
// 用其他覆盖该片元的级联做 blend，避免“shadow map 边缘被硬裁成无影”。
void shadowFactorC0(vec3 world_pos, vec3 N, vec3 L, out float shadow, out float covered) {
    vec4 lsp = uShadowVP[0] * vec4(world_pos, 1.0);
    vec3 ndc = lsp.xyz / lsp.w;
    vec2 uv = ndc.xy * 0.5 + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) { shadow = 0.0; covered = 0.0; return; }
    vec4 dpos = uShadowDepthVP[0] * vec4(world_pos, 1.0);
    float cur = dpos.z / dpos.w;
    // 深度偏置（推导见 ShadowConfig::cascade_bias）：自动=几何推导，override=手工等效边长。
    // 深度轴方向 = uShadowDepthVP 的 z 行（= 阴影 pass 实际用的光传播方向）。
    // 用它算 tanθ 而非 uSunDir：阴影 pass 在光方向近平行世界 up 时会偏置方向以避开
    // lookAt 退化，此时深度轴与 uSunDir 不同（平坦地面仍有真实深度梯度）。
    vec3 dfwd0 = vec3(uShadowDepthVP[0][0][2], uShadowDepthVP[0][1][2], uShadowDepthVP[0][2][2]);
    vec3 Ld0 = -normalize(dfwd0);
    float ndl0 = max(dot(N, Ld0), 1e-3);
    float tanTheta0 = sqrt(max(1.0 - ndl0*ndl0, 0.0)) / ndl0;
    float texelW0 = (uShadowBiasOverride != 0) ? uShadowBiasCascade[0]
                                                 : uShadowTexelWorld[0];
    cur -= max(kMinShadowBias, kBiasK * texelW0 * tanTheta0);
    float s = 0.0;   // 固定 3×3 PCF
    for (int dy = -kPcfRadiusT; dy <= kPcfRadiusT; dy++) for (int dx = -kPcfRadiusT; dx <= kPcfRadiusT; dx++)
        s += (cur <= texture(uShadowMap[0], uv + vec2(float(dx), float(dy)) * uShadowTexel[0]).r) ? 1.0 : 0.0;
    shadow = s / float(kPcfTapCountT);
    covered = 1.0;
}
void shadowFactorC1(vec3 world_pos, vec3 N, vec3 L, out float shadow, out float covered) {
    vec4 lsp = uShadowVP[1] * vec4(world_pos, 1.0);
    vec3 ndc = lsp.xyz / lsp.w;
    vec2 uv = ndc.xy * 0.5 + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) { shadow = 0.0; covered = 0.0; return; }
    vec4 dpos = uShadowDepthVP[1] * vec4(world_pos, 1.0);
    float cur = dpos.z / dpos.w;
    // 深度偏置（推导见 ShadowConfig::cascade_bias）：自动=几何推导，override=手工等效边长。
    // 深度轴方向 = uShadowDepthVP 的 z 行（= 阴影 pass 实际用的光传播方向）。
    // 用它算 tanθ 而非 uSunDir：阴影 pass 在光方向近平行世界 up 时会偏置方向以避开
    // lookAt 退化，此时深度轴与 uSunDir 不同（平坦地面仍有真实深度梯度）。
    vec3 dfwd1 = vec3(uShadowDepthVP[1][0][2], uShadowDepthVP[1][1][2], uShadowDepthVP[1][2][2]);
    vec3 Ld1 = -normalize(dfwd1);
    float ndl1 = max(dot(N, Ld1), 1e-3);
    float tanTheta1 = sqrt(max(1.0 - ndl1*ndl1, 0.0)) / ndl1;
    float texelW1 = (uShadowBiasOverride != 0) ? uShadowBiasCascade[1]
                                                 : uShadowTexelWorld[1];
    cur -= max(kMinShadowBias, kBiasK * texelW1 * tanTheta1);
    float s = 0.0;
    for (int dy = -kPcfRadiusT; dy <= kPcfRadiusT; dy++) for (int dx = -kPcfRadiusT; dx <= kPcfRadiusT; dx++)
        s += (cur <= texture(uShadowMap[1], uv + vec2(float(dx), float(dy)) * uShadowTexel[1]).r) ? 1.0 : 0.0;
    shadow = s / float(kPcfTapCountT);
    covered = 1.0;
}
void shadowFactorC2(vec3 world_pos, vec3 N, vec3 L, out float shadow, out float covered) {
    vec4 lsp = uShadowVP[2] * vec4(world_pos, 1.0);
    vec3 ndc = lsp.xyz / lsp.w;
    vec2 uv = ndc.xy * 0.5 + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) { shadow = 0.0; covered = 0.0; return; }
    vec4 dpos = uShadowDepthVP[2] * vec4(world_pos, 1.0);
    float cur = dpos.z / dpos.w;
    // 深度偏置（推导见 ShadowConfig::cascade_bias）：自动=几何推导，override=手工等效边长。
    // 深度轴方向 = uShadowDepthVP 的 z 行（= 阴影 pass 实际用的光传播方向）。
    // 用它算 tanθ 而非 uSunDir：阴影 pass 在光方向近平行世界 up 时会偏置方向以避开
    // lookAt 退化，此时深度轴与 uSunDir 不同（平坦地面仍有真实深度梯度）。
    vec3 dfwd2 = vec3(uShadowDepthVP[2][0][2], uShadowDepthVP[2][1][2], uShadowDepthVP[2][2][2]);
    vec3 Ld2 = -normalize(dfwd2);
    float ndl2 = max(dot(N, Ld2), 1e-3);
    float tanTheta2 = sqrt(max(1.0 - ndl2*ndl2, 0.0)) / ndl2;
    float texelW2 = (uShadowBiasOverride != 0) ? uShadowBiasCascade[2]
                                                 : uShadowTexelWorld[2];
    cur -= max(kMinShadowBias, kBiasK * texelW2 * tanTheta2);
    float s = 0.0;
    for (int dy = -kPcfRadiusT; dy <= kPcfRadiusT; dy++) for (int dx = -kPcfRadiusT; dx <= kPcfRadiusT; dx++)
        s += (cur <= texture(uShadowMap[2], uv + vec2(float(dx), float(dy)) * uShadowTexel[2]).r) ? 1.0 : 0.0;
    shadow = s / float(kPcfTapCountT);
    covered = 1.0;
}
void shadowFactorC3(vec3 world_pos, vec3 N, vec3 L, out float shadow, out float covered) {
    vec4 lsp = uShadowVP[3] * vec4(world_pos, 1.0);
    vec3 ndc = lsp.xyz / lsp.w;
    vec2 uv = ndc.xy * 0.5 + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) { shadow = 0.0; covered = 0.0; return; }
    vec4 dpos = uShadowDepthVP[3] * vec4(world_pos, 1.0);
    float cur = dpos.z / dpos.w;
    // 深度偏置（推导见 ShadowConfig::cascade_bias）：自动=几何推导，override=手工等效边长。
    // 深度轴方向 = uShadowDepthVP 的 z 行（= 阴影 pass 实际用的光传播方向）。
    // 用它算 tanθ 而非 uSunDir：阴影 pass 在光方向近平行世界 up 时会偏置方向以避开
    // lookAt 退化，此时深度轴与 uSunDir 不同（平坦地面仍有真实深度梯度）。
    vec3 dfwd3 = vec3(uShadowDepthVP[3][0][2], uShadowDepthVP[3][1][2], uShadowDepthVP[3][2][2]);
    vec3 Ld3 = -normalize(dfwd3);
    float ndl3 = max(dot(N, Ld3), 1e-3);
    float tanTheta3 = sqrt(max(1.0 - ndl3*ndl3, 0.0)) / ndl3;
    float texelW3 = (uShadowBiasOverride != 0) ? uShadowBiasCascade[3]
                                                 : uShadowTexelWorld[3];
    cur -= max(kMinShadowBias, kBiasK * texelW3 * tanTheta3);
    float s = 0.0;
    for (int dy = -kPcfRadiusT; dy <= kPcfRadiusT; dy++) for (int dx = -kPcfRadiusT; dx <= kPcfRadiusT; dx++)
        s += (cur <= texture(uShadowMap[3], uv + vec2(float(dx), float(dy)) * uShadowTexel[3]).r) ? 1.0 : 0.0;
    shadow = s / float(kPcfTapCountT);
    covered = 1.0;
}
void shadowFactorC4(vec3 world_pos, vec3 N, vec3 L, out float shadow, out float covered) {
    vec4 lsp = uShadowVP[4] * vec4(world_pos, 1.0);
    vec3 ndc = lsp.xyz / lsp.w;
    vec2 uv = ndc.xy * 0.5 + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) { shadow = 0.0; covered = 0.0; return; }
    vec4 dpos = uShadowDepthVP[4] * vec4(world_pos, 1.0);
    float cur = dpos.z / dpos.w;
    // 深度偏置（推导见 ShadowConfig::cascade_bias）：自动=几何推导，override=手工等效边长。
    // 深度轴方向 = uShadowDepthVP 的 z 行（= 阴影 pass 实际用的光传播方向）。
    // 用它算 tanθ 而非 uSunDir：阴影 pass 在光方向近平行世界 up 时会偏置方向以避开
    // lookAt 退化，此时深度轴与 uSunDir 不同（平坦地面仍有真实深度梯度）。
    vec3 dfwd4 = vec3(uShadowDepthVP[4][0][2], uShadowDepthVP[4][1][2], uShadowDepthVP[4][2][2]);
    vec3 Ld4 = -normalize(dfwd4);
    float ndl4 = max(dot(N, Ld4), 1e-3);
    float tanTheta4 = sqrt(max(1.0 - ndl4*ndl4, 0.0)) / ndl4;
    float texelW4 = (uShadowBiasOverride != 0) ? uShadowBiasCascade[4]
                                                 : uShadowTexelWorld[4];
    cur -= max(kMinShadowBias, kBiasK * texelW4 * tanTheta4);
    float s = 0.0;
    for (int dy = -kPcfRadiusT; dy <= kPcfRadiusT; dy++) for (int dx = -kPcfRadiusT; dx <= kPcfRadiusT; dx++)
        s += (cur <= texture(uShadowMap[4], uv + vec2(float(dx), float(dy)) * uShadowTexel[4]).r) ? 1.0 : 0.0;
    shadow = s / float(kPcfTapCountT);
    covered = 1.0;
}

// 按片元到相机的距离，对**所有级联**加权混合阴影（级联间交叉 fade）。
// 返回最终阴影因子 [0,1]（1=完全受照无影，0=全影）。
// 按片元到相机的距离，对“主分级联 + 相邻级联”加权混合阴影（级联间交叉 fade）。
// 返回最终阴影因子 [0,1]（1=完全受照无影，0=全影）。
//
// 级联间 blend：在级联边界附近，相邻级联的权重互补——前级联在远端平滑降、
// 后级联在近端平滑升（smoothstep 边界重叠区），两者权重和恒≈1，消除不同级联
// 分辨率造成的硬分界/亮度跳变。片元深度在其主级联区间内，故主级联权重不会归零，
// 边界处不会出现“影子消失的细缝”。uv 越界（covered=0）的级联不贡献，由覆盖
// 它的其他级联供影（避免 shadow map 边缘被硬裁成无影）。
float computeSunShadow(vec3 world_pos, vec3 N, vec3 L, float frag_dist) {
    if (uCascadeCount <= 0 || uHasSun == 0) return 1.0;
    if (frag_dist >= uShadowFadeEnd) return 1.0;   // 淡出结束无影子

    float cNear[5]; cNear[0] = uCameraNear;
    for (int i = 1; i < 5; ++i) cNear[i] = (i <= uCascadeCount) ? uCascadeRanges[i-1] : cNear[i-1];

    float s; float cv;
    float wsum = 0.0;
    float wshadow = 0.0;

    // 每个实际声明的级联：近端升 × 远端降的平滑权重（与相邻级联互补）。
    // blend 宽度 = 该级联跨度的 15%。级联0 近端 / 末级联远端无邻居 → 恒 1。
    //
    // ⚠️ 早退（重要）：权重 (wlo·whi) 只依赖 frag_dist，与 shadow map 采样无关。
    // 绝大多数片元只有 1~2 个级联权重非零（其余权重为 0，对 wsum/wshadow 贡献
    // 恒为 0）。因此在**调用 shadowFactorCn 之前**先判权重：为 0 则整段跳过，
    // 省下该级联的 3×3 PCF（9 次纹理采样）。结果与不跳过**完全等价**（跳过的项
    // 贡献 = 0 · s），仅去掉纯浪费的采样。
    // 实测（默认 5 级联配置）：片元平均只 1.24 个级联权重非零，跳过约 75% 采样。
    if (uCascadeCount >= 1) {
        float n = uCameraNear, f = uCascadeRanges[0]; float b = 0.15*(f-n);
        float wlo = (uCascadeCount>=2) ? smoothstep(n - b, n + b, frag_dist) : 1.0;
        float whi = (uCascadeCount>=2) ? (1.0 - smoothstep(f - b, f + b, frag_dist)) : 1.0;
        float w0 = wlo * whi;
        if (w0 > kShadowWeightEps) {
            shadowFactorC0(world_pos, N, L, s, cv);
            float cw = w0 * cv; wsum += cw; wshadow += cw * s;
        }
    }
    if (uCascadeCount >= 2) {
        float n = uCascadeRanges[0], f = uCascadeRanges[1]; float b = 0.15*(f-n);
        float wlo = smoothstep(n - b, n + b, frag_dist);
        float whi = (uCascadeCount>=3) ? (1.0 - smoothstep(f - b, f + b, frag_dist)) : 1.0;
        float w1 = wlo * whi;
        if (w1 > kShadowWeightEps) {
            shadowFactorC1(world_pos, N, L, s, cv);
            float cw = w1 * cv; wsum += cw; wshadow += cw * s;
        }
    }
    if (uCascadeCount >= 3) {
        float n = uCascadeRanges[1], f = uCascadeRanges[2]; float b = 0.15*(f-n);
        float wlo = smoothstep(n - b, n + b, frag_dist);
        float whi = (uCascadeCount>=4) ? (1.0 - smoothstep(f - b, f + b, frag_dist)) : 1.0;
        float w2 = wlo * whi;
        if (w2 > kShadowWeightEps) {
            shadowFactorC2(world_pos, N, L, s, cv);
            float cw = w2 * cv; wsum += cw; wshadow += cw * s;
        }
    }
    if (uCascadeCount >= 4) {
        float n = uCascadeRanges[2], f = uCascadeRanges[3]; float b = 0.15*(f-n);
        float wlo = smoothstep(n - b, n + b, frag_dist);
        float whi = (uCascadeCount>=5) ? (1.0 - smoothstep(f - b, f + b, frag_dist)) : 1.0;
        float w3 = wlo * whi;
        if (w3 > kShadowWeightEps) {
            shadowFactorC3(world_pos, N, L, s, cv);
            float cw = w3 * cv; wsum += cw; wshadow += cw * s;
        }
    }
    if (uCascadeCount >= 5) {
        float n = uCascadeRanges[3], f = uCascadeRanges[4]; float b = 0.15*(f-n);
        float wlo = smoothstep(n - b, n + b, frag_dist);
        float whi = 1.0;
        float w4 = wlo * whi;
        if (w4 > kShadowWeightEps) {
            shadowFactorC4(world_pos, N, L, s, cv);
            float cw = w4 * cv; wsum += cw; wshadow += cw * s;
        }
    }

    float shadow = (wsum > 1e-5) ? (wshadow / wsum) : 1.0;
    float fade = 1.0 - clamp((frag_dist - uShadowFadeStart) / max(uShadowFadeEnd - uShadowFadeStart, 1e-5), 0.0, 1.0);
    return mix(1.0, shadow, fade);
}

vec3 fresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

float distributionGGX(vec3 N, vec3 H, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;
    float denom = NdotH2 * (a2 - 1.0) + 1.0;
    denom = PI * denom * denom;
    return a2 / max(denom, 1e-7);
}

float geometrySchlickGGX(float NdotX, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdotX / (NdotX * (1.0 - k) + k);
}

float geometrySmith(vec3 N, vec3 V, vec3 L, float roughness) {
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    return geometrySchlickGGX(NdotV, roughness) * geometrySchlickGGX(NdotL, roughness);
}

int readTileLights(int tile_col, int tile_row, out uint out_indices[MAX_LIGHTS_PER_TILE]) {
    int count = 0;
    for (int t = 0; t < MAX_LIGHTS_PER_TILE / 4; t++) {
        vec4 px = texelFetch(uTileLightIndices, ivec2(tile_col * (MAX_LIGHTS_PER_TILE / 4) + t, tile_row), 0);
        uint r = uint(px.r * 255.0 + 0.5) & 0xFFu;
        uint g = uint(px.g * 255.0 + 0.5) & 0xFFu;
        uint b = uint(px.b * 255.0 + 0.5) & 0xFFu;
        uint a = uint(px.a * 255.0 + 0.5) & 0xFFu;
        if (r != LIGHT_INDEX_SENTINEL && r < uint(uTotalLights) && count < MAX_LIGHTS_PER_TILE) {
            out_indices[count] = r; count++;
        }
        if (g != LIGHT_INDEX_SENTINEL && g < uint(uTotalLights) && count < MAX_LIGHTS_PER_TILE) {
            out_indices[count] = g; count++;
        }
        if (b != LIGHT_INDEX_SENTINEL && b < uint(uTotalLights) && count < MAX_LIGHTS_PER_TILE) {
            out_indices[count] = b; count++;
        }
        if (a != LIGHT_INDEX_SENTINEL && a < uint(uTotalLights) && count < MAX_LIGHTS_PER_TILE) {
            out_indices[count] = a; count++;
        }
    }
    return count;
}

// 累积单盏点光源的 diffuse + specular（含 Representative Point 球面光源）。
// li: 光源在 uLights[] 中的索引。共用于 tile culling 路径与非 cull 路径。
// 需传入 base_color / metallic / roughness（片元已解析的材质参数）；
// 通过 inout total_diffuse / total_specular 累加。
void accumulateOneLight(int li, vec3 N, vec3 V, vec3 world_pos,
                         vec3 base_color, float metallic, float roughness,
                         inout vec3 total_diffuse, inout vec3 total_specular) {
    vec3 L = uLights[li].position - world_pos;
    float dist = length(L);
    if (dist >= uLights[li].radius) return;

    vec3 light_dir = L / dist;
    float attenuation = 1.0 - (dist / uLights[li].radius);

    // ── Representative Point (Karis 2013) ──
    // 把点光源当作球面光源，用反射方向上离球最近的点作为 specular
    // 的有效入射方向。物理半径越大 → 球面积越大 → 更多 micro-facet
    // 能从不同区域命中镜面 lobe → 金属面不会全黑。
    // physicalRadius = 0 时退化为原始点光源行为。
    float sourceRadius = uLights[li].physicalRadius;
    vec3 spec_dir = light_dir;  // default: same as point light
    if (sourceRadius > 0.0) {
        vec3 Rf = reflect(-V, N);
        vec3 centerToRay = dot(L, Rf) * Rf - L;
        vec3 closestPt = L + centerToRay *
            clamp(sourceRadius / max(length(centerToRay), 1e-4), 0.0, 1.0);
        spec_dir = normalize(closestPt);
    }

    // ── diffuse：原始 light_dir ──
    float NdotL = max(dot(N, light_dir), 0.0);
    if (NdotL > 0.0) {
        vec3 Hd = normalize(light_dir + V);
        vec3 Fd = fresnelSchlick(max(dot(Hd, V), 0.0),
                     mix(vec3(0.04), base_color, metallic));
        vec3 kD = (vec3(1.0) - Fd) * (1.0 - metallic);
        vec3 diffuse = kD * base_color / PI;
        total_diffuse += uLights[li].color * uLights[li].intensity * diffuse * NdotL * attenuation;
    }

    // ── specular：representative point 方向 ──
    float NdotL_s = max(dot(N, spec_dir), 0.0);
    if (NdotL_s > 0.0) {
        vec3 H = normalize(spec_dir + V);
        float NdotV = max(dot(N, V), 0.0);
        vec3 F0 = mix(vec3(0.04), base_color, metallic);
        vec3 F = fresnelSchlick(max(dot(H, V), 0.0), F0);
        float D = distributionGGX(N, H, roughness);
        float G = geometrySmith(N, V, spec_dir, roughness);
        vec3 spec = (F * D * G) / max(4.0 * NdotL_s * NdotV, 1e-5);
        total_specular += uLights[li].color * uLights[li].intensity * spec * NdotL_s * attenuation;
    }
}

void main() {
    vec3 N = normalize(vWorldNormal);
    vec3 V = normalize(uCameraPos - vWorldPos);

    if (uHasNormalTex == 1) {
        vec3 tex_normal = texture(uNormalTex, vTexCoord).rgb * 2.0 - 1.0;
        tex_normal = normalize(tex_normal);
        tex_normal.xy *= uNormalScale;
        tex_normal = normalize(tex_normal);
        vec3 T = normalize(vWorldTangent - dot(vWorldTangent, N) * N);
        vec3 B = normalize(cross(N, T));
        mat3 TBN = mat3(T, B, N);
        N = normalize(TBN * tex_normal);
    }

    vec3 base_color = (uHasBaseColorTex == 1)
        ? texture(uBaseColorTex, vTexCoord).rgb
        : uBaseColor;

    float metallic = (uHasMetallicTex == 1)
        ? texture(uMetallicTex, vTexCoord).r
        : uMetallic;
    float roughness = (uHasRoughnessTex == 1)
        ? texture(uRoughnessTex, vTexCoord).r
        : uRoughness;
    vec3 emissive = (uHasEmissiveTex == 1)
        ? texture(uEmissiveTex, vTexCoord).rgb
        : uEmissive;
    float ao = (uHasAoTex == 1)
        ? texture(uAoTex, vTexCoord).r
        : uAO;
    ao = clamp(ao, 0.0, 1.0);

    // uTileCulling=0：跳过 tile 查询，直接遍历全部点光源（经典前向光照）。
    // uTileCulling=1：16×16 tile culling，只结算本 tile 命中的光源，
    // 高效但有 tile 边界分界线伪影（相邻 tile 光源取舍不同 → 亮暗跳变）。
    uint light_indices[MAX_LIGHTS_PER_TILE];
    int num_lights = 0;
    bool use_tile = (uTileCulling == 1);
    if (use_tile) {
        ivec2 grid = ivec2(textureSize(uTileLightIndices, 0));
        int grid_cols = grid.x / (MAX_LIGHTS_PER_TILE / 4);
        int grid_rows = grid.y;
        int tile_col = clamp(int(gl_FragCoord.x) / TILE_SIZE, 0, grid_cols - 1);
        int tile_row = clamp(int(gl_FragCoord.y) / TILE_SIZE, 0, grid_rows - 1);
        num_lights = readTileLights(tile_col, tile_row, light_indices);
    }

    vec3 ambient = uAmbientColor * uAmbientIntensity;
    vec3 total_diffuse = vec3(0.0);
    vec3 total_specular = vec3(0.0);

    // ── 太阳平行光（直射 GGX diffuse + specular，× 级联阴影因子）──
    // 平行光无衰减、方向全局，独立于 tile culling 的点光源循环。
    if (uHasSun == 1) {
        vec3 L = normalize(-uSunDir);       // 从片元指向光源
        float NdotL = max(dot(N, L), 0.0);
        if (NdotL > 0.0) {
            float frag_dist = length(uCameraPos - vWorldPos);
            float shadow = computeSunShadow(vWorldPos, N, L, frag_dist);
            vec3 light_col = uSunColor * uSunIntensity * shadow;

            // diffuse（Lambert + 菲涅尔去金属部分）
            vec3 Hd = normalize(L + V);
            vec3 Fd = fresnelSchlick(max(dot(Hd, V), 0.0),
                         mix(vec3(0.04), base_color, metallic));
            vec3 kD = (vec3(1.0) - Fd) * (1.0 - metallic);
            total_diffuse += light_col * (kD * base_color / PI) * NdotL;

            // specular（GGX Cook-Torrance）
            vec3 H = normalize(L + V);
            float NdotV = max(dot(N, V), 0.0);
            vec3 F0 = mix(vec3(0.04), base_color, metallic);
            vec3 F = fresnelSchlick(max(dot(H, V), 0.0), F0);
            float D = distributionGGX(N, H, roughness);
            float G = geometrySmith(N, V, L, roughness);
            vec3 spec = (F * D * G) / max(4.0 * NdotL * NdotV, 1e-5);
            total_specular += light_col * spec * NdotL;
        }
    }

    // 点光源：按 tile_culling 开关分流。
    //   uTileCulling=1：只结算本 tile 命中的光源（num_lights 已由 readTileLights 填充）。
    //   uTileCulling=0：遍历全部 uTotalLights（经典前向光照，无分界线）。
    if (use_tile) {
        for (int i = 0; i < num_lights; i++) {
            accumulateOneLight(int(light_indices[i]), N, V, vWorldPos,
                               base_color, metallic, roughness,
                               total_diffuse, total_specular);
        }
    } else {
        for (int i = 0; i < uTotalLights; i++) {
            accumulateOneLight(i, N, V, vWorldPos,
                               base_color, metallic, roughness,
                               total_diffuse, total_specular);
        }
    }

    vec3 result = ambient * base_color * ao / PI + total_diffuse + total_specular + emissive;
    FragColor = vec4(result, 1.0);
}
)glsl";

    // ---- Tile culling 常量 ----
    // 与上面 kMeshFs3dPBR 的 GLSL #define 一一对应，改任一处必须同时改另一处。
    static constexpr int kTileSize16 = 16;
    static constexpr int kMaxLightsPerTile = 16;
    static constexpr int kTexelsPerTile = kMaxLightsPerTile / 4;
    static constexpr int kMaxTotalLights = 255;
    static constexpr uint8_t kLightIndexSentinel = 255;

    // ---- EnsureTileLighting ----
    //
    // 分配/重建 tile index 纹理（GL_RGBA8），分辨率 = grid_cols×4 × grid_rows。
    // tile 网格与 3D FBO 同尺寸，随 fbo_w/fbo_h 变化时重建。
    //
    // 参数：
    //   fbo_w, fbo_h:   3D FBO 像素尺寸（≥ 1）。
    //   tile_index_tex: tile 纹理 GLuint，分配/重建到此（output，调用方持有所有权）。
    //   grid_w, grid_h: tile 网格列/行数（output）。
    //   tex_w, tex_h:   tile 纹理像素尺寸（output，grid_w×4 × grid_h）。
    //
    // Pre-condition: GL context 已激活
    static void EnsureTileLighting(int fbo_w, int fbo_h,
                                   unsigned int* tile_index_tex /*output*/,
                                   int* grid_w /*output*/, int* grid_h /*output*/,
                                   int* tex_w /*output*/, int* tex_h /*output*/);

    // ---- UploadLightData ----
    //
    // 将 cmds.point_lights（至多 kMaxTotalLights 个）上传到 PBR shader 的
    // uLights[] uniform 数组 + uTotalLights。两个 shader program 都上传
    //（DrawObject3DProg + DrawObject3DProgFull），因为 DrawObject3D 按材质动态切换。
    //
    // 参数：
    //   cmds:          RenderCommandList，取其 point_lights。
    //   shader_mgr:    ShaderManager 引用，用于 glUniform*（通过 GetUniform）。
    //   prog:          DrawObject3DProg() 返回值。
    //   prog_full:     DrawObject3DProgFull() 返回值。
    //
    // 每帧在 BuildTileLightIndices 之前调用一次（非逐 object 热路径）。
    static void UploadLightData(const RenderCommandList& cmds,
                                ShaderManager& shader_mgr,
                                unsigned int prog,
                                unsigned int prog_full);

    // ---- BuildTileLightIndices ----
    //
    // CPU 端 tile culling：遍历 point_lights，把每个光源投影到屏幕空间 → 覆盖
    // tile 范围 → 向每个覆盖 tile 写入光源 index（先到先得，每 tile ≤16 个）。
    // 覆盖是保守近似（6 轴向边界点投影取 min/max）；光源跨越相机时全屏覆盖。
    //
    // 参数：
    //   cmds:            RenderCommandList，取其 point_lights。
    //   fbo_w, fbo_h:    3D FBO 像素尺寸。
    //   tile_index_tex:  EnsureTileLighting 分配的 tile 纹理。
    //   grid_w, grid_h:  EnsureTileLighting 输出的网格尺寸。
    //   tex_w, tex_h:    EnsureTileLighting 输出的纹理像素尺寸。
    //   mvp:             MVP 矩阵（Proj×View），16 floats 列主序。
    //
    // Pre-condition: EnsureTileLighting 已调用，tile_index_tex ≠ 0
    // Pre-condition: mvp 是当前相机参数下的合法投影矩阵
    static void BuildTileLightIndices(const RenderCommandList& cmds,
                                      int fbo_w, int fbo_h,
                                      unsigned int tile_index_tex,
                                      int grid_w, int grid_h,
                                      int tex_w, int tex_h,
                                      const float mvp[16]);

    // ---- DrawObject3D ----
    //
    // 渲染单个 Object3DCommand：构建 Model 矩阵，选择 shader，绑定材质纹理，
    // 绑定 tile index 纹理，发起 draw call。
    //
    // 参数：
    //   cmd:            Object3DCommand（mesh_id + center/up/front + material）。
    //   cmds:           RenderCommandList（取 camera.position）。
    //   mesh_mgr:       MeshManager，取 GPU mesh（vao/vbo/ebo）。
    //   texture_mgr:    TextureManager，取材质纹理 GL 对象。
    //   shader_mgr:     ShaderManager，取 program（prog/prog_full）。
    //   mvp:            MVP 矩阵（Proj×View），16 floats 列主序。
    //   prog:           DrawObject3DProg()（无 UV 版本）。
    //   prog_full:      DrawObject3DProgFull()（完整版，含 UV+tangent）。
    //   tile_index_tex: tile culling 纹理（绑定到 TEXTURE0）。
    //
    // GL 状态前置要求（调用方负责）：
    //   - 3D MSAA FBO 已绑定，viewport 已设置
    //   - glEnable(GL_DEPTH_TEST) + GL_CULL_FACE 已设置
    //   - 若有光照：UploadLightData + BuildTileLightIndices 已调用
    //
    // 内部通过 glPushAttrib/glPopAttrib 恢复修改的 GL 状态。
    static void DrawObject3D(const Object3DCommand& cmd,
                             const RenderCommandList& cmds,
                             MeshManager& mesh_mgr,
                             TextureManager& texture_mgr,
                             ShaderManager& shader_mgr,
                             const float mvp[16],
                             unsigned int prog,
                             unsigned int prog_full,
                             unsigned int tile_index_tex);

    // ---- DrawObject3DShadow ----
    // 阴影 pass：用深度专用 shader（kShadowVs + kShadowFs）把一个 Object3D
    // 从太阳正交光空间视角画进阴影纹理（只写相对主视锥中心的线性深度到颜色 .r，不光照）。
    // shadow_vp: uShadowMVP = proj*view*model（裁剪）；
    // depth_vp:  uShadowDepthMVP = DepthProj*view*model（输出线性深度）。
    static void DrawObject3DShadow(const Object3DCommand& cmd,
                                   MeshManager& mesh_mgr,
                                   ShaderManager& shader_mgr,
                                   const float shadow_vp[16],
                                   const float depth_vp[16],
                                   unsigned int shadow_prog);

    // ---- UploadSunData ----
    // 把 cmds.sun（DirectionalLight）与级联阴影贴图参数上传到 PBR shader。
    // 无 sun 时仅把 uHasSun 置 0。两个 shader program 都上传。
    // shadow_fbos: 各级联 shadow FBO（取 .tex 绑到 TEXTURE7+i），
    //              长度须 == cfg.cascade_count。
    // shadow_vp:   各级联光空间 ViewProj，[kMaxCascades][16]。
    // shadow_depth_vp: 各级联光空间线性深度矩阵（DepthProj*view），[kMaxCascades][16]。
    // shadow_texel_world: 各级联单纹素世界边长（米），长度 kMaxCascades；自动深度偏置用
    //                     （见 ShadowConfig::cascade_bias）。
    static void UploadSunData(
        ShaderManager& shader_mgr,
        unsigned int prog,
        unsigned int prog_full,
        const std::vector<CascadeFBO>& shadow_fbos,
        const float shadow_vp[][16],
        const float shadow_depth_vp[][16],
        const float shadow_texel_world[],
        const ShadowConfig& cfg,
        const std::optional<DirectionalLight>& sun);

    // ---- UploadAmbient ----
    // 把入参 ambient（AmbientLight）上传到 PBR shader 的 uAmbientColor /
    // uAmbientIntensity。每帧在 Draw3DCommands 前调用一次（与 sun/点光源并列），
    // 两个 shader program 都上传。
    static void UploadAmbient(ShaderManager& shader_mgr,
                              unsigned int prog,
                              unsigned int prog_full,
                              const AmbientLight& ambient);
};

}  // namespace jpov

#endif  // JPOV_OBJECT3D_RENDERER_H_
