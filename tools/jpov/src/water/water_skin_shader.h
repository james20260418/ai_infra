// JPOV 水皮（water skin）pass —— GLSL
//
// 语义：世界 `y = level_y` 的基准面上，叠加**波浪高度场 h(x,z,t)**（可为零）。
//   · 无波浪（uWaveCount = 0）⇒ 纯平面，逐字节退化为旧版；
//   · 有波浪 ⇒ 求交改为「平面初值 + Newton」，法线取高度场解析梯度。
// 只画「界面」这一层，**不管水体介质**（介质的吸收/注入由合成趟的 MediumAt 负责）。
//
// ⭐ 为什么是「全屏三角形 + 解析求交」而不是一块网格/quad（2026-10-28 已验证，别重开）：
//   视线越接近水平，与平面的交点距离 → ∞（眼高 1.5 m 时地平线在 4.4 km）。
//   任何有限 quad 都会在「quad 远端边界 ↔ 真地平线」之间留一条缝，缝里是天球下半球的
//   ground 色 —— 看得到一片"陆地"。几何上无解 ⇒ 只能逐像素解交点。
//   波浪只把「平面解析解」换成「高度场 Newton 解」，屏幕空间求交这条路径不变。
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
//     · C = F · S_P(∞) —— 「深海积分反射」：从 P 沿镜像方向到**无穷远**的介质积分。
//   水上（相机在水面以上）：
//     · C = F · sky(mirror)（探针；将来接 SSR 时换成 A·SSR + (1−A)·sky）；
//     · α = 1 − (1−F)/n² —— **÷n² 折进透射率**：水下内容透过界面观察要按 L/n² 守恒变暗。
//
// ⚠️ 波浪（2026-11-18 起）只改**界面几何**：求交点、法线、菲涅尔、反射方向、日盘像。
//    水体介质仍按 `level_y` 的水平半空间积分（波浪化介质见 known_issue §11.5，另 PR）。

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
// 介质部分（`MediumAt` 与那组 uWater* uniform）由 `kWaterMediumGlsl` 提供；
// 波浪部分（`WaveSampleAt` / `WaveSurfaceHit` 与 uWave* uniform）由 `kWaterWaveGlsl` 提供：
// 本 pass 只**调用**它们，不重复实现物理。
// ⚠️ 本片段**不含 `#version`**：使用方要在最前面拼 `#version 330 core\n`，再拼
//    `kWaterMediumGlsl`、`kWaterWaveGlsl`、GGX 片段，最后拼本片段。
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

uniform int uWaveDebug;          // 1 = 输出法线颜色（debug）

// ── 太阳的像（解析 GGX，盘状光源）──
// 由 Renderer 从 cmds.sun 填。与介质那份 uWaterSun* 分开：介质可以关，水面依然有日盘像。
uniform int   uHasSun;            // 0 = 无太阳 ⇒ 这一项恒 0
uniform vec3  uSunToward;         // 指向太阳（气侧，单位）
uniform vec3  uSunIrradiance;     // color × intensity（辐照度语义）
uniform float uSunAngularRadius;  // 盘角半径（弧度，> 0）
uniform float uSunFootprint;      // 像素足迹带宽开关（1 = 开；GPU 对拍测试置 0）

const float kBackgroundDepth = 0.999999;
const float kDeepSeaRayT = 1.0e6; // “无穷远”的射线参数（均质介质下 τ 早已饱和）
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

// 粗糙界面的**反射方向反照率**（角度平均菲涅尔）：
//   R_eff(μo, α) = ∫ f_ss(ωo,ωi)·F(ωo·h)·cosθi dωi
// 掠射时远低于平面菲涅尔：2° 仰角下 0.80 → 0.34（σ²≈0.017）/0.24（0.05）/0.15（0.15）。
// 差额就是进了透射的那部分（T_eff = 1 − R_eff）→ 海面因此不会“水天一色”。
//
// ⚠️ **输入是斜率方差 σ²，不是 a2**（2026-11-24 修，与 water_optics.h 同源）：
//    Beckmann ↔ GGX 的标准映射是 α² = 2σ²（GGX 自身方差 = α²/2）；旧版把 σ² 当 a2 用
//    ⇒ 粗糙度少一半 ⇒ 远海反射率偏镜面/偏亮。
//
// 解析近似（对 12×6 的 Monte-Carlo 表拟合，平均误差 0.026，掠射段 <0.03）：
//   R_eff ≈ mix( F(cos_i + 0.52·α^0.72), F(cos_i), smoothstep(0.05, 0.5, cos_i) )
//   前者负责掠射（粗面把掠射峰压下去），后者保证高仰角回到平面值。
float RoughFresnelAlbedo(float cos_i, float slope_var, float ior) {
    float a2 = 2.0 * max(slope_var, 0.0);
    float s = sqrt(a2);
    float shifted = FresnelDielectric(clamp(cos_i + 0.52 * pow(max(s, 1.0e-6), 0.72),
                                           1.0e-4, 0.999), 1.0, ior);
    float plain = FresnelDielectric(clamp(cos_i, 1.0e-4, 0.999), 1.0, ior);
    float w = smoothstep(0.05, 0.5, cos_i);
    return mix(shifted, plain, w);
}

void main() {
    vec3 rd = normalize(uCamFwd
                        + uCamRight * (vNdc.x * uTanHalfFov * uAspect)
                        + uCamUp    * (vNdc.y * uTanHalfFov));

    // ── ① 太阳像的半向量与像素足迹：**必须无条件先算** ──
    //    dFdx/dFdy 需要 uniform control flow：discard 或变值分支之后求导是未定义的。
    //    这里的 rd 是**未夹地平线的原始视线**（地平线夹取只作用于探针支路）。
    //    半向量 H 与法线无关 ⇒ 可以在求交（可能 discard）之前算完；NoV/NoL/NoH 之后用 N 算。
    vec3 Vw = -rd;                       // P → 相机
    vec3 H = Vw + uSunToward;            // 半向量（未归一）
    float hlen = length(H);
    H = (hlen > 1.0e-6) ? (H / hlen) : vec3(0.0, 1.0, 0.0);
    vec3 dHdx = dFdx(H);
    vec3 dHdy = dFdy(H);
    // 像素足迹并入斜率方差（specular AA 口径，docs/jpov_specular_bandwidth.md §1.3③）：
    // 平面法线恒定，H 的变化全来自视线 ⇒ 掠射时这一项自动变大，正好抵消投影压缩。
    float footprint_a2 = 0.25 * (dot(dHdx, dHdx) + dot(dHdy, dHdy));

    // ── ③ 视线 ∩ 水面（无波浪 ⇒ 平面解析解；有波浪 ⇒ 二分求交）──
    // ⚠️ 这里**不能 discard**（2026-11-24 修）：下面的 dFdx(N) 必须在整 quad 一致的控制流里求值；
    //    地平线那一行的 quad 里一半像素 t<=0，一旦 discard，helper invocation 被杀死 ⇒
    //    导数取到垃圾 ⇒ normal_var_px 整行爆掉 ⇒ a2 / env_lod 错 ⇒ 地平线上一条 1px 亮/暗线
    //    （这正是历史上反复追的「地平线裂隙/暗带」的机器）。真正 discard 放到所有导数之后。
    float t_raw = (uWaterLevelY - uCamPos.y) / rd.y;
    bool has_hit = (t_raw > 0.0);
    float t = max(t_raw, 1.0e-3);   // 不可见的像素用安全值，只为让导数有定义
    // 平面交点距离：**平滑**、与波浪无关。只用于深度测试（见 ④）——波浪求交的 t 在掠射时
    // 会因"先撞到哪个波峰"在相邻像素间跳几十米到几公里，用它比较会出单像素级的误剔/误留。
    float t_plane = t;

    // ── ③b 法线 + 像素内法线方差（specular AA 的唯一正确来源）──
    //    ⚠️ 这里**不能 discard**：dFdx/dFdy 必须在近 uniform 的控制流里求值。
    //    wave_ok=false 的像素留到最后再 discard（它们只在边缘，导数误差可忽略）。
    vec3 N = vec3(0.0, 1.0, 0.0);        // 无波浪 = 纯平面法线
    bool wave_ok = true;
    // 像素在水面上覆盖的世界尺寸（用平面 t 估；掠射时按 |rd.y| 放大）。
    // 公式与护栏在 kWaterWaveGlsl::WaveFootprint（与 CPU water::WaveFootprintM 同源）：
    // 护栏只是除零护栏（1e-4），不再有 0.15 的"遮丑地板"。
    float pixel_angle = 2.0 * uTanHalfFov / uViewport.y;
    float footprint = WaveFootprint(uCamPos, rd, uWaterLevelY, pixel_angle);
    if (uWaveCount > 0) {
        vec3 pos_w = vec3(0.0);
        vec3 nrm_w = vec3(0.0, 1.0, 0.0);
        float t_wave = t;
        wave_ok = WaveSurfaceHit(uCamPos, rd, uWaterLevelY, footprint, t_wave, pos_w, nrm_w);
        t = wave_ok ? t_wave : t;
        N = wave_ok ? nrm_w : vec3(0.0, 1.0, 0.0);
    }
    // 像素内法线变化量²（≈ 每像素斜率方差）。
    // 这是「谱里解析法线已确定、但像素盖住了一条波 ⇒ 反射方向无法逐像素解析」的补偿：
    //   · 太阳高光：并入 a2（GGX 带宽）；
    //   · 环境反射：并入探针 LOD（否则远处波峰交界 = 高频碎光/麻点）。
    // 无波浪时恒 0 ⇒ 与旧版逐字节一致（LOD0、a2 不变）。
    vec3 dNdx = dFdx(N);
    vec3 dNdy = dFdy(N);
    // **只用横向差分**（这是设计选择，不是权宜）：水面/天空的边界是**水平**的，
    // 那一行的 2×2 quad 里上半是天空（has_hit=false，N 没有意义）⇒ dFdy(N) 会把方差顶到
    // 天上；横向邻居始终是水面 ⇒ dFdx 安全。各向同性下 E|dN/dx|² = E|dN/dy|²，
    // 所以 0.5·|dFdx|² 与 0.25·(|dx|²+|dy|²) 期望相同。
    // （2026-11-24：以前这里还有个更严重的毛病 —— 上面 ③ 的 discard 发生在求导之前，
    //   整个 quad 的 helper invocation 被杀 ⇒ dFdx 本身也是垃圾；那条已修。）
    float normal_var_px = 0.5 * dot(dNdx, dNdx);
    // ── ③d 法线场过滤：把「看不见的波」折算成粗糙度 ─────────────────────────
    //   σ² = E|∇h|² = 被足迹截断的斜率方差 + 短波（毛细波/短重力波）。
    //   送进 BRDF 时要换成 α² = 2σ²（见 RoughFresnelAlbedo / a2 两处）。
    float sxx, sxz, szz;
    WaveSlopeTensor(footprint, sxx, sxz, szz);
    // 两个投影，各有各的消费者（2026-11-24 修：以前只有一个"视线投影"被三个消费者共用）：
    //   · var_p_slope：入射面方向（视线在 xz 的投影）—— 反射方向在该平面内摆动，
    //     所以探针模糊（env_lod）用它是正确的；
    //   · var_p_mean：张量各向同性的平均（(σx²+σz²)/2）—— 反射率 R_eff 是**对半球积分**，
    //     不能用某一个方向的投影（8 条主波只沿风 ±20~30° 散布 ⇒ 张量是强各向异性的，
    //     用视线方位会在侧风时高估/低估最多 2~4×）。
    vec2 td = rd.xz;
    float tdl = length(td);
    td = (tdl > 1.0e-6) ? (td / tdl) : vec2(1.0, 0.0);
    float var_p = td.x * td.x * sxx + 2.0 * td.x * td.y * sxz + td.y * td.y * szz;
    float var_p_mean = 0.5 * (sxx + szz);
    // 消费者：F_eff（用 var_p_mean）、探针 LOD（用 var_p）、太阳的像的 a2（用 normal_var_px）。
    if (!has_hit) {
        discard;   // 交点在相机后方（延迟到这里：见 ③ 的注释，导数必须先算完）
    }
    if (!wave_ok) {
        discard;   // 求交失败 / 交点跑到相机后方
    }

    // ── ④ 场景深度拒绝（用视轴深度比较：MRT#1 存的是 gl_FragCoord.z）──
    // ⚠️ 用**平面**交点距离，不用波浪求交的 t：后者在掠射时相邻像素能差几公里 ⇒
    //    view_z 抖动 ⇒ 与场景深度比较会在单像素上翻转（偶发单像素"z 不对"）。
    float view_z = t_plane * dot(rd, uCamFwd);
    float scene_ndc = texture(uSceneDepth, gl_FragCoord.xy / uViewport).r;
    // 深度拒绝。debug=3 = 全局跳过（对比用）。
    if (uWaveDebug == 3) {
        // 全局跳过（debug）
    } else if (scene_ndc < kBackgroundDepth) {
        float scene_z = LinearizeDepthNdc(scene_ndc, uNear, uFar);
        if (view_z > scene_z + uDepthBiasM) {
            discard;   // 不透明场景挡在水面前
        }
    }

    float env_lod_debug = 0.0;   // 调试用（uWaveDebug=11）
    vec3  refl_debug = vec3(0.0);   // 调试用（uWaveDebug=12）
    float FresnelDebug = 0.0;       // 调试用（uWaveDebug=13）
    // ── ⑤ 入射角与菲涅尔（法线可为波浪扰动法线）──
    bool  under = (uCamPos.y < uWaterLevelY);
    float cos_i = abs(dot(rd, N));
    float F = under ? FresnelDielectric(cos_i, uWaterIor, 1.0)
                    : FresnelDielectric(cos_i, 1.0, uWaterIor);
    // ③ 角度平均菲涅尔：粗糙界面的方向反照率。反射↓、透射↑（两边一起动）。
    //    没有开关 —— 这是正确算法（掠射 2° 平面 0.80 → 0.36@MSS0.017）。
    if (!under) {
        F = RoughFresnelAlbedo(cos_i, var_p_mean, uWaterIor);
    }

    // ── ⑥ 太阳的像（解析 GGX，盘状光源）—— 只做乘除，无导数 ⇒ 可在 discard 之后 ──
    vec3 sun_spec = vec3(0.0);
    float NoV = dot(Vw, N);
    if (uHasSun != 0) {
        float NoL = dot(uSunToward, N);
        if (NoV > 0.0 && NoL > 0.0) {
            float NoH = max(dot(H, N), 0.0);
            // 带宽 = 盘角半径（θ/2，半向量只偏一半）+ 像素足迹。
            // 上限 0.25（⇒ α ≤ 0.5，半角约 26°）是**数值兜底**：极端掠射时 dFdx 会把足迹
            // 方差推出数量级（H 在屏幕空间近似不连续），不夹会把日斑摊成一大片糊。
            // 带宽 = 盘角半径 + 像素足迹。
            // ⚠️ **不再叠加解析 σ_slope**（2026-11-18 定）：波浪法线是**逐像素解析梯度**，
            //    每个像素的反射方向已经不同 ⇒ 日盘自然被打碎成 glitter 光带。
            //    再把 σ_slope 当带宽加进去，等于「法线已知却按未知处理」⇒ 双计 + 抹平闪光。
            //    σ_slope 是给「法线贴图/法线只到低频」的方案用的（known_issue §11）。
            // 带宽 = 盘角半径 + 视线像素足迹 + **像素内法线方差**。
            // 最后一项不能省：远处一条波只占几个像素时，逐像素法线各不相同但 GGX lobe
            // 仍按 θ 级窄带算 ⇒ 高光变成稀疏白点（llvmpipe 实测就是「波峰交界的麻点」）。
            // 方差项 → α² 也要 ×2（α² = 2σ²，与 RoughFresnelAlbedo 同一口径）。
            float a2 = min(GgxDiskBandwidthA2(uSunAngularRadius)
                           + uSunFootprint * footprint_a2
                           + 2.0 * normal_var_px,
                           0.25);   // 0.25 = 数值兜底（见上）
            float D = GgxDistribution(NoH, a2);
            float G = GgxSmithG1(NoV, a2) * GgxSmithG1(NoL, a2);
            // 界面反射率：与探针支路同一个 F 口径（含角度平均修正）。
            float Fw = F;
            sun_spec = uSunIrradiance * (Fw * D * G / max(4.0 * NoV * NoL, 1.0e-6)) * NoL;
        }
    }

    vec3  C;      // 层色（P 点辐射度，未经相机段衰减）
    float alpha;  // 层遮挡率
    if (under) {
        // 深海积分反射：从 P 沿镜像方向到无穷远的介质积分（同一个 MediumAt，z→∞）。
        // ⚠️ P 往水面下推 1 mm：MediumAt 用 ro.y 判“从空气进水/本来就在水里”，
        //    落在平面上时是浮点不确定的（会影响折射后的 dy_w 与 B）。
        vec3 P = uCamPos + rd * t;
        P.y -= 1.0e-3;
        vec3 mirror = reflect(rd, N);
        // ⚠️ 波浪把法线倾斜到让镜像方向**朝上**时，半空间介质会立刻"出水" ⇒
        //    MediumAt 返回空 ⇒ S=0 ⇒ 环内出现**纯黑**（不是"深海色偏暗"，是积分没开始）。
        //    把镜像方向钳到水面以下（极小负 y），那片区域回到正常的水体自发光。
        //    连续、无分支：只在 mirror.y >= 0 的像素上生效。
        if (mirror.y > -1.0e-3) {
            float xz = length(mirror.xz);
            mirror = (xz > 1.0e-6)
                         ? vec3(mirror.x * (1.0 / max(xz, 1.0e-6)), -1.0e-3,
                                mirror.z * (1.0 / max(xz, 1.0e-6)))
                         : vec3(0.0, -1.0, 0.0);
            mirror = normalize(mirror);
        }
        vec3 tau_deep;
        vec3 S_deep;
        MediumAt(P, mirror, kDeepSeaRayT, tau_deep, S_deep);
        C = F * S_deep;
        alpha = F;          // 环外 Fresnel 已回到 1 ⇒ α = 1（整片替换）
    } else {
        // 水面以上：天光探针沿镜像方向（方向必须夹到地平线以上 —— 探针下半球是“地色”）。
        vec3 rdir = reflect(rd, N);
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
        // ── 探针 LOD（拉糊）：把「像素足迹内**未解析**的斜率方差」折成模糊角 ──
        // 两个来源相加（方差可加）：
        //   · var_p          谱侧：被足迹/淡出截掉的那部分波 + 短波 MSS。它们不在几何里，
        //                    但物理上存在 —— 反射必须按它们的斜率分布模糊，否则会走样；
        //   · normal_var_px  屏幕侧：相邻像素法线的实际跳变（残余/边缘走样）。
        // 镜面反射把斜率放大 2×（入射角=反射角）⇒ 需要的模糊角 ≈ 2σ；
        // 128² 探针的 texel 角 = (π/2)/128 · 2^lod。
        //
        // ⚠️ 上限随反射方向仰角放开：探针**下半球是地色**，地平线附近糊大了会把地色卷进来
        //    （整行发暗的"地平线裂隙"）。近地平线仍钳 1.5 级（≈2° 的 texel）；
        //    仰角 >~20° 时放到 3 级（探针 16²/面 ≈ 5.6°）。
        //    2026-11-23 报的「低风速海面硬边」就是 2~6 px 的细节波在 27° 仰角的反射里走样：
        //    旧上限 1.5 级糊不到 3° 以上，这条边就留着（known_issue §10.16 的"拉糊"）。
        const float kEnvTexelAngle = 1.5707963268 / 128.0;   // 常量表达式（GLSL const 限制）
        float refl_sigma = sqrt(max(var_p, 0.0) + max(normal_var_px, 0.0));
        float env_lod_need = log2(max(2.0 * refl_sigma, 1.0e-6) / kEnvTexelAngle);
        float env_lod_cap = mix(1.5, 3.0, smoothstep(0.05, 0.35, rdir.y));
        float env_lod = clamp(env_lod_need, 0.0, env_lod_cap);
        env_lod_debug = env_lod;
        vec3 refl = textureLod(uEnvCube, rdir, env_lod).rgb * uEnvScale;
        // 环境反射 + 太阳的像。
        // ⚠️ 太阳项是**加法**：探针不含日月盘（kEnvProbe），SSR 也不负责它 ——
        //    将来接 SSR 时写成 F*(A*SSR + (1-A)*sky) + sun_spec，
        //    千万别把 sun_spec 也乘上 A（那会让日盘像随 SSR 命中与否闪）。
        C = F * refl + sun_spec;
        refl_debug = refl;
        FresnelDebug = F;

        // 透射率 = (1−F)/n²（辐射度跨介质守恒）；WBO 的 α 是“背景衰减率”= 1 − T。
        alpha = 1.0 - (1.0 - F) / (uWaterIor * uWaterIor);
    }

    // ── debug：法线可视化（N*0.5+0.5）──
    //    用来判断「浪坑里的噪点」是法线场本身跳变，还是着色/求交。
    if (uWaveDebug == 7) {
        // alpha：0.02~0.06 = 窗内（F 很小），1.0 = 环外 TIR。看楔形是否落在 0/1 跳变带上。
        oAccum  = vec4(vec3(alpha), 1.0);
        oReveal = 1.0;   // 注意：reveal 槽写 src，混合后 dst=1−src；写 1 ⇒ 可见
        oDepth  = 1.0;
        return;
    }
    if (uWaveDebug == 8) {
        // 求交是否成功（1 = ok，0 = 退化/回退平面法线）
        oAccum  = vec4(wave_ok ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0), 1.0);
        oReveal = 1.0;
        oDepth  = 1.0;
        return;
    }
    if (uWaveDebug == 9) {
        // cos_i = |dot(rd,N)|：临界角 0.661 附近就是窗缘
        oAccum  = vec4(vec3(cos_i), 1.0);
        oReveal = 1.0;
        oDepth  = 1.0;
        return;
    }
    if (uWaveDebug == 6) {
        // 纯红不透明：只回答「这一像素有没有水面覆盖」。
        oAccum  = vec4(1.0, 0.0, 0.0, 1.0);
        oReveal = 1.0;
        oDepth  = 1.0;
        return;
    }
    if (uWaveDebug == 12) {
        // 探针反射色（未乘 F）：判断"硬边"是不是探针本身带的
        oAccum  = vec4(refl_debug, 1.0);
        oReveal = 1.0;
        oDepth  = 1.0;
        return;
    }
    if (uWaveDebug == 13) {
        // 反射率 F（探针支路同口径）
        oAccum  = vec4(vec3(FresnelDebug), 1.0);
        oReveal = 1.0;
        oDepth  = 1.0;
        return;
    }
    if (uWaveDebug == 11) {
        // 探针 LOD（调试）：0 = 原图，1 = LOD3。看"拉糊"到底有没有生效/生效在哪。
        float v = env_lod_debug / 3.0;
        oAccum  = vec4(v, v, v, 1.0);
        oReveal = 1.0;
        oDepth  = 1.0;
        return;
    }
    if (uWaveDebug == 2) {
        // 像素内法线变化量（×20 便于肉眼：0.05 rad/px → 1.0）
        float v = clamp(sqrt(normal_var_px) * 20.0, 0.0, 1.0);
        oAccum  = vec4(v, v, v, 1.0);
        oReveal = 1.0;
        oDepth  = 1.0;
        return;
    }
    if (uWaveDebug != 0) {
        oAccum  = vec4(N * 0.5 + 0.5, 1.0);
        oReveal = 1.0;
        oDepth  = 1.0;
        return;
    }

    // ── WBO 覆盖集 ──
    oAccum  = vec4(C, alpha);
    oReveal = alpha;
    // 代表深度 = 射线距离 × 遮挡率。
    // ⚠️ 波浪求交的 t 在掠射时会在相邻像素间跳（先撞哪个波峰由亚像素方向决定），
    //    所以合成趟**不用这个值做排序**：水体层在合成里被强制排到最前（见 fog_composite）。
    oDepth  = t * alpha;
}
)glsl";

}  // namespace water
}  // namespace jpov

#endif  // JPOV_SRC_WATER_WATER_SKIN_SHADER_H_
