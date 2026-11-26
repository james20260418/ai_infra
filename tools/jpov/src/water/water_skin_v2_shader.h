// JPOV 水皮 V2（单像素多法线采样）—— GLSL
//
// 设计：docs/jpov_water_skin_v2_design.md。**2026-11-26 验收通过，样貌固定**：
//
//     水皮 V2 = 一个公式 + 每像素 S 个法线采样 + 两个截断旋钮
//
// ┌─ 链路（从上到下就是一次着色）──────────────────────────────────────────────┐
// │ ① ResolveSurfaceV2   几何，**永不 discard**，唯一真状态                       │
// │     · 视线 → rd；**地平线钳位**（|rd.y| < 1 px 拉到水面一侧，天际线那一行不漏）│
// │     · 平面交点 t_plane（**只用于深度测试**，波浪无关）                       │
// │     · 波浪求交 t + 中心法线 N + 足迹 footprint（含谱截断）                    │
// │ ② RejectOccludedV2   唯一 discard 点                                         │
// │     · !hit / !wave_ok / 场景更近 / **钳位像素：这一像素有几何就 discard**      │
// │ ③ ShadeAboveV2 | ShadeBelowV2   S 个样本**各自着色**再平均，EmitWBO          │
// │     · 样本位置：中心交点 + 椭圆足迹内的 Hammersley 抖动（面积均匀、静态确定）  │
// │     · 法线：对样本 xz 调 WaveNormalAtXZ（解析梯度，与几何同一份谱）            │
// └──────────────────────────────────────────────────────────────────────────┘
//
// ┌─ 两个旋钮（WaterWaves）──────────────────────────────────────────────────┐
// │ v2_sample_count          S ∈ [1,16]，默认 4（实测 S≥4 已收敛）              │
// │ v2_truncate_px_main      主波截断乘数，默认 **0 = 不截断**                   │
// │ v2_truncate_px_detail    细节波截断乘数，默认 **1 = 走 V1 那条 3~8 px 线**    │
// │ 语义：`3T ~ 8T px`；T=1 与 V1 同一条线（乘 1.0 精确 ⇒ 可与 V1 逐字节对比）。   │
// │ 为什么分开：主波管几何/剪影，细节波只进法线 —— 实测**噪点元凶是细节波**。      │
// │ 为什么主波默认不截断：截掉主波会把远处的"骨架"拿掉（变平），而它并不产生噪点。  │
// └──────────────────────────────────────────────────────────────────────────┘
//
// ⭐ 与 V1（water_skin_shader.h）的关系：**独立的一整份 pass**（自己的 renderer /
//    program / WBO 附件），V1 一行不改、逐字节保留，靠 `WaterWaves::v2` 二选一。
//    差别只有一条：V1 用「1 个法线 + 若干二阶补偿量（屏幕导数 / 方差）」去近似面积分，
//    V2 直接采样。V1 的 9 个补偿项（var_p / var_p_mean / normal_var_px / footprint_a2 /
//    env_lod 的 normal_var 项 / env_lod_cap / a2 的 0.25 兜底 / far 分流 / lod 常量本身）
//    在这里**全部不存在**。
//
// ⭐ 结构性收益：V2 里**没有显式 dFdx/dFdy** ⇒ 历史上反复追的「导数必须先于 discard」
//    这条不变式整条消失。（唯一残留的隐式导数是 `texture(uSceneDepth, …)` 的 LOD 选择，
//    但那张深度图是 NEAREST 且无 mip ⇒ 取哪一级都一样；与 V1 同一份代码。）
//
// ⭐ MSS（Cox–Munk 短波）**不在波表里** ⇒ 永远不可采样，作为 σ 偏置保留，
//    参与反射率 F、太阳 lobe 宽度、探针 LOD 三处（见 docs §7）。
//
// ⭐ 三条必须写死的纪律（docs §6.1）：
//    ① 不能平均法向再着色一次：mean(Shade(N_i)) ≠ Shade(mean(N_i))；
//    ② 不能平均 env 再乘平均 F：mean(F_i·E_i) ≠ mean(E_i)·mean(F_i)；
//    ③ C 与 alpha 必须用同一批样本（反射/透射的能量账才守恒）。
//
// ⭐ 采样点位置的纪律（Danis 2026-11-26）：世界 xz 一律用「海面是平的」推演，
//    **绝不用中心像素的 N** 把偏移投影到切平面 —— 否则中心法线的噪声会变成采样位置、
//    再被采回来（正反馈，噪声收不住）。
//
#ifndef JPOV_SRC_WATER_WATER_SKIN_V2_SHADER_H_
#define JPOV_SRC_WATER_WATER_SKIN_V2_SHADER_H_

namespace jpov {
namespace water {

// ───────────────────────── 公共：uniform + 求交 + 采样 + 发射 ─────────────────────────
//
// 相机基约定与 V1 **完全一致**（与 Primitives3DRenderer::BuildMVP 的 lookAt 同源）：
//   rd = normalize(fwd + right·(ndc.x·tanHalfFov·aspect) + up'·(ndc.y·tanHalfFov))
//
// 波浪部分（WaveSampleAt / WaveWarpAt / WaveDetailGrad / WaveSurfaceHit 与那组 uWave*）
// 与介质部分（MediumAt）由共享片段提供：本 pass 只调用它们，不重复实现物理。
// ⚠️ 本片段**不含 `#version`**，使用方要在最前面拼 `#version 330 core\n`。
inline constexpr const char* kWaterSkinV2CommonGlsl = R"glsl(
in vec2 vNdc;

layout(location = 0) out vec4  oAccum;    // (C, α)：C 为预乘色（S 个样本的平均）
layout(location = 1) out float oReveal;   // α 本身
layout(location = 3) out float oDepth;    // t · α（**射线距离**，不是视轴深度）

uniform vec2  uViewport;     // 3D FBO 尺寸（把 gl_FragCoord 换成 uv 采场景深度）
uniform vec3  uCamPos;       // 相机世界位置（y 是绝对高度；xz 只用于平面求交与深度）
// 相机在同一帧的**波浪局部系**里的 xz（= cam.xz − origin，CPU 用 double 减）。
// 为什么单独一个量（而不是在着色器里减）：`cam + rd·t` 的结果在 1e5~1e6 m 量级时，
// float 的 ulp 已经是 0.008~0.06 m —— 细节波（λ=0.3 m，k≈21）的相位会因此震 ±0.2~1.3 rad。
// 只有在**求位置之前**就换成局部坐标，相位才真的是小数相乘。
uniform vec2  uCamPosXZLocal;
uniform vec3  uCamFwd;       // 视线方向（单位）
uniform vec3  uCamRight;     // 右（单位，已正交化）
uniform vec3  uCamUp;        // 上（单位，已正交化）
uniform float uTanHalfFov;   // tan(fov/2)
uniform float uAspect;       // 宽/高
uniform float uNear;
uniform float uFar;
uniform float uDepthBiasM;       // 水面与场景深度的比较容差（米）
uniform sampler2D   uSceneDepth; // MRT#1 场景深度（gl_FragCoord.z，1.0 = 背景/远平面）

// ── 太阳（解析 GGX，盘状光源）──
// 与介质那份 uWaterSun* 分开：介质可以关，水面依然有日盘像。
uniform int   uHasSun;            // 0 = 无太阳 ⇒ 这一项恒 0
uniform vec3  uSunToward;         // 指向太阳（气侧，单位）
uniform vec3  uSunIrradiance;     // color × intensity（辐照度语义）
uniform float uSunAngularRadius;  // 盘角半径（弧度，> 0）
// ⚠️ 没有 V1 的 uSunFootprint（像素足迹带宽）—— V2 不靠 dFdx 做 AA，
//    抗锯齿由 S 个采样本身完成。

// 每像素法线采样数 S ∈ [1, 16]（renderer 已 CHECK）。S = 1 ⇒ 只取中心法线。
uniform int uSampleCount;

const int   kMaxSkinSamples = 16;
const float kBackgroundDepth = 0.999999;

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

// 反射方向夹到地平线以上的最小 y（= sin 1°）。由 renderer 从 water::kHorizonClampSin 上传
// （**单一出处**：CPU 常量在 water_skin_math.h；V1 用的是同一个 uniform 名）。
// ⚠️ 这是**探针缺陷补偿**（探针下半球是地色），不是物理；探针补下半球后应删。
uniform float uHorizonClampSin;

// 把反射方向夹到地平线以上并精确归一化（先夹 y、再把 xz 缩放到 sqrt(1−y²)）。
vec3 ClampReflectionAboveHorizon(vec3 dir) {
    float xz = sqrt(dir.x * dir.x + dir.z * dir.z);
    if (dir.y >= uHorizonClampSin) {
        return normalize(dir);
    }
    if (xz < 1.0e-6) {
        return vec3(0.0, 1.0, 0.0);   // 正下方（反射不会出现，防御）
    }
    float scale = sqrt(1.0 - uHorizonClampSin * uHorizonClampSin) / xz;
    return vec3(dir.x * scale, uHorizonClampSin, dir.z * scale);
}

// ── 一次求交的全部中间量（V2 版：**只有几何**，没有方差/导数类派生量）──
struct WaterSurfaceV2 {
    bool  hit;            // 射线打中水面（t > 0）
    bool  wave_ok;        // 波浪求交成功（无波浪恒 true）
    float t;              // 中心交点射线参数（欧氏，波浪时 = 二分解）
    float t_plane;        // 平面交点（深度测试用，波浪无关）
    vec2  pos_xz;         // 中心交点 xz（**波浪局部系**，含高度置换）—— 采样围绕它铺开
    vec3  rd;             // 视线方向（单位）
    vec3  N;              // 中心法线（S=1 时就是它；也是调试可视化用的那个）
    float cos_i;          // 中心样本的 |dot(rd, N)|
    float footprint;      // 一个像素在水面上的世界尺寸（米，沿地面**径向**；见 SkinV2SampleOffset）
    float graze;          // max(|rd.y|, 护栏)：短半轴 = 0.5·footprint·graze
    bool  horizon_clamped; // |rd.y| 被钳到水面一侧（天际线那一行；见 ResolveSurfaceV2）
};

// 着色输出（生产只用 C/α；调试读中心样本的 F / 探针反射色 / 探针 LOD）。
struct WaterShadeV2 {
    vec3  C;               // 层色（P 点辐射度，**S 个样本的平均**，未乘相机段衰减）
    float alpha;           // 层遮挡率（S 个样本的平均）
    float fresnel_debug;   // 调试（13）：**第一个样本**（i=0；S=1 时就是中心）的 F
    float env_lod_debug;   // 调试（11）：探针 LOD（全像素常量，只由 MSS 决定）
    vec3  refl_debug;      // 调试（12）：第一个抖动样本的探针反射色（未乘 F）
    // ⚠️ 调试 9（cos_i）/ 默认（法线色）用的是**中心法线** s.N，与 12/13 不是同一个采样点；
    //    S>1 时 i=0 落在离中心 sqrt(0.5/S)·footprint 处。别看混。
};

// NDC → 视线方向（单位）。相机基约定与文件顶部一致。
vec3 SkinV2RayDir(vec2 ndc) {
    return normalize(uCamFwd
                     + uCamRight * (ndc.x * uTanHalfFov * uAspect)
                     + uCamUp    * (ndc.y * uTanHalfFov));
}

// 求交 + 中心法线 + 足迹。**永不 discard**（discard 一律在 RejectOccludedV2）。
// 与 V1 的 ResolveSurface 相比：少了 H / footprint_a2 / normal_var_px / var_p*（都退休了），
// 多了 pos_xz（采样原点，**局部系**）与 graze（椭圆短轴因子）。
void ResolveSurfaceV2(out WaterSurfaceV2 s) {
    s.rd = SkinV2RayDir(vNdc);

    // ── 地平线钳位（**照抄 V1 的老口径**，Danis 2026-11-26 第三次报障后要求）──
    // 水面与天空的「地平线以下」起点都在 rd.y = 0，卡在同一条线上 ⇒ 边界那一行会露出
    // 天空在地平线以下填的 ground_color（"地缝"/暗线，观感是水皮覆盖不足）。
    // 做法：|rd.y| < 1 px 的射线一律**拉到水面那一侧**，让这一层多盖一像素。
    //   · 水面上方：rd.y → −eps（交点在"无穷远"但仍在前方 ⇒ t > 0）
    //   · 水面下方：rd.y → +eps（看向水面，同样多盖一像素）
    // ⚠️ 只改**求交用的方向**，不改亮度/反射的数学 —— 这一行本来就是半覆盖的边界像素。
    float pixel_angle = 2.0 * uTanHalfFov / uViewport.y;   // 一个像素的视角（弧度）
    float horizon_eps = pixel_angle;                       // = 1 px（与 V1 的 0.0016@720p/60° 同量级）
    s.horizon_clamped = false;
    if (abs(s.rd.y) < horizon_eps) {
        float sign_y = (uCamPos.y < uWaterLevelY) ? 1.0 : -1.0;
        s.rd = normalize(vec3(s.rd.x, sign_y * horizon_eps, s.rd.z));
        s.horizon_clamped = true;
    }

    // 波浪场的求值一律在「相对 uWaveOriginXZ 的局部 xz」里做；y 保持世界系
    //（高度的基准是 uWaterLevelY，与 xz 无关）。见 uCamPosXZLocal 的注释。
    vec3 cam_wave = vec3(uCamPosXZLocal.x, uCamPos.y, uCamPosXZLocal.y);

    // ── 视线 ∩ 水面（无波浪 ⇒ 平面解析解；有波浪 ⇒ 二分求交，**不截断谱**）──
    float t_raw = (uWaterLevelY - uCamPos.y) / s.rd.y;
    s.hit = (t_raw > 0.0);
    s.t = max(t_raw, 1.0e-3);   // 不可见的像素用安全值，只为让后续运算有定义
    s.t_plane = s.t;

    s.footprint = WaveFootprint(uCamPos, s.rd, uWaterLevelY, pixel_angle);
    // 椭圆短轴因子：像素在水面上的足迹是**各向异性**的 ——
    //   径向（视线在水平面的投影方向）被 1/|rd.y| 拉长 ⇒ 整宽 = footprint（半轴 = 1/2）；
    //   横向方向本身就在水面内 ⇒ 整宽 = t·pixel_angle = footprint·|rd.y|。
    // 用圆盘会把横向细节一起糊掉（掠射时短轴比长轴小 1/|rd.y| 倍）。
    s.graze = max(abs(s.rd.y), kWaveGrazeFloorSin);

    s.pos_xz = cam_wave.xz + s.rd.xz * s.t;
    s.N = vec3(0.0, 1.0, 0.0);
    s.wave_ok = true;
    if (uWaveCount > 0) {
        vec3 pos_w = vec3(0.0);
        vec3 nrm_w = vec3(0.0, 1.0, 0.0);
        float t_wave = s.t;
        s.wave_ok = WaveSurfaceHit(cam_wave, s.rd, uWaterLevelY, s.footprint,
                                   t_wave, pos_w, nrm_w);
        s.t = s.wave_ok ? t_wave : s.t;
        if (s.wave_ok) {
            s.pos_xz = pos_w.xz;      // 真交点（含高度置换）：采样以它为原点
            s.N = nrm_w;
        }
    }
    s.cos_i = abs(dot(s.rd, s.N));
}

// 唯一 discard 点：命中/求交失败 + 场景深度拒绝。
// skip_depth = true ⇒ 跳过深度拒绝（调试 uWaveDebug==3 的 A/B 对照）。
void RejectOccludedV2(WaterSurfaceV2 s, bool skip_depth) {
    if (!s.hit) {
        discard;   // 交点在相机后方
    }
    if (!s.wave_ok) {
        discard;   // 求交失败 / 交点跑到相机后方
    }
    if (skip_depth) {
        return;
    }
    float scene_ndc = texture(uSceneDepth, gl_FragCoord.xy / uViewport).r;
    if (s.horizon_clamped) {
        // 钳位像素的交点在"无穷远" ⇒ **任何不透明几何都挡在它前面**。
        // ⚠️ 不能拿 view_z 去比（钳位后是数十 km，浮点残差会把整行剔掉 ⇒ 反而露出地色），
        //    也不能整条跳过（那会把天际线那 1~2 行的近处物体一起盖掉 ——
        //    2026-11-19 报的「box 上沿被水皮锯 3 行」就是这么来的）。
        //    判据只有一句：这一像素有没有几何。（V1 的最终口径，逐字照抄。）
        if (scene_ndc < kBackgroundDepth) {
            discard;
        }
        return;
    }
    // 用**平面**交点距离（不用波浪求交的 t）：后者在掠射时相邻像素能差几公里 ⇒ view_z 抖动。
    float view_z = s.t_plane * dot(s.rd, uCamFwd);
    if (scene_ndc < kBackgroundDepth) {
        float scene_z = LinearizeDepthNdc(scene_ndc, uNear, uFar);
        if (view_z > scene_z + uDepthBiasM) {
            discard;   // 不透明场景挡在水面前
        }
    }
}

// ── 采样：Hammersley 低差异序列（**静态**：不闪、确定性、可复现）──
//
// radical inverse（base 2）：把 i 的二进制位反过来当小数位。
float SkinV2RadicalInverse(uint bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}

// 第 i 个样本相对中心交点的世界偏移（xz，米）：**椭圆足迹内均匀**。
//   · 长**半**轴 = footprint/2：footprint 是**一个整像素**沿地面径向的世界宽度
//     （WaveFootprint = t·pixel_angle/|rd.y|，pixel_angle 是整像素角），所以像素覆盖
//     ±footprint/2。用整值当半径会让采样盘变成 2×2 像素（面积 4×）⇒ 白送一级 mip，
//     并把「噪声起点距离」系统性拉近 ~2 倍（那正是 V3 定分工线要用的表）。
//   · 短**半**轴 = footprint·graze/2：横向方向本身就在水面内，宽度 = t·pixel_angle
//     = footprint·|rd.y|（graze 是同一道除零护栏）。
//   · 圆盘映射 r = sqrt(u) 保证面积均匀（不是半径均匀）。
// S = 1 ⇒ 偏移恒 0（退化成「只取中心法线」，与 docs §5.1 一致）。
vec2 SkinV2SampleOffset(int i, int sample_count, vec3 rd, float footprint, float graze) {
    if (sample_count <= 1 || !(footprint > 0.0)) {
        return vec2(0.0);
    }
    float u = (float(i) + 0.5) / float(sample_count);
    float v = SkinV2RadicalInverse(uint(i));
    float r = sqrt(u);
    float a = 6.283185307179586 * v;
    vec2 disk = vec2(r * cos(a), r * sin(a));
    // 径向：视线在水平面的投影方向（正下看时退化 ⇒ 取 +x）。
    vec2 radial = rd.xz;
    float rlen = length(radial);
    radial = (rlen > 1.0e-6) ? radial / rlen : vec2(1.0, 0.0);
    vec2 lateral = vec2(-radial.y, radial.x);
    float long_half = 0.5 * footprint;
    float short_half = 0.5 * footprint * graze;
    return radial * (disk.x * long_half) + lateral * (disk.y * short_half);
}

// 本像素实际要跑的样本数（唯一口径）。
int SkinV2SampleCount() {
    return clamp(uSampleCount, 1, kMaxSkinSamples);
}

// ── 一个采样点的全部几何（两条图案的唯一汇合点）──
//
// ⭐ 纪律（2026-11-26 Danis 定，**不能破**）：
//   ① 采样点的世界 xz 一律用「海面是平的」推演（椭圆足迹以**中心交点**为锚点铺开）；
//   ② **绝不用中心像素的法线 N** 去把偏移"投影到切平面"上 —— 那会把中心法线自己的
//      噪声当成采样位置，再采回法线 ⇒ 正反馈，噪声彻底收不住。
// 法线仍然是对该 xz 的解析场求值（WaveNormalAtXZ）⇒ 波浪细节一个没丢，只是"位置"不跟着抖。
struct SkinV2SampleGeom {
    vec3  rd;          // 该样本的视线方向（当前只有一个方向；保留字段是为了将来接子像素）
    vec2  pos_xz;      // 波浪局部系 xz
    vec3  N;           // 该 xz 上的解析法线
    float footprint;   // 该样本的像素足迹（米）——谱截断用
};

SkinV2SampleGeom SkinV2SampleAt(WaterSurfaceV2 s, int i, int sample_count) {
    SkinV2SampleGeom g;
    g.rd = s.rd;
    g.footprint = s.footprint;
    g.pos_xz = s.pos_xz + SkinV2SampleOffset(i, sample_count, s.rd, s.footprint, s.graze);
    g.N = WaveNormalAtXZ(g.pos_xz, g.footprint);
    return g;
}

// MSS（Cox–Munk 短波）的各向同性斜率方差 σ²：**唯一**的 σ 来源（表内波全部采样）。// uShortWaveMss = along + cross（张量迹）⇒ 各向同性平均 = 0.5·迹。
float SkinV2MssSlopeVariance() {
    return 0.5 * max(uShortWaveMss, 0.0);
}

// WBO 发射（生产程序唯一出口）。att3 必须是**射线距离** t（见 water_skin_shader.h 顶部）。
void EmitWBO(vec3 C, float alpha, float t) {
    oAccum  = vec4(C, alpha);
    oReveal = alpha;
    oDepth  = t * alpha;
}
)glsl";

// ───────────────────────── 水上着色（S 个样本 → 平均）─────────────────────────
// 反射 + 太阳的像 + F + 透射率。需要 kGgxSpecularGlsl（太阳 GGX）与 uEnv*（探针）。
inline constexpr const char* kWaterSkinV2AboveGlsl = R"glsl(
uniform float uEnvScale;       // 天光反射消费量 [0,1]（= EnvSpecular::sky_scale）
uniform samplerCube uEnvCube;  // 天光探针（大气色，不含日月盘）

// 粗糙界面的**反射方向反照率**（角度平均菲涅尔）：与 V1 / water_optics.h 同源。
// ⚠️ 输入是**斜率方差 σ²**（不是 a2）：a2 = 2σ²（GGX 自身方差 = α²/2）。
float RoughFresnelAlbedo(float cos_i, float slope_var, float ior) {
    float a2 = 2.0 * max(slope_var, 0.0);
    float s = sqrt(a2);
    float shifted = FresnelDielectric(clamp(cos_i + 0.52 * pow(max(s, 1.0e-6), 0.72),
                                           1.0e-4, 0.999), 1.0, ior);
    float plain = FresnelDielectric(clamp(cos_i, 1.0e-4, 0.999), 1.0, ior);
    float w = smoothstep(0.05, 0.5, cos_i);
    return mix(shifted, plain, w);
}

// 单个样本的太阳的像（解析 GGX，盘状光源）。
//   N     该样本的法线
//   H     半向量（只依赖 rd 与太阳方向 ⇒ 每像素算一次，全样本共享）
//   F     该样本的反射率
//   sigma2 MSS 斜率方差（**唯一**的带宽来源；V2 没有 footprint/导数项）
// 带宽 a2 = 盘角² + 2σ²，**无上限**（V1 那个 min(…, 0.25) 是给会爆的 dFdx 兜底的，
// V2 里没有 dFdx ⇒ 兜底退休）。
vec3 SkinV2SunSpecular(vec3 N, vec3 rd, vec3 H, float F, float sigma2) {
    if (uHasSun == 0) {
        return vec3(0.0);
    }
    float NoV = dot(-rd, N);
    float NoL = dot(uSunToward, N);
    if (!(NoV > 0.0) || !(NoL > 0.0)) {
        return vec3(0.0);
    }
    float NoH = max(dot(H, N), 0.0);
    float a2 = GgxDiskBandwidthA2(uSunAngularRadius) + 2.0 * sigma2;
    float D = GgxDistribution(NoH, a2);
    float G = GgxSmithG1(NoV, a2) * GgxSmithG1(NoL, a2);
    return uSunIrradiance * (F * D * G / max(4.0 * NoV * NoL, 1.0e-6)) * NoL;
}

void ShadeAboveV2(WaterSurfaceV2 s, out WaterShadeV2 o) {
    float sigma2 = SkinV2MssSlopeVariance();
    // 探针 LOD：V2 里唯一的模糊来源就是 MSS 的斜率方差（表内波由采样自己平均）。
    // 镜面反射把斜率放大 2× ⇒ 需要的模糊角 ≈ 2σ；128² 探针的 texel 角 = (π/2)/128 · 2^lod。
    // ⚠️ σ 是**各向同性斜率标准差** = √sigma2（sigma2 = 0.5·迹，见 SkinV2MssSlopeVariance）：
    //    V1 的 `2√var_p` 在 MSS 各向同性时恰好等于 `2√sigma2`（var_p 是投影方差，迹/2），
    //    所以这条与 V1 同口径。**不要**写成 2√(uShortWaveMss)（那是 √2 倍，重复计迹）。
    // 上限 1.5（docs §6 的直接口径；V1 那个随仰角放开的 env_lod_cap 已退休）。
    const float kEnvTexelAngle = 1.5707963268 / 128.0;
    float env_lod = clamp(log2(max(2.0 * sqrt(sigma2), 1.0e-6) / kEnvTexelAngle), 0.0, 1.5);

    int S = SkinV2SampleCount();
    vec3 sum_c = vec3(0.0);
    float sum_a = 0.0;
    for (int i = 0; i < kMaxSkinSamples; ++i) {
        if (i >= S) {
            break;
        }
        SkinV2SampleGeom g = SkinV2SampleAt(s, i, S);
        float cos_i = abs(dot(g.rd, g.N));
        // 反射率：**逐样本** F（掠射时跨度极大，不能平均后再乘）。
        float F = RoughFresnelAlbedo(cos_i, sigma2, uWaterIor);
        vec3 refl = textureLod(uEnvCube, ClampReflectionAboveHorizon(reflect(g.rd, g.N)),
                              env_lod).rgb * uEnvScale;
        // 半向量随 rd 变（将来接子像素射线时每个样本一条）⇒ 逐样本算，别提到循环外。
        vec3 H = uSunToward - g.rd;
        float hlen = length(H);
        H = (hlen > 1.0e-6) ? H / hlen : vec3(0.0, 1.0, 0.0);
        // 预乘后累加：mean(F_i·E_i) ≠ mean(E_i)·mean(F_i)。
        sum_c += F * refl + SkinV2SunSpecular(g.N, g.rd, H, F, sigma2);
        sum_a += 1.0 - (1.0 - F) / (uWaterIor * uWaterIor);
        if (i == 0) {
            o.fresnel_debug = F;
            o.refl_debug = refl;
        }
    }
    o.C = sum_c / float(S);
    // 透射率对样本是线性的 ⇒ alpha 平均是**正确的**（1 − mean(T_i) = mean(α_i)）。
    o.alpha = sum_a / float(S);
    o.env_lod_debug = env_lod;
}
)glsl";

// ───────────────────────── 水下着色（S 个样本 → 平均）─────────────────────────
// 深海积分反射（MediumAt 到无穷远）+ F + α。需要 kWaterMediumGlsl（MediumAt）。
inline constexpr const char* kWaterSkinV2BelowGlsl = R"glsl(
// “无穷远”的射线参数（均质介质下 τ 早已饱和）。
const float kSkinV2DeepSeaRayT = 1.0e6;

void ShadeBelowV2(WaterSurfaceV2 s, out WaterShadeV2 o) {
    int S = SkinV2SampleCount();
    vec3 sum_c = vec3(0.0);
    float sum_a = 0.0;
    for (int i = 0; i < kMaxSkinSamples; ++i) {
        if (i >= S) {
            break;
        }
        SkinV2SampleGeom g = SkinV2SampleAt(s, i, S);
        float cos_i = abs(dot(g.rd, g.N));
        // 水→空气菲涅尔（含全内反射）：逐样本（环内/环外的边界正是逐样本变的）。
        float F = FresnelDielectric(cos_i, uWaterIor, 1.0);

        // 采样点在水面下 1 mm：介质是 **level_y 的水平半空间**（波浪不改介质，见 known_issue
        // §11.5），所以这里用 level_y 而不是该样本自己的置换高度 —— 与介质的几何口径一致。
        vec3 P = vec3(g.pos_xz.x, uWaterLevelY - 1.0e-3, g.pos_xz.y);
        vec3 mirror = reflect(g.rd, g.N);
        // 法线把镜像方向倾斜到水面之上时，半空间介质会立刻"出水" ⇒ S = 0 ⇒ 环内纯黑。
        // 钳到水面以下（极小负 y）：那片区域回到正常的水体自发光（与 V1 同一条）。
        if (mirror.y > -1.0e-3) {
            float xz = length(mirror.xz);
            mirror = (xz > 1.0e-6)
                         ? vec3(mirror.x * (1.0 / max(xz, 1.0e-6)), -1.0e-3,
                                mirror.z * (1.0 / max(xz, 1.0e-6)))
                         : vec3(0.0, -1.0, 0.0);
            mirror = normalize(mirror);
        }
        vec3 tau_deep;   // MediumAt 的 out 参数：深海反射只用 S，不需要 τ
        vec3 S_deep;
        MediumAt(P, mirror, kSkinV2DeepSeaRayT, tau_deep, S_deep);
        sum_c += F * S_deep;
        sum_a += F;      // 环外 Fresnel 已回到 1 ⇒ α = 1（整片替换）
    }
    o.C = sum_c / float(S);
    o.alpha = sum_a / float(S);
    o.refl_debug = vec3(0.0);   // 水下 V1 也不采集探针反射（调试模式不复用）
    o.fresnel_debug = 0.0;      // 水下 V1 也是 0（F 已折进 C/α）
    o.env_lod_debug = 0.0;
}
)glsl";

// ───────────────────────── 调试 ─────────────────────────
// 只有调试 program 拼这个片段。V2 **没有** normal_var_px / var_p / footprint_a2，
// 所以 V1 的 2（像素内法线方差）退化成默认（法线色）；11/12/13 用中心样本 + MSS 口径。
inline constexpr const char* kWaterSkinV2DebugGlsl = R"glsl(
uniform int uWaveDebug;

void EmitDebugV2(WaterSurfaceV2 s, WaterShadeV2 o, int mode) {
    if (mode == 7) {
        // α（S 个样本的平均）：0.02~0.06 = 窗内，1.0 = 环外 TIR。
        oAccum  = vec4(vec3(o.alpha), 1.0);
        oReveal = 1.0;
        oDepth  = 1.0;
        return;
    }
    if (mode == 8) {
        // 中心求交是否成功（1 = ok，0 = 退化/回退平面法线）
        oAccum  = vec4(s.wave_ok ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0), 1.0);
        oReveal = 1.0;
        oDepth  = 1.0;
        return;
    }
    if (mode == 9) {
        // 中心样本的 cos_i（临界角 0.661 附近就是窗缘）
        oAccum  = vec4(vec3(s.cos_i), 1.0);
        oReveal = 1.0;
        oDepth  = 1.0;
        return;
    }
    if (mode == 6) {
        // 纯红不透明：只回答「这一像素有没有水面覆盖」
        oAccum  = vec4(1.0, 0.0, 0.0, 1.0);
        oReveal = 1.0;
        oDepth  = 1.0;
        return;
    }
    if (mode == 12) {
        // 中心样本的探针反射色（未乘 F）
        oAccum  = vec4(o.refl_debug, 1.0);
        oReveal = 1.0;
        oDepth  = 1.0;
        return;
    }
    if (mode == 13) {
        // 中心样本的反射率 F
        oAccum  = vec4(vec3(o.fresnel_debug), 1.0);
        oReveal = 1.0;
        oDepth  = 1.0;
        return;
    }
    if (mode == 11) {
        // 探针 LOD（V2 只由 MSS 决定 ⇒ 全像素常量）：0 = 原图，1 = LOD1.5
        float v = o.env_lod_debug / 1.5;
        oAccum  = vec4(v, v, v, 1.0);
        oReveal = 1.0;
        oDepth  = 1.0;
        return;
    }
    // 默认（含 2/3/4/5/10…）：中心法线可视化（N*0.5+0.5）
    oAccum  = vec4(s.N * 0.5 + 0.5, 1.0);
    oReveal = 1.0;
    oDepth  = 1.0;
}
)glsl";

// ───────────────────────── 4 个 main ─────────────────────────
inline constexpr const char* kWaterSkinV2AboveMainGlsl = R"glsl(
void main() {
    WaterSurfaceV2 s;
    ResolveSurfaceV2(s);
    RejectOccludedV2(s, false);
    WaterShadeV2 o;
    ShadeAboveV2(s, o);
    EmitWBO(o.C, o.alpha, s.t);
}
)glsl";

inline constexpr const char* kWaterSkinV2BelowMainGlsl = R"glsl(
void main() {
    WaterSurfaceV2 s;
    ResolveSurfaceV2(s);
    RejectOccludedV2(s, false);
    WaterShadeV2 o;
    ShadeBelowV2(s, o);
    EmitWBO(o.C, o.alpha, s.t);
}
)glsl";

inline constexpr const char* kWaterSkinV2AboveDebugMainGlsl = R"glsl(
void main() {
    WaterSurfaceV2 s;
    ResolveSurfaceV2(s);
    RejectOccludedV2(s, uWaveDebug == 3);
    WaterShadeV2 o;
    ShadeAboveV2(s, o);
    EmitDebugV2(s, o, uWaveDebug);
}
)glsl";

inline constexpr const char* kWaterSkinV2BelowDebugMainGlsl = R"glsl(
void main() {
    WaterSurfaceV2 s;
    ResolveSurfaceV2(s);
    RejectOccludedV2(s, uWaveDebug == 3);
    WaterShadeV2 o;
    ShadeBelowV2(s, o);
    EmitDebugV2(s, o, uWaveDebug);
}
)glsl";

}  // namespace water
}  // namespace jpov

#endif  // JPOV_SRC_WATER_WATER_SKIN_V2_SHADER_H_
