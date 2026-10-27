// JPOV 水皮（water skin）pass —— GLSL
//
// 一块**水平正方形的 quad**（边长由 uniform 缩放，默认 10 km²）+ 解析式菲涅耳。
// 位置：froxel（点状雾）合成**之后**、高亮 / tone map 之前（见 Renderer::Render）。
//
// 透射（本版刻意最简单）：**固定管线 alpha 混合**
//     out = water_color·α + dst·(1 − α)
// dst 就是帧缓冲里已经画好的不透明场景颜色 —— 所以本 pass 不采样场景颜色、
// 也不需要自己的 FBO（与 horizon_fog / froxel 同一套"就地合成"的写法）。
//
// 两种观察方向（用 uCamPos.y 与 uLevelY 自己判，不靠第二个开关）：
//   ── 相机在水上 ──
//     α    = mix(body_alpha, 1, F)     // 正对视线看得穿，掠射处收敛成亮带
//     color= mix(body_color, rim_color, F)
//     F    = f0 + (1 − f0)·(1 − cosθ)^power        （Schlick，θ 与向上法线的夹角）
//   ── 相机在水下（抬头看水面）──
//     界面反射率 R 仍是**精确未极化 Fresnel（水→空气）**，临界角 θc = asin(1/ior) ≈ 48.6°：
//       θ ≥ θc ⇒ R = 1（全内反射）⇒ 整片是"地色"（水面变成一面暗镜）
//       θ <  θc ⇒ 0.02 ≤ R < 1，窗内**原样透出背景**（真实场景：天空 + 水上物体）
//     合成：out = tirColor·R + 背景·(1−R)　（固定管线 alpha 混合，不需要抓帧/探针）
//
// ⚠️ **不做折射**（Danis 2026-10-27 定：按"简单游戏引擎"的做法）。
//    之前用 refract() 把整个 0..90° 天空压进 0..48.6° 锥（物理正确，但观感"很怪"，
//    而且任何在此基础上采"水上几何"的屏幕空间做法都必然在锥缘拉条）。现在改为：
//    天空按**视线方向 + 表面斜率**采样（法线扰动的查找），只保留 Fresnel/TIR 这个界面权重。
//    代价（已知）：窗内的天不会被压扁、地平线不会贴在锥缘 —— 换来的是干净、可调、可控。
//    n 滑条拉到 1.0 时界面在物理上消失（R≡0）⇒ 整片原样透出背景（含水上人物）。

// ⭐ 波浪 = **只扰法线，不动几何**（Danis：先看波纹，几何起伏以后再说）。
//    法线扰动同时喂三个消费者：① Fresnel/TIR 的权重（窗缘随波纹起伏）、
//    ② 水下窗内天空的查找方向（天光随波纹晃动）、③ 水上视角的玻璃边反。
//     水面不平（surface_slope_rms_deg > 0）时对**精确 R**做 5 抽头斜率分布平均（不是把 R
//     往 1 推 —— 那会把过渡带整体往窗内偏，窗内出现一片灰）。
//
// 本版**不做**（写在前面，免得当成 bug）：
//   * 水下没有吸收/散射（这是"绝对干净的水"）；
//   * 窗内不画**水上几何**的折射像（只采天空探针）⇒ 水上物体在窗内不出现。
//     试过屏幕空间折射 march，结论是**不能用**：平面折射把上半球压进 97.2° 锥，
//     锥缘处的屏幕压缩趋于无穷 ⇒ 采样必然在物体剪影上横向拖出长条（"黏住"）。
//     要做对只能"重渲上半场景"（相机位置的环境图 / 顶点级折射重渲），不是屏幕空间的事。
//   * 没有太阳的**折射像**（探针按设计不含日月盘）；
//   * 全内反射区是常量地色，不是"水下环境的真实镜像"（那需要平面反射）。
//
// 深度：HDR 目标**没有深度附件**（场景深度由 MRT#1 逐片元写入），所以这里手动比：
//   场景深度 z 与水面自身 z 都用相机 near/far 线性化（与 strip pass 的 ZView 同一份公式），
//   水面比不透明场景**远**超过 2 cm 就丢弃 —— 水皮不能盖到挡在它前面的东西上。
//
// 已知边界（本版有意不做）：
//   * 没有吸收/散射（水下物体只是被一层淡色膜按 α 混掉，不随深度变蓝）；
//   * 没有反射探针（rim_color 是常量，不是天空 cubemap）；
//   * 波浪**只扰法线、不位移几何**（Danis 定：先看波纹，几何起伏以后再说）。

#ifndef JPOV_SRC_WATER_WATER_SKIN_SHADER_H_
#define JPOV_SRC_WATER_WATER_SKIN_SHADER_H_

namespace jpov {

// quad 的局部坐标在 ±1（x/z），由 uHalfExtent / uLevelY 展开成世界空间的正方形。
inline constexpr const char* kWaterSkinVs = R"glsl(
#version 330 core
layout(location = 0) in vec3 aPos;   // x,z ∈ {-1,+1}，y = 0
uniform mat4  uVP;
uniform float uHalfExtent;           // 正方形半边长（米）
uniform float uLevelY;               // 水面世界高度（米）
out vec3 vWorld;
void main() {
    vec3 p = vec3(aPos.x * uHalfExtent, uLevelY, aPos.z * uHalfExtent);
    vWorld = p;
    gl_Position = uVP * vec4(p, 1.0);
}
)glsl";

inline constexpr const char* kWaterSkinFs = R"glsl(
#version 330 core
in vec3 vWorld;
out vec4 FragColor;

uniform sampler2D uSceneDepth;   // MRT#1 场景深度（gl_FragCoord.z，1.0 = 背景/远平面）
uniform vec2  uViewport;         // 3D FBO 尺寸（把 gl_FragCoord 换成 [0,1] uv）
uniform float uNear;
uniform float uFar;
uniform vec3  uCamPos;           // 相机世界位置
uniform float uLevelY;           // 水面世界高度（米）—— 判相机在水上/水下用
uniform vec3  uBodyColor;        // 本体色（线性 HDR）
uniform float uBodyAlpha;        // 正对视线时的覆盖率 [0,1]
uniform vec3  uRimColor;         // 掠射处的反射色（线性 HDR）
uniform float uF0;               // 正入射反射率（水 ≈ 0.02）
uniform float uPower;            // Schlick 指数（物理值 5）
uniform vec3  uTirColor;         // 水下视角：临界角外的全内反射色（“地色”，建议 = SkyCommand::ground_color）
uniform float uIor;              // 水的折射率（水→空气临界角 = asin(1/ior)）；=1 时界面消失
uniform float uMinTransmission;  // 强制透射下限 [0,1]：R 被夹到 ≤ 1−它（实验用，非物理）
// ── 波浪（法线扰动）──
uniform float uWavePhase;        // 相位（弧度；调用方自己推进，"调相位"就是挪它）
uniform float uWaveSlopeRad;     // 最大斜率（弧度）—— 扰动的强度；0 = 完全平静
uniform float uWaveLengthM;      // 主波长（米）
uniform float uWaveDirRad;       // 传播方向（世界 XZ 平面，弧度）
// 透射侧 = **帧缓冲里已经画好的东西**（也就是这个 pass 正在混合的背景）：
// 固定管线 alpha 混合天然给出 out = tirColor·R + 背景·(1−R)，不需要抓帧、不需要探针。
// ⭐ 这一点决定了"水上物体能不能透过水面看到"：透射的是**真实场景**（人、桌子、天空），
//    不是"只含天空的探针" —— 后者在 n→1 时也看不到人（2026-10-27 Danis 抓到）。
float FresnelFromWater(float cos_i, float eta) {
    float sin2t = eta * eta * (1.0 - cos_i * cos_i);
    if (sin2t >= 1.0) {
        return 1.0;
    }
    float ct = sqrt(1.0 - sin2t);
    float rs = (eta * cos_i - ct) / (eta * cos_i + ct);
    float rp = (eta * ct - cos_i) / (eta * ct + cos_i);
    return 0.5 * (rs * rs + rp * rp);
}

// 窗口深度 → 视空间距离（与 strip pass 的 ZView 同一份公式）。
float ZView(float d) {
    float ndc = 2.0 * d - 1.0;
    float denom = (uFar + uNear) - ndc * (uFar - uNear);
    // 退化相机守卫（near ≈ 0 / near ≈ far）：0/0 会变 NaN 并污染整帧。
    if (!(abs(denom) > 1e-6)) {
        return max(uNear, 1e-4);
    }
    return (2.0 * uNear * uFar) / denom;
}

// 波浪法线（解析，**不位移几何**）：
//   h(p) = Σ Aᵢ·sin(kᵢ·dᵢ·p + wᵢ·φ)，法线 = normalize(-∇h, 1)。
//   5 组方向/波长互不成谐波的正弦叠加（3 组会看出规则"点阵"）；合成后再**限幅**到
//   uWaveSlopeRad（保证"波幅滑条"的物理含义是"最大斜率"，不是"随便一个乘数"）。
//
// 远处必须淡出：10 km² 的水面在 1 km 处一个像素覆盖米级面积，解析波就是一片闪烁噪点。
// 判据用像素足迹（fwidth）⇒ 远景收敛回镜面，正是真实远海的观感。
vec3 WaveNormal(vec3 p) {
    if (uWaveSlopeRad <= 1.0e-5) {
        return vec3(0.0, 1.0, 0.0);
    }
    // 抗锯齿：波浪是逐像素解析算的、没有 mip ⇒ 频率超过像素 Nyquist 就变闪烁/摩尔纹。
    // 判据用**该像素在水面上的足迹**（fwidth(world.xz)）而不是"到相机的距离"：
    // 掠射角下足迹长得极快，距离判据会漏（实测水面远处出现同心圆摩尔纹）。
    // 足迹 > λ/6 开始淡出、> λ/3 完全平滑（远海本来就该收敛成镜面）。
    float lam = max(uWaveLengthM, 0.05);
    float footprint = length(fwidth(p.xz));
    float fade = 1.0 - smoothstep(0.17 * lam, 0.33 * lam, footprint);
    if (fade <= 0.0) {
        return vec3(0.0, 1.0, 0.0);
    }

    // 5 组方向/波长互不成谐波的正弦（+ 深水色散 ω ∝ 1/√λ）：
    //   少了会出现规则网格/摩尔纹（实测 3 组就是一张"点阵"），多了没必要。
    //   权重之和 = 1 ⇒ 合成斜率的上界仍是 uWaveSlopeRad（"波幅"滑条含义稳定）。
    const int   kCount = 5;
    // 方向/波长都取**不成比例**的值（不用 0/45/90 那种整齐角度）：整齐的组合会在水面上
    // 拍出"华夫饼"式的规则点阵，远看变成摩尔纹（实测 3 组整齐方向就是这个观感）。
    const float kAng[kCount]     = float[kCount](0.00, 1.13, 2.41, 3.63, 5.02);
    const float kLamMul[kCount]  = float[kCount](1.00, 0.63, 1.57, 0.37, 2.31);
    const float kWeight[kCount]  = float[kCount](0.30, 0.25, 0.19, 0.15, 0.11);

    vec2 xz = p.xz;
    vec2 grad = vec2(0.0);
    for (int i = 0; i < kCount; ++i) {
        float a = uWaveDirRad + kAng[i];
        vec2 d = vec2(cos(a), sin(a));
        float lam = max(uWaveLengthM * kLamMul[i], 0.02);
        float k = 6.28318530718 / lam;
        // 色散：短波跑得快（ω ∝ 1/√λ），相位推进因此各不同 —— 波纹不会"整片平移"。
        float w = inversesqrt(kLamMul[i]);
        grad += d * (kWeight[i] * cos(k * dot(d, xz) + w * uWavePhase));
    }

    float slope = length(grad);
    if (slope > 1.0e-6) {
        grad *= min(uWaveSlopeRad, slope * fade) / slope;   // 限幅 + 足迹淡出
    } else {
        grad = vec2(0.0);
    }
    return normalize(vec3(-grad.x, 1.0, -grad.y));
}

void main() {
    // 手动深度测试：水面在这块不透明几何后面就整片丢弃（否则水会盖到挡在它前面的东西上）。
    float scene_z = ZView(texture(uSceneDepth, gl_FragCoord.xy / uViewport).r);
    float z = ZView(gl_FragCoord.z);
    if (z > scene_z + 0.02) {
        discard;
    }

    vec3 N = WaveNormal(vWorld);                       // 波浪扰过的法线（几何仍是平面）
    vec3 V = normalize(uCamPos - vWorld);
    float cos_i = clamp(abs(dot(N, V)), 1.0e-4, 1.0); // 与法线夹角（双面）

    vec3 color;
    float alpha;
    if (uCamPos.y < uLevelY) {
        // ── 水下抬头看水面：界面权重 R（精确 Fresnel + TIR，法线被波浪扰动）
        //    out = 地色·R + **背景**·(1−R)
        //    R→0（正上方 / n→1）⇒ 原样透出背景：天空**和水上的物体**都在；
        //    R→1（掠过临界角 / 窗外）⇒ 整片地色（本版不做镜像）。
        float R = FresnelFromWater(cos_i, uIor);
        // 实验旋钮：强制留一部分透射（TIR 区也不再是"全反射"）。物理上不存在。
        R = min(R, 1.0 - clamp(uMinTransmission, 0.0, 1.0));
        color = uTirColor;
        alpha = clamp(R, 0.0, 1.0);
    } else {
        // ── 水上俯视/平视 ──
        float F = uF0 + (1.0 - uF0) * pow(1.0 - cos_i, uPower);
        color = mix(uBodyColor, uRimColor, F);
        alpha = clamp(mix(uBodyAlpha, 1.0, F), 0.0, 1.0);
    }
    FragColor = vec4(color, alpha);
}
)glsl";

}  // namespace jpov

#endif  // JPOV_SRC_WATER_WATER_SKIN_SHADER_H_
