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
//     界面反射率 R(θ) = **精确未极化 Fresnel（水→空气）**，临界角 θc = asin(1/ior) ≈ 48.6°：
//       θ ≥ θc ⇒ R = 1（全内反射）⇒ 整片是"地色"（水面变成一面暗镜）
//       θ <  θc ⇒ 0.02 ≤ R < 1，窗内按**折射方向**采天空探针，乘 n² 辐亮度增益
//     合成：color = skyProbe(refract) · (n_w/n_a)² · (1−R) + tirColor · R，**α = 1**
//     （水下抬头时水面就是该方向最近的面 ⇒ 整片替换；近似点见文件末尾）
//     "折射方向"这一步就是 Snell 窗的全部观感来源：整个 0..90° 的天空被压进 0..48.6°，
//     于是**地平线正好压在锥的边缘**、云和太阳被挤到边上去。少了它，窗内只是"原样天空"
//     被一个圆切开 —— 看着不像从水下看外面。
//     水面不平（surface_slope_rms_deg > 0）时对**精确 R**做 5 抽头斜率分布平均（不是把 R
//     往 1 推 —— 那会把过渡带整体往窗内偏，窗内出现一片灰）。
//
// 本版**不做**（写在前面，免得当成 bug）：
//   * 水下没有吸收/散射（这是"绝对干净的水"）；
//   * 窗内不画**水上几何**的折射像（只采天空探针）⇒ 水上物体在窗内不出现（屏幕空间折射的
//     固有取舍；要画就得把整个上半场景当环境图重渲一遍）；
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
//   * 没有波浪（quad 是纯平面，没有逐顶点位移）。

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
uniform float uIor;              // 水的折射率（水→空气临界角 = asin(1/ior)）
uniform float uSlopeRmsRad;      // 水面 RMS 斜率（弧度）；0 = 理想镜面（临界角是硬边）
uniform samplerCube uEnvCube;    // 天空探针（大气色，不含日月盘；下半球=地色）
uniform int   uHasEnvCube;       // 0 = 没有探针（退化：不做折射采样）
uniform sampler2D uSceneColor;   // 本 pass 之前 HDR 颜色的**拷贝**（水上几何的折射查找）
uniform int   uHasSceneColor;    // 0 = 没有拷贝（只采天空探针）
uniform mat4  uVP;               // world → clip（与 VS 同一个矩阵，屏幕空间 march 用）


// 辐亮度守恒：光由空气进水里，L_water = L_air · (1−R) · (n_w/n_a)²
// （n² = 1.777 就是"水下的窗口看起来比天上的同一片天更亮"的来源）。
float AirToWaterGain() { return uIor * uIor; }

// 从水（n = eta）射向空气（n = 1）的界面反射率（未极化）。
// 超过临界角 ⇒ 全内反射 ⇒ 返回 1（这一步就是“角度超过特定值时用地色补充”的开关）。
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

// 粗糙界面 = 把**精确 R** 对斜率分布做平均（不是把 R 往 1 推：后者会把过渡带整体
// 往窗内偏 ~6°，窗内出现一大片灰）。5 抽头，高斯权重；slope ≈ 0 时退化为精确单点。
float FresnelFromWaterSoft(float cos_i, float eta, float slope_rms) {
    if (slope_rms <= 1.0e-4) {
        return FresnelFromWater(cos_i, eta);
    }
    float theta = acos(clamp(cos_i, -1.0, 1.0));
    float sum_r = 0.0;
    float sum_w = 0.0;
    for (int i = -2; i <= 2; ++i) {
        float fi = float(i);
        float th = clamp(theta + fi * 0.5 * slope_rms, 0.0, 1.5533);   // < π/2
        float wgt = exp(-2.0 * fi * fi);
        sum_r += wgt * FresnelFromWater(cos(th), eta);
        sum_w += wgt;
    }
    return sum_r / sum_w;
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

// 折射 march 的最远距离（米）。水上几何都在这个范围内；超出就退回天空探针。
const float kMaxMarchM = 400.0;

// 屏幕空间折射：从入水点 P 沿**折射方向** rdir 走，与不透明场景的深度比，
// 第一次穿过场景表面就算命中，取那里的颜色（= 水上物体透过水面的像）。
//
// 为什么需要它：天空探针里没有**几何**。少了这一步，窗内只有天，站在水上的人/船
// 不会出现在窗里 —— 而真实的 Snell 窗是把"整个上半世界"（天 + 水上物体）压进锥里。
//
// 已知近似：只能查到**当前屏幕内**的东西（离开视锥的物体取不到）；步长是指数增长的，
// 极细的物体可能被跨过去（此处物体都远大于步长）。
bool TraceRefracted(vec3 P, vec3 rdir, out vec3 outColor) {
    float t = 0.05;
    for (int i = 0; i < 32; ++i) {
        vec3 X = P + rdir * t;
        vec4 clip = uVP * vec4(X, 1.0);
        if (clip.w <= 1.0e-4) {
            break;
        }
        vec2 uv = (clip.xy / clip.w) * 0.5 + 0.5;
        if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
            break;
        }
        float ray_z = ZView(clip.z / clip.w * 0.5 + 0.5);
        float scene_z = ZView(texture(uSceneDepth, uv).r);
        // 穿过（或贴到）场景表面：命中。不做厚度判断 —— 射线是从水面往上走的，
        // 遇到的第一个不透明面就是它；贴太近（ray_z 略小于 scene_z）也当命中，
        // 否则薄物体（人形）会被整块漏掉。
        if (ray_z >= scene_z * 0.98) {
            outColor = texture(uSceneColor, uv).rgb;
            return true;
        }
        t += max(0.1, t * 0.35);
        if (t > kMaxMarchM) {
            break;
        }
    }
    return false;
}

void main() {
    // 手动深度测试：水面在这块不透明几何后面就整片丢弃（否则水会盖到挡在它前面的东西上）。
    float scene_z = ZView(texture(uSceneDepth, gl_FragCoord.xy / uViewport).r);
    float z = ZView(gl_FragCoord.z);
    if (z > scene_z + 0.02) {
        discard;
    }

    vec3 N = vec3(0.0, 1.0, 0.0);                      // 水面法线（本版无波浪）
    vec3 V = normalize(uCamPos - vWorld);
    float cos_i = clamp(abs(V.y), 1.0e-4, 1.0);       // = |N·V|，双面

    vec3 color;
    float alpha;
    if (uCamPos.y < uLevelY) {
        // ── 水下抬头看水面 ──
        // R 由精确 Fresnel 给（含 TIR）；窗内按**折射方向**采天空并乘 n²，
        // 窗外整片给地色（全内反射看到的是水下环境，本版用地色代表）。
        float R = FresnelFromWaterSoft(cos_i, uIor, uSlopeRmsRad);
        if (uHasEnvCube == 1) {
            // refract(I, N, eta)：I = 入射方向（-V，从相机指向水面），
            // N 必须指向入射侧（相机在水下 ⇒ -N），eta = n_水 / n_空气 = uIor。
            //
            // ⚠️ 两个坑（都实测踩过）：
            //   ① 采 mip 必须用 **lod 0**（镜面水）。拿 uEnvMaxLod（=log2(128)=7）当 lod
            //      会采到 1×1 的那一级 ⇒ 整个窗变成一个**常量色**（没有渐变、看着像贴纸）。
            //   ② TIR 时 GLSL 的 refract() 返回 0 向量，normalize(0) = NaN，NaN 乘 (1−R)=0
            //      仍是 NaN ⇒ 整片像素未定义。所以 TIR 侧改用水平方向的天色（= 窗缘收敛值），
            //      让 R→1 的路上颜色连续。
            vec3 rdir;
            if (uIor * uIor * (1.0 - cos_i * cos_i) < 1.0) {
                rdir = normalize(refract(-V, -N, uIor));
            } else {
                rdir = normalize(vec3(-V.x, 0.0, -V.z));
            }
            // 先找**水上几何**的折射像（屏幕空间），没命中才退回天空探针。
            // 两者都是"透过水面看到的东西"，所以都要乘同一个 n² 辐亮度增益。
            vec3 seen = textureLod(uEnvCube, rdir, 0.0).rgb;
            if (uHasSceneColor == 1) {
                vec3 hit_color;
                if (TraceRefracted(vWorld, rdir, hit_color)) {
                    seen = hit_color;
                }
            }
            // 水面就是该方向最近的面 ⇒ 直接替换（不再与背景 alpha 混合）。
            color = mix(seen * AirToWaterGain(), uTirColor, clamp(R, 0.0, 1.0));
            alpha = 1.0;
        } else {
            // 没探针（关掉了环境镜面）：退回"背景透射 + 地色"的近似 —— 窗内看到的
            // 是**没被折射压缩**的天，观感不对，但至少不会伪造出假天空。
            color = uTirColor;
            alpha = clamp(R, 0.0, 1.0);
        }
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
