// JPOV 水皮（water skin）pass —— GLSL
//
// 语义：世界 `y = uLevelY` 的一张**无限水平平面**（纯平面，无波浪、无几何位移）。
// 只画「界面」这一层：反射天光（环境立方体探针）+ 菲涅尔透射；相机在水下时
// 变成「Snell 窗 + 全内反射」。水体介质的吸收/散射**不在本 pass**（下一步交给体积雾）。
//
// ⭐ 为什么是「全屏三角形 + 解析求交」而不是一块网格/quad（2026-10-28 已验证，别重开）：
//   视线越接近水平，与平面的交点距离 → ∞（眼高 1.5 m 时地平线在 4.4 km）。
//   任何有限 quad 都会在「quad 远端边界 ↔ 真地平线」之间留一条缝，缝里是天球下半球的
//   ground 色 —— 看得到一片"陆地"。几何上无解 ⇒ 只能逐像素解 `t = (level − cam.y)/dir.y`。
//   副产品：没有「水面尺寸」这个参数（接口里也就不给）。
//
// ⭐ 输出（WBO 覆盖集，见 src/wbo/wbo_target.h）：
//     att0 accum  = vec4(反射色 × F, F)   ← 按 (ONE, ONE) 累加，w = 1
//     att1 reveal = F                      ← 按 (ZERO, ONE_MINUS_SRC_COLOR) 累加，
//                                            单层累完后纹理里就是 1 − F
//     att3 depth  = z_view × F             ← 代表深度 = 水面交点的视轴深度
//   ⚠️ reveal 槽要写 **α 本身**（不是 1−α）：WBO 的 revealage 混合是
//      dst' = dst·(1 − src)，清成 1 ⇒ 单层后 = 1 − α。写反会得到一个“反相”的
//      菲涅尔（掠射变暗、俯视变亮），2026-11-15 首帧实测就是这个错。
//   合成（WboComposite）做 `out = C·cov + dst·(1−cov)`，即标准的界面 over：
//     out = 反射色·F + 背景·(1−F)
//   其中「背景」是已经画好的不透明场景（含天空）—— 所以本 pass **不采样场景颜色**，
//   也不需要抓帧副本。等体积雾接入后，这一层会交给雾的「雾段 + 层」交替积分。
//
// ⭐ 为什么走 WBO 而不是直接 alpha 混合（Danis 2026-11-15 定）：
//   下一步的「水下吸收 + 注入」由雾按层积分，水皮必须是带代表深度的**层**，
//   而不是一个已经拌进画面的像素。WBO 只是承载这一层的现成载体。
//
// 本版**不做**（写在前面，免得当成 bug）：
//   · 波浪：法线恒 (0,1,0)，反射不抖、没有太阳路；
//   · 折射：窗内不压缩天球（按原视线方向看天），水面之下也不位移；
//   · 消光 / 散射：水下段不衰减（下一步交给体积雾）；
//   · 平面反射：倒影只有天光探针（映不出岛屿/人）；
//   · 太阳高光：探针按设计不含日月盘（SkyOutput::kEnvProbe）⇒ 水面上没有太阳的像。

#ifndef JPOV_SRC_WATER_WATER_SKIN_SHADER_H_
#define JPOV_SRC_WATER_WATER_SKIN_SHADER_H_

namespace jpov {
namespace water {

// 全屏三角形（3 顶点，无 VBO）。vNdc 在屏幕内插值出 [-1,1] 的 NDC 坐标。
// 顶点取 (-1,-1) (3,-1) (-1,3)：覆盖整个视口，且不会在 llvmpipe 上触发
// 「超大三角形 varying 插值退化」（本项目天空/雾都因此改成全屏 + 反投影）。
inline constexpr const char* kWaterSkinVs = R"glsl(
#version 330 core
out vec2 vNdc;
void main() {
    vec2 c = vec2((gl_VertexID == 1) ? 3.0 : -1.0,
                  (gl_VertexID == 2) ? 3.0 : -1.0);
    vNdc = c;
    gl_Position = vec4(c, 0.0, 1.0);
}
)glsl";

// 片元着色器 —— 与 src/water/water_skin_math.h **同源**（改一处必须改另一处）。
//
// 相机基约定（与 Primitives3DRenderer::BuildMVP 的 lookAt 一致）：
//   fwd = normalize(target − position)
//   right = normalize(cross(fwd, up))
//   up' = cross(right, fwd)            ← 必须正交化，否则与投影矩阵的 img 不一致
//   rd = normalize(fwd + right·(ndc.x·tanHalfFov·aspect) + up'·(ndc.y·tanHalfFov))
inline constexpr const char* kWaterSkinFs = R"glsl(
#version 330 core
in vec2 vNdc;

layout(location = 0) out vec4  oAccum;    // (预乘反射色, α)
layout(location = 1) out float oReveal;   // 1 − α
layout(location = 3) out float oDepth;    // z_view · α

uniform vec2  uViewport;     // 3D FBO 尺寸（把 gl_FragCoord 换成 uv 采场景深度）
uniform vec3  uCamPos;       // 相机世界位置
uniform vec3  uCamFwd;       // 视线方向（单位）
uniform vec3  uCamRight;     // 右（单位，已正交化）
uniform vec3  uCamUp;        // 上（单位，已正交化）
uniform float uTanHalfFov;   // tan(fov/2)
uniform float uAspect;       // 宽/高
uniform float uNear;
uniform float uFar;
uniform float uLevelY;       // 水面世界高度（米）
uniform float uIor;          // 水的折射率（只用于 Fresnel 与全内反射）
uniform float uEnvScale;     // 天光反射消费量 [0,1]（= EnvSpecular::sky_scale）
uniform vec3  uTirColor;     // 水下全内反射色（= SkyCommand::ground_color）
uniform float uHorizonClampSin;  // 反射方向夹到地平线以上的最小 y（= sin 1°）
uniform float uDepthBiasM;       // 水面与场景深度的比较容差（米）
uniform samplerCube uEnvCube;    // 天光探针（大气色，不含日月盘）
uniform sampler2D   uSceneDepth; // MRT#1 场景深度（gl_FragCoord.z，1.0 = 背景/远平面）

const float kBackgroundDepth = 0.999999;
const float kEnvLod = 0.0;   // 纯平面 = 镜面；探针只有 128²，LOD0 对天空已足够

// 无偏振电介质菲涅尔（含全内反射）。与 water_skin_math.h 的 FresnelDielectric 同源。
float FresnelDielectric(float cos_i, float n1, float n2) {
    float eta = n1 / n2;
    float sin2_t = eta * eta * (1.0 - cos_i * cos_i);
    if (sin2_t >= 1.0) {
        return 1.0;   // 全内反射
    }
    float cos_t = sqrt(1.0 - sin2_t);
    float rs = (n1 * cos_i - n2 * cos_t) / (n1 * cos_i + n2 * cos_t);
    float rp = (n1 * cos_t - n2 * cos_i) / (n1 * cos_t + n2 * cos_i);
    return 0.5 * (rs * rs + rp * rp);
}

// NDC 深度 → 视轴深度（米）。与 water_skin_math.h 的 LinearizeDepthNdc 同源。
float LinearizeDepthNdc(float ndc_z, float near_plane, float far_plane) {
    float ndc = ndc_z * 2.0 - 1.0;
    return 2.0 * near_plane * far_plane / (far_plane + near_plane - ndc * (far_plane - near_plane));
}

void main() {
    vec3 rd = normalize(uCamFwd
                        + uCamRight * (vNdc.x * uTanHalfFov * uAspect)
                        + uCamUp    * (vNdc.y * uTanHalfFov));

    // ── 视线 ∩ 平面 ──
    if (abs(rd.y) < 1.0e-6) {
        discard;   // 与水面平行：没有交点
    }
    float t = (uLevelY - uCamPos.y) / rd.y;
    if (t <= 0.0) {
        discard;   // 交点在相机后方
    }

    // ── 场景深度拒绝 ──
    // 视轴深度（不是欧氏距离）：MRT#1 存的是 gl_FragCoord.z。
    float view_z = t * dot(rd, uCamFwd);
    float scene_ndc = texture(uSceneDepth, gl_FragCoord.xy / uViewport).r;
    if (scene_ndc < kBackgroundDepth) {
        float scene_z = LinearizeDepthNdc(scene_ndc, uNear, uFar);
        if (view_z > scene_z + uDepthBiasM) {
            discard;   // 不透明场景挡在水面前面
        }
    }

    // ── 菲涅尔 ──
    bool  under = (uCamPos.y < uLevelY);
    float cos_i = abs(rd.y);
    float F = under ? FresnelDielectric(cos_i, uIor, 1.0)
                    : FresnelDielectric(cos_i, 1.0, uIor);

    // ── 反射色 ──
    // 水上：天光探针沿镜像方向。方向必须夹到地平线以上 —— 探针下半球是"地色"，
    //       掠射时不夹会在水平线上画一条黑边（2026-10-28 实测）。
    // 水下：窗外是全内反射（R=1），反射的是水下环境，本版用常量地色代替；
    //       窗内 R 很小，(1−R) 的背景自然透出（背景 = 已经画好的天空/岛屿）。
    vec3 refl;
    if (under) {
        refl = uTirColor;
    } else {
        vec3 rdir = vec3(rd.x, -rd.y, rd.z);
        // 与 water_skin_math.h 的 ClampReflectionAboveHorizon 同源：夹住 y，再把 xz
        // 缩到 sqrt(1−y²)，结果精确在单位球上且 y ≥ 夹值。
        float xz = sqrt(rdir.x * rdir.x + rdir.z * rdir.z);
        if (rdir.y < uHorizonClampSin) {
            if (xz > 1.0e-6) {
                float scale = sqrt(1.0 - uHorizonClampSin * uHorizonClampSin) / xz;
                rdir = vec3(rdir.x * scale, uHorizonClampSin, rdir.z * scale);
            } else {
                rdir = vec3(0.0, 1.0, 0.0);
            }
        } else {
            rdir = normalize(rdir);
        }
        refl = textureLod(uEnvCube, rdir, kEnvLod).rgb * uEnvScale;
    }

    // ── WBO 覆盖集 ──
    // reveal 槽写 α（不是 1−α）：混合是 dst·(1−src)，单层后 = 1−F。
    oAccum  = vec4(refl * F, F);
    oReveal = F;
    oDepth  = view_z * F;
}
)glsl";

}  // namespace water
}  // namespace jpov

#endif  // JPOV_SRC_WATER_WATER_SKIN_SHADER_H_
