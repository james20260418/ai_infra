// JPOV 水皮（water skin）pass —— GLSL
//
// 语义：世界 `y = uWaterLevelY` 的一张**无限水平平面**（纯平面、无法线扰动、无几何位移）。
// 只画「界面」这一层，**不管水体介质**（介质的吸收/注入由合成趟的 MediumAt 负责）。
//
// ⭐ 为什么是「全屏三角形 + 解析求交」而不是一块网格/quad（2026-10-28 已验证，别重开）：
//   视线越接近水平，与平面的交点距离 → ∞（眼高 1.5 m 时地平线在 4.4 km）。
//   任何有限 quad 都会在「quad 远端边界 ↔ 真地平线」之间留一条缝，缝里是天球下半球的
//   ground 色 —— 看得到一片"陆地"。几何上无解 ⇒ 只能逐像素解 `t = (level − cam.y)/dir.y`。
//   副产品：没有「水面尺寸」这个参数（接口里也就不给）。
//
// ⭐ 输出 = **一层**（WBO 覆盖集，见 src/wbo/wbo_target.h 与 docs/jpov_water_contract.md §2）：
//     att0 accum  = vec4(C, α)   ← (ONE, ONE) 累加：C 是**预乘色**、α 是遮挡率
//     att1 reveal = α            ← (ZERO, ONE_MINUS_SRC_COLOR) ⇒ 单层后 = 1 − α
//     att3 depth  = t · α        ← **射线距离**（不是视轴深度！）
//   ⚠️ reveal 槽要写 **α 本身**：WBO 的 revealage 混合是 dst' = dst·(1 − src)，
//      清成 1 ⇒ 单层后纹理里就是 1 − α。写反会得到“反相菲涅尔”。
//   ⚠️ att3 必须是**射线距离 t**：合成趟的 `FieldAt(ro, rd, z)` 与 `z_op` 都以射线参数为准；
//      写视轴深度会让层被放早 1/cosθ（画面角落 ~1.5×），且贴水面那段水体的注入被吃掉。
//
// ⭐ 层的两个分支（契约 §2.2 / §2.3）
//   水下（相机在水面以下）：
//     · 环外（θi > 临界角）Fresnel 返回 1 ⇒ α = 1，整个背景被盖掉；
//     · 环内 α = F（2%~6%），背景（= 已经过天空折射的天球）透出；
//     · C = F · S_P(∞) —— 「深海积分反射」：从 P 沿镜像方向到**无穷远**的介质积分，
//       不是新积分器，就是合成趟那个 `MediumAt` 取 `z → ∞`。
//   水上（相机在水面以上）：
//     · C = F · sky(mirror)（探针；将来接 SSR 时换成 A·SSR + (1−A)·sky）；
//     · α = 1 − (1−F)/n² —— **÷n² 折进透射率**：水下内容透过界面观察要按 L/n² 守恒变暗。
//       （注意 α 在这里不是 F：它表示“背景衰减率”，注释义务见契约 §2.3）
//
// 本版**不做**（写在前面，免得当成 bug）：
//   · 波浪：法线恒 (0,1,0)，反射不抖、没有太阳路；
//   · 折射位移：窗内天球的压缩与 n² 由**天空 pass** 负责（契约 §3），本 pass 不碰；
//   · 平面反射：水上支路的倒影只有天光探针（映不出岛/人；SSR 未接）；
//   · 太阳高光 / 太阳的像：探针不含日月盘，且平面下是 δ 函数（known_issue §10.10）。

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

// 片元着色器 —— 与 src/water/water_skin_math.h **同源**（改一处必须改另一处，
// 由 test/water:water_skin_gpu_test 逐像素对拍兜底）。
//
// 相机基约定（与 Primitives3DRenderer::BuildMVP 的 lookAt 一致）：
//   fwd = normalize(target − position)
//   right = normalize(cross(fwd, up))
//   up' = cross(right, fwd)            ← 必须正交化，否则与投影矩阵的 img 不一致
//   rd = normalize(fwd + right·(ndc.x·tanHalfFov·aspect) + up'·(ndc.y·tanHalfFov))
//
// 介质部分（`MediumAt` 与那组 uWater* uniform）由 `kWaterMediumGlsl` 提供：
// 本 pass 只**调用**它取 S_P(∞)，不重复实现物理。
// ⚠️ 本片段**不含 `#version`**：使用方要在最前面拼 `#version 330 core\n`，再拼
//    `kWaterMediumGlsl`（介质片段），最后拼本片段 —— 与 fog 合成趟的组装顺序一致。
inline constexpr const char* kWaterSkinFs = R"glsl(
in vec2 vNdc;

layout(location = 0) out vec4  oAccum;    // (C, α)：C 为预乘色
layout(location = 1) out float oReveal;   // α 本身
layout(location = 3) out float oDepth;    // t · α（射线距离）

uniform vec2  uViewport;     // 3D FBO 尺寸（把 gl_FragCoord 换成 uv 采场景深度）
uniform vec3  uCamPos;       // 相机世界位置
uniform vec3  uCamFwd;       // 视线方向（单位）
uniform vec3  uCamRight;     // 右（单位，已正交化）
uniform vec3  uCamUp;        // 上（单位，已正交化）
uniform float uTanHalfFov;   // tan(fov/2)
uniform float uAspect;       // 宽/高
uniform float uNear;
uniform float uFar;
uniform float uEnvScale;     // 天光反射消费量 [0,1]（= EnvSpecular::sky_scale）
uniform float uHorizonClampSin;  // 反射方向夹到地平线以上的最小 y（= sin 1°）
uniform float uDepthBiasM;       // 水面与场景深度的比较容差（米）
uniform samplerCube uEnvCube;    // 天光探针（大气色，不含日月盘）
uniform sampler2D   uSceneDepth; // MRT#1 场景深度（gl_FragCoord.z，1.0 = 背景/远平面）

const float kBackgroundDepth = 0.999999;
const float kEnvLod = 0.0;        // 纯平面 = 镜面；探针只有 128²，LOD0 对天空已足够
const float kDeepSeaRayT = 1.0e6; // “无穷远”的射线参数（均质介质下 τ 早已饱和）
const float kHorizonEps = 1.0e-4; // 地平线钳位（|rd.y| 下限）⇒ 层多盖几像素，堵住地缝

// 无偏振电介质菲涅尔（含全内反射）。与 water_optics.h 的 FresnelDielectric 同源。
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
    // 地平线附近（|rd.y| ≈ 0）**不 discard**：把方向钳到极小的 |dy|，让这一层照样覆盖上去。
    // 否则地平线那 1~2 像素的层缺失 ⇒ 露出天空在地平线以下填的 ground_color（“地缝”）。
    // 这是「把水面稍微拉大几像素盖住地缝」的落点：不改 ground_color、不填色兜底。
    if (abs(rd.y) < kHorizonEps) {
        float sgn = (uCamPos.y < uWaterLevelY) ? 1.0 : -1.0;   // 让交点在前方（不能写 const：非编译期常量）
        rd = normalize(vec3(rd.x, sgn * kHorizonEps, rd.z));
    }
    float t = (uWaterLevelY - uCamPos.y) / rd.y;
    if (t <= 0.0) {
        discard;   // 交点在相机后方
    }

    // ── 场景深度拒绝（用视轴深度比较：MRT#1 存的是 gl_FragCoord.z）──
    float view_z = t * dot(rd, uCamFwd);
    float scene_ndc = texture(uSceneDepth, gl_FragCoord.xy / uViewport).r;
    if (scene_ndc < kBackgroundDepth) {
        float scene_z = LinearizeDepthNdc(scene_ndc, uNear, uFar);
        if (view_z > scene_z + uDepthBiasM) {
            discard;   // 不透明场景挡在水面前
        }
    }

    // ── 菲涅尔 ──
    bool  under = (uCamPos.y < uWaterLevelY);
    float cos_i = abs(rd.y);
    float F = under ? FresnelDielectric(cos_i, uWaterIor, 1.0)
                    : FresnelDielectric(cos_i, 1.0, uWaterIor);

    vec3  C;      // 层色（P 点辐射度，未经相机段衰减）
    float alpha;  // 层遮挡率
    if (under) {
        // 深海积分反射：从 P 沿镜像方向到无穷远的介质积分（同一个 MediumAt，z→∞）。
        // ⚠️ P 往水面下推 1 mm：MediumAt 用 ro.y 判“从空气进水/本来就在水里”，
        //    落在平面上时是浮点不确定的（会影响折射后的 dy_w 与 B）。
        vec3 P = uCamPos + rd * t;
        P.y -= 1.0e-3;
        vec3 mirror = vec3(rd.x, -rd.y, rd.z);
        vec3 tau_deep;
        vec3 S_deep;
        MediumAt(P, mirror, kDeepSeaRayT, tau_deep, S_deep);
        C = F * S_deep;
        alpha = F;          // 环外 Fresnel 已回到 1 ⇒ α = 1（整片替换）
    } else {
        // 水面以上：天光探针沿镜像方向（方向必须夹到地平线以上 —— 探针下半球是“地色”）。
        vec3 rdir = vec3(rd.x, -rd.y, rd.z);
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
        vec3 refl = textureLod(uEnvCube, rdir, kEnvLod).rgb * uEnvScale;
        C = F * refl;
        // 透射率 = (1−F)/n²（辐射度跨介质守恒）；WBO 的 α 是“背景衰减率”= 1 − T。
        alpha = 1.0 - (1.0 - F) / (uWaterIor * uWaterIor);
    }

    // ── WBO 覆盖集 ──
    oAccum  = vec4(C, alpha);
    oReveal = alpha;
    oDepth  = t * alpha;      // 代表深度 = 射线距离 × 遮挡率
}
)glsl";

}  // namespace water
}  // namespace jpov

#endif  // JPOV_SRC_WATER_WATER_SKIN_SHADER_H_
