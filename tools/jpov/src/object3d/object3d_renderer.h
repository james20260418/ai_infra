// JPOV Object3DRenderer — PBR 静态物体渲染 + Tile Forward 光照
//
// 管理 DrawObject3D 的 PBR shader、材质纹理绑定、点光源数据上传、
// 以及 CPU 端 tile culling。作为 Renderer 的内部组件，生命周期与 Renderer
// 相同。
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
    //   输入: vec3 aPos(loc=0)，uniform: mat4 uShadowMVP（光空间 ViewProj × Model）
    // 把顶点变换到太阳正交相机裁剪空间，输出光空间 ndc.z（[-1,1]）供写入阴影纹理。
    static constexpr const char* kShadowVs = R"glsl(
#version 330 core
layout(location = 0) in vec3 aPos;
uniform mat4 uShadowMVP;
out float vShadowDepth;
void main() {
    vec4 clip = uShadowMVP * vec4(aPos, 1.0);
    gl_Position = clip;
    vShadowDepth = clip.z / clip.w;   // ndc 深度 [-1,1]
}
)glsl";

    // kShadowFs: 阴影 pass fragment shader。把光空间深度写入颜色通道 .r。
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
};

uniform Light uLights[MAX_TOTAL_LIGHTS];
uniform int uTotalLights;
uniform vec3 uCameraPos;

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
uniform sampler2D uShadowMap[5];      // 各级联光空间深度贴图（TEXTURE7+i，.r = ndc.z）
uniform mat4  uShadowVP[5];           // 各级联光空间 ViewProj
uniform float uShadowTexel[5];        // 各级联 1.0/shadow map 尺寸（PCF 纹素步长）
uniform float uShadowBias;            // 深度偏移，抗自阴影 acne
uniform float uShadowFadeStart;       // 影子淡出起点（距相机）
uniform float uShadowFadeEnd;         // 影子淡出终点（此距离后无影子）

// 全局环境光（AmbientLight）：无方向、无影子，照亮背阳面。
uniform vec3  uAmbientColor;    // 环境光颜色（RGB）
uniform float uAmbientStrength; // 环境光强度（标量，乘 color）

const float PI = 3.14159265;

// 阴影因子：对世界坐标在指定级联的光空间里采样深度贴图，做 3×3 PCF。
// 返回 [0,1]，1=完全受照，0=完全在影子里。
// shadow map 存的是光空间 ndc.z（[-1,1]，正交投影下与线性能深对应）。
// 用 slope-scaled bias 抗自阴影 acne：掠射角越大（dot(N,L) 越小）偏置越大。
// ⚠️ GLSL 330 桌面版禁止非编译期常量的 sampler 数组索引，故各级联必须拆成
// 独立函数（或 if/else 全展开），不能 shadowFactorCascade(c, ...) 里动态取
// uShadowMap[c]。这里按 kMaxCascades=5 手写全展开。
float shadowFactorC0(vec3 world_pos, vec3 N, vec3 L) {
    vec4 lsp = uShadowVP[0] * vec4(world_pos, 1.0);
    vec3 ndc = lsp.xyz / lsp.w;
    vec2 uv = ndc.xy * 0.5 + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) return 1.0;
    float slopeBias = uShadowBias * (1.0 - dot(N, L));
    float cur = ndc.z - slopeBias;
    float shadow = 0.0;
    for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++)
        shadow += (cur <= texture(uShadowMap[0], uv + vec2(float(dx), float(dy)) * uShadowTexel[0]).r) ? 1.0 : 0.0;
    return shadow / 9.0;
}
float shadowFactorC1(vec3 world_pos, vec3 N, vec3 L) {
    vec4 lsp = uShadowVP[1] * vec4(world_pos, 1.0);
    vec3 ndc = lsp.xyz / lsp.w;
    vec2 uv = ndc.xy * 0.5 + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) return 1.0;
    float slopeBias = uShadowBias * (1.0 - dot(N, L));
    float cur = ndc.z - slopeBias;
    float shadow = 0.0;
    for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++)
        shadow += (cur <= texture(uShadowMap[1], uv + vec2(float(dx), float(dy)) * uShadowTexel[1]).r) ? 1.0 : 0.0;
    return shadow / 9.0;
}
float shadowFactorC2(vec3 world_pos, vec3 N, vec3 L) {
    vec4 lsp = uShadowVP[2] * vec4(world_pos, 1.0);
    vec3 ndc = lsp.xyz / lsp.w;
    vec2 uv = ndc.xy * 0.5 + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) return 1.0;
    float slopeBias = uShadowBias * (1.0 - dot(N, L));
    float cur = ndc.z - slopeBias;
    float shadow = 0.0;
    for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++)
        shadow += (cur <= texture(uShadowMap[2], uv + vec2(float(dx), float(dy)) * uShadowTexel[2]).r) ? 1.0 : 0.0;
    return shadow / 9.0;
}
float shadowFactorC3(vec3 world_pos, vec3 N, vec3 L) {
    vec4 lsp = uShadowVP[3] * vec4(world_pos, 1.0);
    vec3 ndc = lsp.xyz / lsp.w;
    vec2 uv = ndc.xy * 0.5 + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) return 1.0;
    float slopeBias = uShadowBias * (1.0 - dot(N, L));
    float cur = ndc.z - slopeBias;
    float shadow = 0.0;
    for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++)
        shadow += (cur <= texture(uShadowMap[3], uv + vec2(float(dx), float(dy)) * uShadowTexel[3]).r) ? 1.0 : 0.0;
    return shadow / 9.0;
}
float shadowFactorC4(vec3 world_pos, vec3 N, vec3 L) {
    vec4 lsp = uShadowVP[4] * vec4(world_pos, 1.0);
    vec3 ndc = lsp.xyz / lsp.w;
    vec2 uv = ndc.xy * 0.5 + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) return 1.0;
    float slopeBias = uShadowBias * (1.0 - dot(N, L));
    float cur = ndc.z - slopeBias;
    float shadow = 0.0;
    for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++)
        shadow += (cur <= texture(uShadowMap[4], uv + vec2(float(dx), float(dy)) * uShadowTexel[4]).r) ? 1.0 : 0.0;
    return shadow / 9.0;
}

// 按片元到相机的距离选级联（0 起），并对影子做线性淡出。
// 返回最终阴影因子 [0,1]（1=完全受照无影，0=全影）。
// 选段用 if/else 链（cascade 索引是编译期常量，避开 sampler 数组动态索引限制）。
float computeSunShadow(vec3 world_pos, vec3 N, vec3 L, float frag_dist) {
    if (uCascadeCount <= 0 || uHasSun == 0) return 1.0;
    if (frag_dist >= uShadowFadeEnd) return 1.0;   // 淡出结束：无影子
    int c = 0;
    for (int i = 0; i < uCascadeCount; i++)
        if (frag_dist <= uCascadeRanges[i]) { c = i; break; }
    float shadow;
    if (c == 0) shadow = shadowFactorC0(world_pos, N, L);
    else if (c == 1) shadow = shadowFactorC1(world_pos, N, L);
    else if (c == 2) shadow = shadowFactorC2(world_pos, N, L);
    else if (c == 3) shadow = shadowFactorC3(world_pos, N, L);
    else shadow = shadowFactorC4(world_pos, N, L);
    // 淡出：fade 从 1（frag_dist<=fade_start）线性降到 0（frag_dist>=fade_end）。
    // shadow = mix(1.0, shadow, fade)：fade=0 时 shadow=1.0（无影子，亮度变大）。
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
        total_diffuse += uLights[li].color * diffuse * NdotL * attenuation;
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
        total_specular += uLights[li].color * spec * NdotL_s * attenuation;
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

    vec3 ambient = uAmbientColor * uAmbientStrength;
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

    vec3 result = ambient * base_color * ao + total_diffuse + total_specular + emissive;
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
    // 从太阳正交光空间视角画进阴影纹理（只写光空间 ndc.z 到颜色 .r，不光照）。
    static void DrawObject3DShadow(const Object3DCommand& cmd,
                                   MeshManager& mesh_mgr,
                                   ShaderManager& shader_mgr,
                                   const float shadow_vp[16],
                                   unsigned int shadow_prog);

    // ---- UploadSunData ----
    // 把 cmds.sun（DirectionalLight）与级联阴影贴图参数上传到 PBR shader。
    // 无 sun 时仅把 uHasSun 置 0。两个 shader program 都上传。
    // shadow_fbos: 各级联 shadow FBO（取 .tex 绑到 TEXTURE7+i），
    //              长度须 == cfg.cascade_count。
    // shadow_vp:   各级联光空间 ViewProj，[kMaxCascades][16]。
    static void UploadSunData(
        ShaderManager& shader_mgr,
        unsigned int prog,
        unsigned int prog_full,
        const std::vector<CascadeFBO>& shadow_fbos,
        const float shadow_vp[][16],
        const ShadowConfig& cfg,
        const std::optional<DirectionalLight>& sun);

    // ---- UploadAmbient ----
    // 把入参 ambient（AmbientLight）上传到 PBR shader 的 uAmbientColor /
    // uAmbientStrength。每帧在 Draw3DCommands 前调用一次（与 sun/点光源并列），
    // 两个 shader program 都上传。
    static void UploadAmbient(ShaderManager& shader_mgr,
                              unsigned int prog,
                              unsigned int prog_full,
                              const AmbientLight& ambient);
};

}  // namespace jpov

#endif  // JPOV_OBJECT3D_RENDERER_H_
