// JPOV 水体介质 —— GLSL 提供者片段（拼进合成趟 FS）
//
// 契约（合成趟唯一要求）：本片段必须定义
//     void MediumAt(vec3 ro, vec3 rd, float z, out vec3 tau, out vec3 S);
//   · z   = 沿视线从相机起的**欧氏距离**（米），与合成趟的 z_op / 层深度同口径；
//   · tau = 「相机 → z」在水体里的累积光学厚度（逐通道）⇒ 背景透射率 = exp(−tau)；
//   · S   = 同一前缀的累积内散射辐亮度（逐通道）。
// 合成趟把本项与 froxel 雾场的 (τ, S) **直接相加**，再与透明层按深度交替积分 ——
//  所以这里必须是**前缀累积量**，不是局部段贡献。
//
// 物理与 src/water/water_medium_math.h 的 WaterAt **同源**（改一处必须改另一处），
//  由 test/water:water_medium_gpu_test 逐像素对拍兜底。
//
// 本版不做（见 known_issue §11）：CSM 阴影（太阳项按全受照）、多次散射、视线折射、
//  环境场随深度的自身衰减。

#ifndef JPOV_SRC_WATER_WATER_MEDIUM_SHADER_H_
#define JPOV_SRC_WATER_WATER_MEDIUM_SHADER_H_

namespace jpov {
namespace water {

// 介质 uniform（由 FogCompositeRenderer::Draw 上传；名字是唯一出处）。
inline constexpr const char* kWaterMediumGlsl = R"glsl(
uniform int   uHasWater;         // 0 = 本帧无水体介质（MediumAt 恒等，零开销）
uniform float uWaterLevelY;      // 水面世界高度（米，y-up）
uniform float uWaterIor;         // 界面折射率（只影响菲涅尔/折射方向，不产生位移）
uniform vec3  uWaterSigmaA;      // 吸收系数 (1/m)，逐通道
uniform vec3  uWaterSigmaS;      // 散射系数 (1/m)，逐通道
uniform float uWaterPhaseG;      // HG 各向异性 ∈ [0, 0.95)
uniform vec3  uWaterAmbient;     // 空气侧 ambient（已乘 intensity）
uniform vec3  uWaterSunColor;    // 空气侧太阳（已乘 intensity）
uniform vec3  uWaterSunToward;   // 指向太阳（空气侧，单位）
uniform int   uWaterHasSun;      // 0 = 无太阳（只剩环境项）

const float kWmAmbientT = 0.94;  // 界面漫射透射率（与 CPU 侧同名常量对齐）

// Henyey-Greenstein（与 fog 的 HgPhase 同一实现；此处独立命名避免与雾冲突）。
float WmHgPhase(float mu, float g) {
    float gc = clamp(g, 0.0, 0.95);
    float denom = max(1.0 + gc * gc - 2.0 * gc * mu, 1.0e-4);
    return (1.0 - gc * gc) / (4.0 * 3.14159265 * denom * sqrt(denom));
}

// 无偏振电介质菲涅尔（含全内反射）。与 water_skin_shader.h 的实现一致，但独立命名。
float WmFresnel(float cos_i, float n1, float n2) {
    float eta = n1 / n2;
    float sin2_t = eta * eta * (1.0 - cos_i * cos_i);
    if (sin2_t >= 1.0) {
        return 1.0;
    }
    float cos_t = sqrt(1.0 - sin2_t);
    float rs = (n1 * cos_i - n2 * cos_t) / (n1 * cos_i + n2 * cos_t);
    float rp = (n1 * cos_t - n2 * cos_i) / (n1 * cos_t + n2 * cos_i);
    return 0.5 * (rs * rs + rp * rp);
}

// ∫₀^len exp(−sigma·rate·t) dt，逐分量；sigma·rate → 0 时退化到 len（级数展开）。
vec3 WmDecayLength(vec3 sigma, float rate, float len) {
    vec3 x = sigma * rate * len;
    vec3 out_v;
    for (int i = 0; i < 3; ++i) {
        out_v[i] = (abs(x[i]) < 1.0e-4)
            ? len * (1.0 - 0.5 * x[i])
            : (1.0 - exp(-x[i])) / (sigma[i] * rate);
    }
    return out_v;
}

// 空气侧「指向太阳」→ 水内「指向太阳」（法线 = +Y）。ok = 0 表示无直射。
// （GLSL 没有指针，out 参数直接传变量名）
vec3 WmRefractTowardSun(vec3 air_toward, float ior, out int ok) {
    float cy = air_toward.y;
    if (cy <= 1.0e-4 || !(ior > 0.0)) {
        ok = 0;
        return vec3(0.0, 1.0, 0.0);
    }
    float sin2_i = max(0.0, 1.0 - cy * cy);
    float sin2_w = sin2_i / (ior * ior);
    float cos_w = sqrt(max(0.0, 1.0 - sin2_w));
    int k = (sin2_i > 1.0e-12) ? 1 : 0;
    float scale_xz = (k != 0) ? sqrt(sin2_w / sin2_i) : (1.0 / ior);
    ok = 1;
    return vec3(air_toward.x * scale_xz, cos_w, air_toward.z * scale_xz);
}

void MediumAt(vec3 ro, vec3 rd, float z, out vec3 tau, out vec3 S) {
    tau = vec3(0.0);
    S = vec3(0.0);
    if (uHasWater == 0 || z <= 0.0) {
        return;
    }

    // ── 段：射线与半空间 (y <= level) 的交，截到 [0, z] ──
    float t0 = 0.0;
    float t1 = z;
    if (ro.y > uWaterLevelY) {
        if (rd.y >= -1.0e-6) {
            return;                    // 向上/平行：永远不进水体
        }
        t0 = (uWaterLevelY - ro.y) / rd.y;
        if (t0 >= t1) {
            return;                    // 水面在物体之后
        }
    } else if (rd.y > 1.0e-6) {
        t1 = min(t1, (uWaterLevelY - ro.y) / rd.y);   // 水下向上：到水面为止
    }
    float len = t1 - t0;
    if (len <= 0.0) {
        return;
    }

    // ── 视线在水里的**真实方向**（L0）：水上 = 折射后，水下 = 原方向 ──
    // 只修正路径长度，不位移图像（屏幕空间折射 gather 未做）。
    bool from_air = ro.y > uWaterLevelY;
    float dy_w = rd.y;
    if (from_air) {
        float sin2_i = max(0.0, 1.0 - rd.y * rd.y);
        float sin2_w = sin2_i / (uWaterIor * uWaterIor);
        dy_w = -sqrt(max(0.0, 1.0 - sin2_w));
    }
    if (abs(dy_w) < 1.0e-6) {
        return;
    }
    float len_w = len * (abs(rd.y) / abs(dy_w));

    vec3 st = uWaterSigmaA + uWaterSigmaS;
    tau = st * len_w;

    // ── 直射项（闭式）：指数 = σt·[(t−t0) + d(t)/u_w.y]，对 t 线性 ──
    int sun_ok = 0;
    vec3 toward = WmRefractTowardSun(uWaterSunToward, uWaterIor, sun_ok);
    if (uWaterHasSun != 0 && sun_ok != 0) {
        float cos_i = uWaterSunToward.y;
        float fres = WmFresnel(cos_i, 1.0, uWaterIor);
        float gain = (1.0 - fres) / (uWaterIor * uWaterIor);
        float B = 1.0 - dy_w / toward.y;   // 真实路径上的指数斜率
        float d0 = uWaterLevelY - (ro.y + rd.y * t0);
        float phase = WmHgPhase(clamp(dot(rd, toward), -1.0, 1.0), uWaterPhaseG);
        vec3 exp_sun = exp(-st * (d0 / toward.y));
        S = uWaterSigmaS * uWaterSunColor * (phase * gain) * exp_sun *
            WmDecayLength(st, B, len_w);
    }

    // ── 环境项（各向同性：无相位、无太阳路径衰减）──
    S += uWaterSigmaS * (uWaterAmbient * kWmAmbientT) *
         WmDecayLength(st, 1.0, len_w);
}
)glsl";

// 缺省提供者（无水体）：合成趟在没有水时的等价物，保证同一套 FS 组装路径。
inline constexpr const char* kDefaultMediumGlsl = R"glsl(
void MediumAt(vec3 ro, vec3 rd, float z, out vec3 tau, out vec3 S) {
    tau = vec3(0.0);
    S = vec3(0.0);
}
)glsl";

}  // namespace water
}  // namespace jpov

#endif  // JPOV_SRC_WATER_WATER_MEDIUM_SHADER_H_
