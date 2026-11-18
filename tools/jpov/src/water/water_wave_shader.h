// JPOV 水面波浪 —— GLSL 片段（与 water_wave_math.h 同源）
//
// 提供：
//   WaveFadeFactor(dist)          远处淡出
//   WaveFlowAt(p_xz)              采样 flow map（无纹理时恒 0）
//   WaveSampleAt(p_xz, t, fade, out h, out grad_xz)
//   WaveNormalFromGrad(grad_xz)
//   WaveSurfaceHit(cam, rd, level, inout t, out pos, out normal)
//
// ⚠️ 本片段**不含 `#version`**：使用方拼在 `#version 330 core` 之后、
//    水皮主体之前（uniform 声明与函数定义都必须在使用处可见）。
// ⚠️ 与 CPU 版一致：flow 方向的空间导数被忽略（dir 在每点当作常量）。

#ifndef JPOV_SRC_WATER_WATER_WAVE_SHADER_H_
#define JPOV_SRC_WATER_WATER_WAVE_SHADER_H_

namespace jpov {
namespace water {

inline constexpr const char* kWaterWaveGlsl = R"glsl(
// ── 波浪谱 uniform（与水皮共用同一个 program）──
uniform int   uWaveCount;          // 0 ⇒ 纯平面
uniform vec4  uWaveL[8];           // (k, omega, amplitude, phase)
uniform vec2  uWaveDir[8];         // 传播方向（单位，xz）
uniform float uWaveTime;           // 秒
uniform float uWaveFadeStart;      // 米（相机水平距离）
uniform float uWaveFadeEnd;        // 米；<= start ⇒ 不淡出
uniform float uWaveFlowAlign;      // [0,1]
uniform float uWaveFlowWindGain;   // >= 0
uniform int   uWaveHasFlow;        // 0 = 无 flow
uniform sampler2D uWaveFlowMap;    // RG 编码 (flow*0.5+0.5)
uniform vec2  uWaveFlowUvScale;    // p.xz * scale -> uv
uniform float uWaveFlowStrength;   // flow 整体增益
uniform int   uWaveDisplace;       // 0 = 法线 only（debug）；1 = 真位移

const int kWaveBisectIters = 12;
// 抗锯齿：一个波长占的像素数 ≤ 2 就消失、≥ 8 就全幅。
const float kWaveLodPxMin = 2.0;
const float kWaveLodPxMax = 8.0;

float WaveFadeFactor(float dist) {
    if (uWaveFadeEnd <= uWaveFadeStart) {
        return 1.0;
    }
    if (dist <= uWaveFadeStart) {
        return 1.0;
    }
    if (dist >= uWaveFadeEnd) {
        return 0.0;
    }
    float x = (dist - uWaveFadeStart) / (uWaveFadeEnd - uWaveFadeStart);
    return 1.0 - x * x * (3.0 - 2.0 * x);
}

vec2 WaveFlowAt(vec2 p) {
    if (uWaveHasFlow == 0) {
        return vec2(0.0);
    }
    vec2 rg = texture(uWaveFlowMap, p * uWaveFlowUvScale).rg;
    return (rg * 2.0 - 1.0) * uWaveFlowStrength;
}

// 高度 + 梯度。grad 的 x/y 分别对应 ∂h/∂x、∂h/∂z。
void WaveSampleAt(vec2 p, float t, float fade, float footprint, out float h, out vec2 grad) {
    h = 0.0;
    grad = vec2(0.0);
    if (uWaveCount <= 0 || fade <= 0.0) {
        return;
    }

    vec2 flow = WaveFlowAt(p);
    float flen = length(flow);
    vec2 fdir = (flen > 1.0e-6) ? flow / flen : vec2(0.0);
    float blend = (flen > 1.0e-6) ? clamp(uWaveFlowAlign * flen, 0.0, 1.0) : 0.0;
    float amp_gain = 1.0 + uWaveFlowWindGain * clamp(flen, 0.0, 1.0);

    for (int i = 0; i < 8; ++i) {
        if (i >= uWaveCount) {
            break;
        }
        vec4 L = uWaveL[i];
        vec2 d = uWaveDir[i];
        float dl = length(d);
        if (dl > 1.0e-6) {
            d = d / dl;
        }
        if (flen > 1.0e-6) {
            vec2 m = mix(d, fdir, blend);
            float ml = length(m);
            if (ml > 1.0e-4) {
                d = m / ml;
            }
        }
        float k = L.x;
        float omega = L.y;
        // 按像素足迹做谱截断（不是 mip）：细于 2 px 的波必须消失，
        // 否则远处波峰交界处的法线逐像素乱跳 ⇒ 反射/碎光变麻点。
        float lod_amp = 1.0;
        if (footprint > 1.0e-6) {
            float wavelength_px = (6.283185307179586 / k) / footprint;
            if (wavelength_px <= kWaveLodPxMin) {
                lod_amp = 0.0;
            } else if (wavelength_px < kWaveLodPxMax) {
                float u = (wavelength_px - kWaveLodPxMin) / (kWaveLodPxMax - kWaveLodPxMin);
                lod_amp = u * u * (3.0 - 2.0 * u);
            }
        }
        float amp = L.z * amp_gain * fade * lod_amp;
        float phase = k * dot(d, p) - omega * t + L.w;
        h += amp * sin(phase);
        grad += amp * k * d * cos(phase);
    }
}

vec3 WaveNormalFromGrad(vec2 grad) {
    return normalize(vec3(-grad.x, 1.0, -grad.y));
}

// f(t) = (cam.y + rd.y·t − level) − h(p.xz)
float WaveFAt(vec3 cam, vec3 ray_dir, float level, float footprint, float tt) {
    vec3 p = cam + ray_dir * tt;
    float dist = length(p.xz - cam.xz);
    float h;
    vec2 grad;
    WaveSampleAt(p.xz, uWaveTime, WaveFadeFactor(dist), footprint, h, grad);
    return (p.y - level) - h;
}

// 高度上界 ΣA·(1 + wind·|flow|)：二分区间半径的依据（|h| 永远不会超过它）。
float WaveAmplitudeSum() {
    float s = 0.0;
    for (int i = 0; i < 8; ++i) {
        if (i >= uWaveCount) {
            break;
        }
        s += uWaveL[i].z;
    }
    float flow_len = (uWaveHasFlow != 0) ? uWaveFlowStrength : 0.0;
    return s * (1.0 + uWaveFlowWindGain * clamp(flow_len, 0.0, 1.0));
}

// 视线 vs 高度场求交。
//   t 传入平面解、返回修正后的解；返回 false = 无交点。
//   **二分而不是 Newton**：区间两端必变号（|h| ≤ ΣA），全局收敛且相邻像素连续。
//   Newton 在波谷/陡坡处 f' = rd.y − ∇h·rd.xz → 0 ⇒ dt 爆炸/跳根 ⇒ 法线噪点。
bool WaveSurfaceHit(vec3 cam, vec3 ray_dir, float level, float footprint, inout float t,
                    out vec3 pos, out vec3 normal) {
    pos = vec3(0.0);
    normal = vec3(0.0, 1.0, 0.0);

    // 法线 only（debug）：求交仍用平面解。
    if (uWaveDisplace == 0) {
        pos = cam + ray_dir * t;
        float dist = length(pos.xz - cam.xz);
        float h;
        vec2 grad;
        WaveSampleAt(pos.xz, uWaveTime, WaveFadeFactor(dist), footprint, h, grad);
        normal = WaveNormalFromGrad(grad);
        return true;
    }

    float rdy = max(abs(ray_dir.y), 1.0e-3);
    float span = WaveAmplitudeSum() / rdy;
    float t_plane = t;
    float t_lo = max(t_plane - span, 1.0e-4);
    float t_hi = t_plane + span;
    if (t_hi <= t_lo) {
        t_hi = t_lo + 1.0e-4;
    }
    float f_lo = WaveFAt(cam, ray_dir, level, footprint, t_lo);
    float f_hi = WaveFAt(cam, ray_dir, level, footprint, t_hi);
    if ((f_lo > 0.0) == (f_hi > 0.0)) {
        // 理论不会发生；兜底回平面解（不要给出一个乱跳的法线）。
        t = t_plane;
        pos = cam + ray_dir * t;
        return true;
    }
    bool lo_positive = (f_lo > 0.0);
    for (int i = 0; i < kWaveBisectIters; ++i) {
        float tm = 0.5 * (t_lo + t_hi);
        bool m_positive = (WaveFAt(cam, ray_dir, level, footprint, tm) > 0.0);
        if (m_positive == lo_positive) {
            t_lo = tm;
        } else {
            t_hi = tm;
        }
    }
    t = 0.5 * (t_lo + t_hi);
    if (t <= 0.0) {
        return false;
    }
    pos = cam + ray_dir * t;
    float dist = length(pos.xz - cam.xz);
    float h;
    vec2 grad;
    WaveSampleAt(pos.xz, uWaveTime, WaveFadeFactor(dist), footprint, h, grad);
    normal = WaveNormalFromGrad(grad);
    return true;
}
)glsl";

}  // namespace water
}  // namespace jpov

#endif  // JPOV_SRC_WATER_WATER_WAVE_SHADER_H_
