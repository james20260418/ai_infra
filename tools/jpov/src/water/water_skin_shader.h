// JPOV 水皮（water skin）pass —— GLSL
//
// **无限大的水平平面**（y = uLevelY）+ 解析式菲涅耳。
// 位置：froxel（点状雾）合成**之后**、高亮 / tone map 之前（见 Renderer::Render）。
//
// ⭐ 为什么是"无限平面"而不是一块有限 quad（2026-10-28 Danis 提的问题）：
//   有限 quad 不管多大，屏幕上都会在**它的远端边界**与**真地平线**之间留出一条缝 ——
//   那条缝里画的是天球下半球的 ground 色（看得到"一片陆地"），而物理上那里就是海。
//   而"覆盖到真地平线"用几何是做不到的：视线越接近水平，交点距离 → ∞
//   （眼高 1.5 m 时地平线在 4.4 km；眼高 1.5 km 时在 138 km）。
//   ⇒ 正解是**不打几何**：全屏三角形 + 在 FS 里解"视线 ∩ 平面"，
//     每个朝下的方向都有交点，天然覆盖到地平线（只受浮点精度限制）。
//
// 透射（本版刻意最简单）：**固定管线 alpha 混合**
//     out = water_color·α + dst·(1 − α)
// dst 就是帧缓冲里已经画好的不透明场景颜色 —— 所以本 pass 不采样场景颜色、
// 也不需要自己的 FBO（与 horizon_fog / froxel 同一套"就地合成"的写法）。
//
// 两种观察方向（用 uCamPos.y 与 uLevelY 自己判，不靠第二个开关）：
//   ── 相机在水上：镜面天光 + 水下透射 + 太阳高光（三项默认开）──
//     F = Schlick（f0 = ((n−1)/(n+1))²，与水下**同一个 n**）
//     out = bg·(1−B)(1−F) + body·B·(1−F) + skyReflect·F + sunSpec
//     用一次 alpha 混合表达：α = 1−(1−B)(1−F)，color = (body·B·(1−F) + sky·F + sun)/α
//     · 镜面天光 = 天空探针按 reflect(−V, N) 采样（N 被波纹扰动 ⇒ 反光碎成亮片）
//     · 水下透射 = 薄膜 + 背景本身（关掉 ⇒ α=1，水面变成不透明的本体色）
//     · 太阳高光 = Blinn-Phong 单瓣 × F（近处暗、远处掠射亮 = 真实的"太阳路"）
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
//   命中点到相机的**视轴距离**（= t·(dir·fwd)）与 MRT#1 场景深度都用相机 near/far 线性化
//   （与 strip pass 的 ZView 同一份公式），水面比不透明场景**远**超过 2 cm 就丢弃
//   —— 水皮不能盖到挡在它前面的东西上。
//   ✱ 唯一例外：命中点在**远裁剪面之外**（t → ∞，即贴近地平线那一带）时不做深度比较。
//     那里只有天空（天空是最远的背景），水必须盖住它的 ground 色；判深度会把那条
//     地色带重新放出来（这正是 2026-10-28 修的那个问题）。
//
// ⭐ 太阳高光 = **日盘的解析像**（不采纹理）：
//   光源是主平行光（颜色 × 强度 = 太阳辐亮度），高光就是"日盘在表面斜率分布下的镜面
//   反射"，用能量守恒的 Blinn-Phong 单瓣表达。天空探针（SkyOutput::kEnvProbe）**故意
//   不含日月盘** —— 否则与这里的高光双计。所以"日盘高光"在画面上的表现是**太阳路**
//   （被波浪打碎的亮带），而不是探针里的一张小圆盘；这也是 UE/Unity 水面的做法
//   （探针只给环境色，直接光的高光单独算）。
//   已知边界：波瓣宽度由 n 与斜率方差决定，**不**再叠日盘角半径（太阳 0.27°，
//   对 n=1500 的 2° 量级波瓣是零头）。
//
// 已知边界（本版有意不做）：
//   * 没有吸收/散射（水下物体只是被一层淡色膜按 α 混掉，不随深度变蓝）；
//   * 镜面天光优先用天空 cubemap 探针；探针不存在时才退回常量 rim_color；
//   * 波浪**只扰法线、不位移几何**（Danis 定：先看波纹，几何起伏以后再说）。

#ifndef JPOV_SRC_WATER_WATER_SKIN_SHADER_H_
#define JPOV_SRC_WATER_WATER_SKIN_SHADER_H_

namespace jpov {

// 全屏三角形（顶点坐标由 gl_VertexID 生成，不需要顶点缓冲）。
// vNdc = 该顶点在 NDC 的位置（−1..1），光栅化线性插值 ⇒ FS 拿到逐像素 NDC。
inline constexpr const char* kWaterSkinVs = R"glsl(
#version 330 core
out vec2 vNdc;
void main() {
    // 三个顶点盖住整个裁剪空间：(-1,-1) (3,-1) (-1,3)。
    vec2 p = vec2((gl_VertexID == 1) ? 3.0 : -1.0,
                  (gl_VertexID == 2) ? 3.0 : -1.0);
    vNdc = p;
    gl_Position = vec4(p, 0.0, 1.0);
}
)glsl";

inline constexpr const char* kWaterSkinFs = R"glsl(
#version 330 core
in vec2 vNdc;
out vec4 FragColor;

uniform sampler2D uSceneDepth;   // MRT#1 场景深度（gl_FragCoord.z，1.0 = 背景/远平面）
uniform vec2  uViewport;         // 3D FBO 尺寸（把 gl_FragCoord 换成 [0,1] uv）
uniform float uNear;
uniform float uFar;
uniform vec3  uCamPos;           // 相机世界位置
// 相机基（把逐像素 NDC 换成世界射线）：不传逆 VP，省一次 4×4 求逆，也更好读。
uniform vec3  uCamFwd;           // 视线方向（单位）
uniform vec3  uCamRight;         // 右（单位，已正交化）
uniform vec3  uCamUp;            // 上（单位，已正交化）
uniform float uTanHalfFov;       // tan(fov/2)（垂直视野，度→弧度后取半）
uniform float uAspect;           // 宽/高
uniform float uLevelY;           // 水面世界高度（米）—— 判相机在水上/水下用
uniform vec3  uBodyColor;        // 本体色（线性 HDR）
uniform float uBodyAlpha;        // 正对视线时的覆盖率 [0,1]
uniform vec3  uRimColor;         // 掠射处的反射色（线性 HDR）
uniform vec3  uTirColor;         // 水下视角：临界角外的全内反射色（“地色”，建议 = SkyCommand::ground_color）
uniform float uIor;              // 水的折射率（水→空气临界角 = asin(1/ior)）；=1 时界面消失
uniform float uMinTransmission;  // 强制透射下限 [0,1]：R 被夹到 ≤ 1−它（实验用，非物理）
// ── 水上视角的三项（默认全开）──
uniform samplerCube uEnvCube;    // 天空探针（大气色；镜面天光用）
uniform int   uHasEnvCube;       // 0 = 没有探针 ⇒ 镜面天光退回常量 rim 色
uniform float uSkyScale;         // 天光反射量 [0,1]
uniform int   uUseSkyRefl;       // 1 = 镜面天光
uniform int   uUseTransmit;      // 1 = 水下透射（关掉 = 水面变成不透明的本体色）
uniform int   uUseSun;           // 1 = 太阳高光
uniform vec3  uSunDir;           // 指向太阳的世界方向（单位向量）
uniform vec3  uSunColor;         // 太阳直射色（线性 HDR，已乘 intensity）
uniform float uSunShininess;     // Blinn-Phong 幂次（越大越尖）
// LEAN 高光补偿：把**带限丢掉的斜率方差**并进太阳高光波瓣（波瓣变宽、峰值下降、
// 能量不凭空消失）。0 = 关（远处高光会"变软消失"）；1 = 物理量；>1 = 观感加强。
uniform float uLeanGain;
// ── 波浪（法线扰动）──
uniform float uWavePhase;        // 相位（弧度；调用方自己推进，"调相位"就是挪它）
uniform float uWaveSlopeRad;     // 最大斜率（弧度）—— 扰动的强度；0 = 完全平静
uniform float uWaveLengthM;      // 主波长（米）
uniform float uWaveDirRad;       // 传播方向（世界 XZ 平面，弧度）
// 远距离带限方式（对比/教学用；见 WaveNormal 的注释）：
//   0 = 不过滤（aliasing 基线）  1 = 硬关八度 + 归一化  2 = 逐八度 sinc 带限（IQ）
//   3 = 2 + 丢失的斜率方差 → 反射模糊 + 太阳高光增宽（LEAN 思路）
uniform int   uWaveFilterMode;   // 见 WaveNormal 末尾的 0..4 说明
// 模式 4：波浪法线 flipbook 图集（GL_TEXTURE_2D_ARRAY，每层一帧、各自带 mip 链）
uniform sampler2DArray uWaveTile;
uniform int   uHasWaveTile;      // 0 = 图集没建好（退回解析路径）
uniform float uWaveTileM;        // 一张 tile 覆盖多少米
uniform int   uWaveTileFrames;   // flipbook 帧数
// 透射侧 = **帧缓冲里已经画好的东西**（也就是这个 pass 正在混合的背景）：
// 固定管线 alpha 混合天然给出 out = tirColor·R + 背景·(1−R)，不需要抓帧、不需要探针。
// ⭐ 这一点决定了"水上物体能不能透过水面看到"：透射的是**真实场景**（人、桌子、天空），
//    不是"只含天空的探针" —— 后者在 n→1 时也看不到人（2026-10-27 Danis 抓到）。
// 空气→水 的界面反射率（Schlick）：f0 由**同一个 n**推出来，所以 n 滑条同时管水上/水下。
float FresnelAirToWater(float cos_i, float n) {
    float f0 = (n - 1.0) / (n + 1.0);
    f0 = f0 * f0;
    return f0 + (1.0 - f0) * pow(1.0 - clamp(cos_i, 0.0, 1.0), 5.0);
}

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
//   h(p) = Σ Aᵢ·sin(kᵢ·dᵢ·p + ωᵢφ)，法线 = normalize(−∇h, 1)。
//
// ⭐ **逐八度抗锯齿**（这是"远处为什么不该变平"的关键）：
//   程序化波浪没有 mip，像素足迹一旦超过某个波长的 Nyquist，那个波长就只能变成闪烁噪点。
//   但**不能整片一起淡出**（第一版就是这么写的：按最短波长算一个全局 fade ⇒ 50~60 m 外
//   全平了，看起来像"只有近处有波纹"）。正确做法是**每个八度按自己的波长单独判**：
//     fadeᵢ = 1 − smoothstep(λᵢ/6, λᵢ/3, footprint)
//   于是远处先丢掉细波、保留大浪 —— 和法线贴图 mip 一个道理，只是发生在程序化域。
//   丢掉一部分八度后按**存活权重之和**归一化，所以"波幅滑条 = 最大斜率"在任何距离都成立。
vec3 WaveNormal(vec3 p, out float slope_var) {
    slope_var = 0.0;
    if (uWaveSlopeRad <= 1.0e-5) {
        return vec3(0.0, 1.0, 0.0);
    }
    // ── 模式 4：直接采**法线 flipbook 纹理**（硬件 mip + 各向异性）──
    //   这就是"法线也是纹理"那条路：烘焙时保证空间可平铺 + 时间整数圈，运行时只是
    //   一个 texture() 调用，带限/各向异性全交给 GPU。不需要 FFT。
    //   与解析路径的区别：mip 平均的是**斜率**（线性可平均）⇒ 远处斜率趋于 0（变平）。
    //   LEAN 的补法：图集在 B 通道多存一个**二阶矩** E[|s|²]（线性可平均），于是
    //   任意 mip 级别上都能现算"被平均掉的方差" = E[|s|²] − |E[s]|²，喂给高光/探针。
    //   （这就是 LEAN / LEADR 存一阶+二阶矩的出发点；只存斜率就必须靠"mip 级别"猜方差。）
    if (uWaveFilterMode == 4 && uHasWaveTile == 1 && uWaveTileFrames > 0) {
        float frame = uWavePhase / 6.28318530718 * float(uWaveTileFrames);
        frame = mod(floor(frame), float(uWaveTileFrames));
        vec2 uv = p.xz / max(uWaveTileM, 0.05);
        vec4 s = texture(uWaveTile, vec3(uv, frame));
        vec2 g = s.rg * 2.0 - 1.0;            // 归一化斜率（烘焙时按 max_slope 归一）
        vec2 grad = g * uWaveSlopeRad;
        // 方差是**归一化域**的量，最后乘回斜率尺度²（= 弧度²）。
        slope_var = max(s.b - dot(g, g), 0.0) * uWaveSlopeRad * uWaveSlopeRad;
        float slope = length(grad);
        if (slope > 1.0e-6) {
            grad *= min(uWaveSlopeRad, slope) / slope;
        } else {
            grad = vec2(0.0);
        }
        return normalize(vec3(-grad.x, 1.0, -grad.y));
    }

    // 像素在**水面**上的足迹（掠射角下长得极快，所以不能用"到相机的距离"代替）。
    float footprint = length(fwidth(p.xz));

    // 7 个八度：方向/波长都取不成比例的值（整齐的组合会拍出"华夫饼"点阵 + 摩尔纹）。
    const int   kCount = 7;
    const float kAng[kCount]    = float[kCount](0.00, 1.13, 2.41, 3.63, 5.02, 0.72, 4.31);
    const float kLamMul[kCount] = float[kCount](1.00, 0.63, 1.57, 0.37, 2.31, 5.00, 0.49);
    const float kWeight[kCount] = float[kCount](0.22, 0.17, 0.14, 0.10, 0.12, 0.15, 0.10);

    vec2 xz = p.xz;
    vec2 grad = vec2(0.0);
    float wsum = 0.0;
    for (int i = 0; i < kCount; ++i) {
        float lam = max(uWaveLengthM * kLamMul[i], 0.02);
        // ── 三种带限方式（对比用）──
        //   box 滤波一个余弦 = 振幅 × sinc(π·Δ/λ)（IQ "band limiting"）；
        //   硬关八度是它的粗略近似；不过滤就是 aliasing 基线。
        float keep = 1.0;
        if (uWaveFilterMode == 1) {
            keep = (footprint < 0.25 * lam) ? 1.0 : 0.0;                 // 硬切
        } else if (uWaveFilterMode >= 2) {
            float x = 3.14159265359 * footprint / lam;                   // sinc 带限
            keep = (x < 1.0e-4) ? 1.0 : sin(x) / x;
            keep = max(keep, 0.0);
        }
        if (keep <= 1.0e-5) {
            // 该八度被滤掉：把它的**斜率方差**留给高光/反射（LEAN 思路：能量不凭空消失）
            slope_var += (kWeight[i] * uWaveSlopeRad) * (kWeight[i] * uWaveSlopeRad) * 0.5;
            continue;
        }
        float w = kWeight[i] * keep;
        if (keep < 1.0) {
            slope_var += (kWeight[i] * uWaveSlopeRad) * (kWeight[i] * uWaveSlopeRad) *
                         (1.0 - keep * keep) * 0.5;
        }
        float a = uWaveDirRad + kAng[i];
        vec2 d = vec2(cos(a), sin(a));
        float k = 6.28318530718 / lam;
        float omega = inversesqrt(kLamMul[i]);        // 深水色散 ω ∝ 1/√λ
        grad += d * (w * cos(k * dot(d, xz) + omega * uWavePhase));
        wsum += w;
    }
    if (wsum <= 1.0e-5) {
        return vec3(0.0, 1.0, 0.0);
    }
    // 模式 0/1：按存活权重归一化（"波幅=最大斜率"与距离无关）；
    // 模式 2/3：sinc 已经把能量按物理衰减掉了，**不再归一化**（否则等于取消带限）。
    if (uWaveFilterMode <= 1) {
        grad /= wsum;
    }

    float slope = length(grad);
    if (slope > 1.0e-6) {
        grad *= min(uWaveSlopeRad, slope) / slope;
    } else {
        grad = vec2(0.0);
    }
    return normalize(vec3(-grad.x, 1.0, -grad.y));
}

void main() {
    // ── 逐像素视线 ∩ 水平平面（无限平面，没有几何边界）──
    vec3 dir = normalize(uCamFwd
                         + vNdc.x * uTanHalfFov * uAspect * uCamRight
                         + vNdc.y * uTanHalfFov * uCamUp);
    // 交点在**前方**才算命中：dir.y 与 (levelY − camY) 同号（水上朝下 / 水下朝上）。
    //   |dir.y| 极小时射线与水面平行 = 地平线本身，交给天空（否则 t → ∞ 出 NaN）。
    float dy = uLevelY - uCamPos.y;
    if (abs(dir.y) < 1.0e-7) {
        discard;
    }
    float t = dy / dir.y;
    if (!(t > 0.0)) {
        discard;
    }
    vec3 world = uCamPos + dir * t;

    // 手动深度测试：水面在这块不透明几何后面就整片丢弃（否则水会盖到挡在它前面的东西上）。
    //   ✱ 命中点**在远裁剪面之外**时一律不丢：那里只有天空（天空是最远的背景），
    //     水应该盖住它的 ground 色 —— 否则地平线附近（t → ∞）会重新漏出一条地色带。
    //     视轴方向距离 = t·(dir·fwd)，与 ZView() 同一量纲。
    float dist_view = t * dot(dir, uCamFwd);
    if (dist_view <= uFar) {
        float scene_z = ZView(texture(uSceneDepth, gl_FragCoord.xy / uViewport).r);
        if (dist_view > scene_z + 0.02) {
            discard;
        }
    }

    // 波浪查找点：把交点距离夹到远裁剪面（20 km 外的波纹早已被带限抹平，
    // 但世界坐标无界会让图集 uv / sin() 参数精度崩掉）。
    vec3 wave_pos = uCamPos + dir * min(t, uFar);

    float wave_slope_var = 0.0;                        // 被滤掉的八度留下的斜率方差
    vec3 N = WaveNormal(wave_pos, wave_slope_var);      // 波浪扰过的法线（几何仍是平面）
    vec3 V = -dir;                                     // 水面 → 相机（单位向量）
    float cos_i = clamp(abs(dot(N, V)), 1.0e-4, 1.0);  // 与法线夹角（双面）

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
        // ── 水上视角：镜面天光 + 水下透射 + 太阳高光 ──
        //
        // 三层 over 合成（背景 = 已经画好的水下场景）：
        //   ① 薄膜 B（本体色）+ ② 界面反射 F（天光）+ ③ 太阳高光（加法）
        // 展开：out = bg·(1−B)(1−F) + body·B·(1−F) + sky·F + sun
        // 固定管线只给 out = bg·(1−α) + color·α，所以：
        //   α = 1 − (1−B)(1−F)   （关掉"水下透射"时 α = 1：水面变成不透明本体色）
        //   color = (body·B·(1−F) + sky·F + sun) / α
        float F = FresnelAirToWater(cos_i, uIor);
        float B = clamp(uBodyAlpha, 0.0, 1.0);

        // LEAN：带限/硬件 mip 平均掉的斜率方差 = 该处已"看不见的粗糙度"。
        //   uLeanGain = 0 ⇒ lean_var = 0 ⇒ 逐字节回到旧行为（两个消费者都退回原路径）。
        float lean_var = max(wave_slope_var, 0.0) * max(uLeanGain, 0.0);

        vec3 sky = vec3(0.0);
        if (uUseSkyRefl == 1) {
            if (uHasEnvCube == 1) {
                // 镜面天光：反射方向采样天空探针（法线被波纹扰动 ⇒ 反光随涟漪碎开）。
                // LEAN：用它抬探针 mip，让远处反光变钝（而不是变成一块硬镜面）。
                float lod = (lean_var > 1.0e-9)
                                ? clamp(sqrt(lean_var) / max(uWaveSlopeRad, 1e-3) * 5.0,
                                        0.0, 5.0)
                                : 0.0;
                sky = textureLod(uEnvCube, reflect(-V, N), lod).rgb * uSkyScale;
            } else {
                sky = uRimColor;      // 没有探针：退回常量色（旧行为）
            }
        }

        vec3 sun = vec3(0.0);
        if (uUseSun == 1 && uSunColor != vec3(0.0)) {
            // Blinn-Phong 单瓣：H = normalize(V + L)。乘 F 是必须的 —— 太阳在水面上的
            // 高光本身就是菲涅耳反射，所以近处暗、远处（掠射）那条"太阳路"才亮。
            // 能量守恒的 Blinn-Phong 归一化：裸 pow(nh, n) 的峰值只有 1，用它算太阳
            // （辐照度量级 ~3）会得到"几乎看不见的小暗点"——真实太阳是**辐亮度**极高、
            // 角半径极小的光源，归一化因子 (n+8)/8π 补的就是这个峰值增益。
            // LEAN：把**带限丢掉的斜率方差**并进高光波瓣（LEAN 的核心一步）。
            //   Blinn-Phong 波瓣的等价斜率方差：σ₀² = 2/(n+2)  （半宽²）
            //   高斯方差可加 ⇒ σ² = σ₀² + var_lost
            //   再换回指数：n_eff = 2/σ² − 2
            // 效果：远处波瓣变宽（高光扩散成太阳路），而不是"变软消失"。
            float shin = max(uSunShininess, 1.0);
            if (lean_var > 1.0e-9) {
                float sigma2 = 2.0 / (shin + 2.0) + lean_var;
                shin = max(2.0 / max(sigma2, 1.0e-6) - 2.0, 2.0);
            }
            vec3 H = normalize(V + uSunDir);
            float nh = max(dot(N, H), 0.0);
            sun = uSunColor * F * pow(nh, shin) * ((shin + 8.0) / 25.1327412);
        }

        bool transmit = (uUseTransmit == 1);
        alpha = transmit ? clamp(1.0 - (1.0 - B) * (1.0 - F), 0.0, 1.0) : 1.0;
        vec3 under = transmit ? uBodyColor * (B * (1.0 - F)) : uBodyColor;
        color = (under + sky * F + sun) / max(alpha, 1.0e-4);
    }
    FragColor = vec4(color, alpha);
}
)glsl";

}  // namespace jpov

#endif  // JPOV_SRC_WATER_WATER_SKIN_SHADER_H_
