// JPOV SkeletonRenderer — 蒙皮带骨物体渲染 + Tile Forward 光照（Phase 1：整份 Object3DRenderer 复制，待做减法）
//
// 管理蒙皮带骨渲染的 shader、材质纹理绑定、骨骼 pose atlas 查表、逐实例 draw，
// 以及蒙皮阴影 pass 的光空间深度绘制。作为 Renderer 的内部组件，生命周期与 Renderer
// 相同。
//
// 与 Object3DRenderer 的分工（两者互不依赖）：
//   Object3DRenderer  —— 静态 Object3D 的 PBR + 点光源 tile culling + 拾取/高亮。
//   SkeletonRenderer  —— 蒙皮带骨实例的 PBR（太阳直射 + 环境光，无点光源）+ 蒙皮阴影。
// 蒙皮网格的 PBR 片元与 Object3D 同光照模型，但排版为独立的 kMeshFs3dPBR 常量
// （见头文件顶部「一片 shader 字符串不可单测，就不要分开管理」的约定）。
//
// MeshManager / TextureManager / ShaderManager 由 Renderer 共享传入（不持有所有权）。
//
// 用例：
//   SkeletonRenderer skel;
//   skel.UploadSunData(shader_mgr, prog, shadow_fbos, vp, dvp, cfg, sun);
//   skel.UploadAmbient(shader_mgr, prog, ambient);
//   skel.DrawSkinnedMesh(cmd, cmds, mesh_mgr, texture_mgr, shader_mgr, mvp, prog, gh);
//   skel.DrawSkinnedMeshShadow(cmd, mesh_mgr, shader_mgr, gh, shadow_vp, depth_vp, sp);

#ifndef JPOV_SKELETON_RENDERER_H_
#define JPOV_SKELETON_RENDERER_H_

#include <vector>

#include "tools/jpov/interface/render_command.h"
#include "tools/jpov/interface/camera.h"
#include "tools/jpov/src/mesh_manager.h"
#include "tools/jpov/src/shader_manager.h"
#include "tools/jpov/src/skeleton/skeleton_manager.h"
#include "tools/jpov/src/texture_manager.h"

namespace jpov {

class SkeletonRenderer {
public:
    SkeletonRenderer() = default;
    ~SkeletonRenderer() = default;

    SkeletonRenderer(const SkeletonRenderer&) = delete;
    SkeletonRenderer& operator=(const SkeletonRenderer&) = delete;

    // kShadowFs: 蒙皮阴影 pass 的 fragment shader。把蒙皮 VS 输出的线性深度写入颜色 .r。
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

    // kMeshFs3dPBR: 蒙皮带骨物体的 GGX PBR fragment shader（太阳直射 + 天光 + 自发光）。
    //   仅服务蒙皮 program（kSkinnedVs + 本 FS），故无点光源/tile culling 分支：
    //   光照 = 太阳平行光（含 CSM 阴影）+ 全局环境光，两者均为无衰减的全局量。
    //   uHas*Tex 标志控制各通道走纹理采样还是常值 fallback。
    static constexpr const char* kMeshFs3dPBR = R"glsl(
#version 330 core

in vec3 vWorldPos;
in vec3 vWorldNormal;
in vec2 vTexCoord;
in vec3 vWorldTangent;
out vec4 FragColor;

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
uniform float uShadowBiasCascade[5];    // 每级联 depth-bias 的 bias_base（米，外部可配；搭配 minBias=0.01）
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
// ⚠️ mile3：固定 3×3 PCF（平均 soft shadow）；depth bias = max(minBias, bias_base*(1-NdotL))：
//     minBias 全局 0.01（米，兜底垂直光 NdotL→1 使 slope 归零）；
//     bias_base = uShadowBiasCascade[c]（米，复用挡墙 cascade_bias 配置），
//     按“≥ 该级联单 texel 世界覆盖大小”原则设定，slope 项管中等倾角。
//     （PCF 系数=固定 3×3，对外部隐藏，不暴露新参数。）
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
    float slopeBias = uShadowBiasCascade[0] * (1.0 - dot(N, L));
    cur -= max(0.01, slopeBias);   // minBias 0.01 全局 + slope 项（bias_base=米）
    float s = 0.0;   // 固定 3×3 PCF
    for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++)
        s += (cur <= texture(uShadowMap[0], uv + vec2(float(dx), float(dy)) * uShadowTexel[0]).r) ? 1.0 : 0.0;
    shadow = s / 9.0;
    covered = 1.0;
}
void shadowFactorC1(vec3 world_pos, vec3 N, vec3 L, out float shadow, out float covered) {
    vec4 lsp = uShadowVP[1] * vec4(world_pos, 1.0);
    vec3 ndc = lsp.xyz / lsp.w;
    vec2 uv = ndc.xy * 0.5 + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) { shadow = 0.0; covered = 0.0; return; }
    vec4 dpos = uShadowDepthVP[1] * vec4(world_pos, 1.0);
    float cur = dpos.z / dpos.w;
    float slopeBias = uShadowBiasCascade[1] * (1.0 - dot(N, L));
    cur -= max(0.01, slopeBias);
    float s = 0.0;
    for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++)
        s += (cur <= texture(uShadowMap[1], uv + vec2(float(dx), float(dy)) * uShadowTexel[1]).r) ? 1.0 : 0.0;
    shadow = s / 9.0;
    covered = 1.0;
}
void shadowFactorC2(vec3 world_pos, vec3 N, vec3 L, out float shadow, out float covered) {
    vec4 lsp = uShadowVP[2] * vec4(world_pos, 1.0);
    vec3 ndc = lsp.xyz / lsp.w;
    vec2 uv = ndc.xy * 0.5 + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) { shadow = 0.0; covered = 0.0; return; }
    vec4 dpos = uShadowDepthVP[2] * vec4(world_pos, 1.0);
    float cur = dpos.z / dpos.w;
    float slopeBias = uShadowBiasCascade[2] * (1.0 - dot(N, L));
    cur -= max(0.01, slopeBias);
    float s = 0.0;
    for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++)
        s += (cur <= texture(uShadowMap[2], uv + vec2(float(dx), float(dy)) * uShadowTexel[2]).r) ? 1.0 : 0.0;
    shadow = s / 9.0;
    covered = 1.0;
}
void shadowFactorC3(vec3 world_pos, vec3 N, vec3 L, out float shadow, out float covered) {
    vec4 lsp = uShadowVP[3] * vec4(world_pos, 1.0);
    vec3 ndc = lsp.xyz / lsp.w;
    vec2 uv = ndc.xy * 0.5 + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) { shadow = 0.0; covered = 0.0; return; }
    vec4 dpos = uShadowDepthVP[3] * vec4(world_pos, 1.0);
    float cur = dpos.z / dpos.w;
    float slopeBias = uShadowBiasCascade[3] * (1.0 - dot(N, L));
    cur -= max(0.01, slopeBias);
    float s = 0.0;
    for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++)
        s += (cur <= texture(uShadowMap[3], uv + vec2(float(dx), float(dy)) * uShadowTexel[3]).r) ? 1.0 : 0.0;
    shadow = s / 9.0;
    covered = 1.0;
}
void shadowFactorC4(vec3 world_pos, vec3 N, vec3 L, out float shadow, out float covered) {
    vec4 lsp = uShadowVP[4] * vec4(world_pos, 1.0);
    vec3 ndc = lsp.xyz / lsp.w;
    vec2 uv = ndc.xy * 0.5 + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) { shadow = 0.0; covered = 0.0; return; }
    vec4 dpos = uShadowDepthVP[4] * vec4(world_pos, 1.0);
    float cur = dpos.z / dpos.w;
    float slopeBias = uShadowBiasCascade[4] * (1.0 - dot(N, L));
    cur -= max(0.01, slopeBias);
    float s = 0.0;
    for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++)
        s += (cur <= texture(uShadowMap[4], uv + vec2(float(dx), float(dy)) * uShadowTexel[4]).r) ? 1.0 : 0.0;
    shadow = s / 9.0;
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
    if (uCascadeCount >= 1) {
        float n = uCameraNear, f = uCascadeRanges[0]; float b = 0.15*(f-n);
        float wlo = (uCascadeCount>=2) ? smoothstep(n - b, n + b, frag_dist) : 1.0;
        float whi = (uCascadeCount>=2) ? (1.0 - smoothstep(f - b, f + b, frag_dist)) : 1.0;
        shadowFactorC0(world_pos, N, L, s, cv);
        float cw = (wlo * whi) * cv; wsum += cw; wshadow += cw * s;
    }
    if (uCascadeCount >= 2) {
        float n = uCascadeRanges[0], f = uCascadeRanges[1]; float b = 0.15*(f-n);
        float wlo = smoothstep(n - b, n + b, frag_dist);
        float whi = (uCascadeCount>=3) ? (1.0 - smoothstep(f - b, f + b, frag_dist)) : 1.0;
        shadowFactorC1(world_pos, N, L, s, cv);
        float cw = (wlo * whi) * cv; wsum += cw; wshadow += cw * s;
    }
    if (uCascadeCount >= 3) {
        float n = uCascadeRanges[1], f = uCascadeRanges[2]; float b = 0.15*(f-n);
        float wlo = smoothstep(n - b, n + b, frag_dist);
        float whi = (uCascadeCount>=4) ? (1.0 - smoothstep(f - b, f + b, frag_dist)) : 1.0;
        shadowFactorC2(world_pos, N, L, s, cv);
        float cw = (wlo * whi) * cv; wsum += cw; wshadow += cw * s;
    }
    if (uCascadeCount >= 4) {
        float n = uCascadeRanges[2], f = uCascadeRanges[3]; float b = 0.15*(f-n);
        float wlo = smoothstep(n - b, n + b, frag_dist);
        float whi = (uCascadeCount>=5) ? (1.0 - smoothstep(f - b, f + b, frag_dist)) : 1.0;
        shadowFactorC3(world_pos, N, L, s, cv);
        float cw = (wlo * whi) * cv; wsum += cw; wshadow += cw * s;
    }
    if (uCascadeCount >= 5) {
        float n = uCascadeRanges[3], f = uCascadeRanges[4]; float b = 0.15*(f-n);
        float wlo = smoothstep(n - b, n + b, frag_dist);
        float whi = 1.0;
        shadowFactorC4(world_pos, N, L, s, cv);
        float cw = (wlo * whi) * cv; wsum += cw; wshadow += cw * s;
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

    vec3 result = ambient * base_color * ao / PI + total_diffuse + total_specular + emissive;
    FragColor = vec4(result, 1.0);
}
)glsl";

    // ---- DrawSkinnedMesh ----
    // 渲染一批带骨实例（蒙皮主体渲染入口）：
    //   0) 用【蒙皮 program】(kSkinnedVs + kMeshFs3dPBR, 含骨槽) —— 无 prog/prog_full 选择；
    //   1) take SkeletonManager.gpu_handles() → 把【骨纹理 pose atlas】绑到空闲槽(TEXTURE12)
    //      + uBoneCount, 并设 per-instance 的 uPoseRow/uPoseCol（pose 从 cmd.instances[k] 取）;
    //   2) 绑材质纹理 + mesh VAO + 逐实例 draw（光照由调用方在 Render() 主流程经
    //      UploadSunData / UploadAmbient 预置好，此处不上传光照）。
    // 参数：
    //   skinned_prog: 蒙皮 program（调用方经 ShaderManager 建 {kSkinnedVs, kMeshFs3dPBR} 传入）。
    //   gh: SkeletonManager::GpuHandles（pose_atlas_tex/bone_count/pose_per_row）。
    //   cmd: SkinnedMeshCommand（mesh_id + material + instances[center/up/front/scale/pose_a]）。
    static void DrawSkinnedMesh(
        const SkinnedMeshCommand& cmd,
        const RenderCommandList& cmds,
        MeshManager& mesh_mgr,
        TextureManager& texture_mgr,
        ShaderManager& shader_mgr,
        const float mvp[16],
        unsigned int skinned_prog,
        const SkeletonManager::GpuHandles& gh);

    // ---- DrawSkinnedMeshShadow ----
    // 阴影 pass：把一批带骨实例从太阳正交光空间画进阴影纹理（只写相对主视锥中心的
    // 线性深度到颜色 .r，不光照）。蒙皮在 mesh 局部空间做，再乘光空间 VP*model。
    // shadow_vp:  uShadowMVP = proj*view*model（裁剪）；
    // depth_vp:   uShadowDepthMVP = DepthProj*view*model（输出线性深度）。
    static void DrawSkinnedMeshShadow(
        const SkinnedMeshCommand& cmd,
        MeshManager& mesh_mgr,
        ShaderManager& shader_mgr,
        const SkeletonManager::GpuHandles& gh,
        const float shadow_vp[16],
        const float depth_vp[16],
        unsigned int shadow_prog);

    // ---- UploadSunData ----
    // 把 cmds.sun（DirectionalLight）与级联阴影贴图参数上传到蒙皮 PBR shader。
    // 无 sun 时仅把 uHasSun 置 0（防止上一帧残留直射光）。
    // shadow_fbos: 各级联 shadow FBO（取 .tex 绑到 TEXTURE7+i），
    //              长度须 == cfg.cascade_count。
    // shadow_vp:   各级联光空间 ViewProj，[kMaxCascades][16]。
    // shadow_depth_vp: 各级联光空间线性深度矩阵（DepthProj*view），[kMaxCascades][16]。
    static void UploadSunData(
        ShaderManager& shader_mgr,
        unsigned int prog,
        const std::vector<CascadeFBO>& shadow_fbos,
        const float shadow_vp[][16],
        const float shadow_depth_vp[][16],
        const ShadowConfig& cfg,
        const std::optional<DirectionalLight>& sun);

    // ---- UploadAmbient ----
    // 把入参 ambient（AmbientLight）上传到蒙皮 PBR shader 的 uAmbientColor /
    // uAmbientIntensity。每帧在 Draw3DCommands 前调用一次。
    static void UploadAmbient(ShaderManager& shader_mgr,
                              unsigned int prog,
                              const AmbientLight& ambient);
};

}  // namespace jpov

#endif  // JPOV_SKELETON_RENDERER_H_
