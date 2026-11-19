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
// 域扭曲（domain warp）：低频位移场，θ += k·dot(u, d)。uWaveWarpAmp = 0 ⇒ 关。
uniform float uWaveWarpAmp;        // 每条载波的位移幅度（米）
uniform vec4  uWaveWarp[4];        // 每条载波 (k, dir_x, dir_z, phase)
// 二阶 Stokes 谐波（尖峰平谷）。0 ⇒ 关。
uniform int   uWaveStokes2;
// 细节短波（只进法线，不进几何）：(k, amplitude, phase, 未用) + 方向。
uniform int   uWaveDetailCount;
uniform vec4  uWaveDetailL[64];    // (k, amplitude, phase, 未用)
uniform vec2  uWaveDetailDir[64];
// 法线场过滤（远处亚像素波 → 粗糙度）
uniform vec2  uShortWaveDir;         // 短波风向（xz，单位；0,0 = 各向同性）
uniform float uShortWaveMss;         // 短波总斜率方差（σ²）

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

// 低频位移场 u(p) 与它的空间导数（域扭曲）。
// u = Σ_j A·dir_j·sin(k_j·dot(dir_j,p) + φ_j)，∂u/∂x = Σ_j A·dir_j·k_j·cos(·)·dir_j.x。
void WaveWarpAt(vec2 p, out vec2 u, out vec2 du_dx, out vec2 du_dz) {
    u = vec2(0.0);
    du_dx = vec2(0.0);
    du_dz = vec2(0.0);
    if (uWaveWarpAmp <= 0.0) {
        return;
    }
    for (int j = 0; j < 4; ++j) {
        float k = uWaveWarp[j].x;
        vec2 d = uWaveWarp[j].yz;
        float ph = k * dot(d, p) + uWaveWarp[j].w;
        float s = sin(ph);
        float c = cos(ph);
        u += uWaveWarpAmp * d * s;
        du_dx += uWaveWarpAmp * d * (k * c) * d.x;
        du_dz += uWaveWarpAmp * d * (k * c) * d.y;
    }
}

// ── 法线场过滤：远处亚像素波 → BRDF 粗糙度（斜率方差张量）─────────────
// 与 CPU 的 WaveFilteredVariance 同源。输出 xz 平面的 2x2 张量：
//   xx = E[(∂h/∂x)²]、xz = E[∂h/∂x·∂h/∂z]、zz = E[(∂h/∂z)²]
// 来源 = 被足迹截断的 4 波（不再贡献几何的那部分）+ 短波（2.7:1 各向异性）。
void WaveSlopeTensor(float footprint, float fade, out float sxx, out float sxz, out float szz) {
    sxx = 0.0; sxz = 0.0; szz = 0.0;
    if (uWaveCount > 0 && fade > 0.0) {
        for (int i = 0; i < 8; ++i) {
            if (i >= uWaveCount) {
                break;
            }
            float k = uWaveL[i].x;
            float amp = uWaveL[i].z * fade;
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
            float a = amp * k;
            float w = 0.5 * a * a * (1.0 - lod_amp * lod_amp);
            vec2 d = uWaveDir[i];
            sxx += w * d.x * d.x;
            sxz += w * d.x * d.y;
            szz += w * d.y * d.y;
        }
        // 细节短波：被足迹截断的部分同样折进方差（否则远处会“白丢”能量）。
        for (int i = 0; i < 64; ++i) {
            if (i >= uWaveDetailCount) {
                break;
            }
            float k = uWaveDetailL[i].x;
            float amp = uWaveDetailL[i].y * fade;
            float lod_amp = 1.0;
            if (footprint > 1.0e-6) {
                float wavelength_px = (6.283185307179586 / k) / footprint;
                if (wavelength_px <= kWaveLodPxMin) {
                    lod_amp = 0.0;
                } else if (wavelength_px < kWaveLodPxMax) {
                    float uu = (wavelength_px - kWaveLodPxMin) / (kWaveLodPxMax - kWaveLodPxMin);
                    lod_amp = uu * uu * (3.0 - 2.0 * uu);
                }
            }
            float a = amp * k;
            float w = 0.5 * a * a * (1.0 - lod_amp * lod_amp);
            vec2 d = uWaveDetailDir[i];
            sxx += w * d.x * d.x;
            sxz += w * d.x * d.y;
            szz += w * d.y * d.y;
        }
    }
    if (uShortWaveMss > 0.0) {
        vec2 d = uShortWaveDir;
        float dl = length(d);
        if (dl > 1.0e-6) {
            d /= dl;
            vec2 p = vec2(-d.y, d.x);
            float along = 0.73 * uShortWaveMss;
            float cross = 0.27 * uShortWaveMss;
            sxx += along * d.x * d.x + cross * p.x * p.x;
            sxz += along * d.x * d.y + cross * p.x * p.y;
            szz += along * d.y * d.y + cross * p.y * p.y;
        } else {
            sxx += 0.5 * uShortWaveMss;
            szz += 0.5 * uShortWaveMss;
        }
    }
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

    // 域扭曲：整片场沿低频位移 u(p) 平移，θ += k·dot(u, d)。
    vec2 wu, wdux, wduz;
    WaveWarpAt(p, wu, wdux, wduz);

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
        float wx = k * dot(wdux, d);
        float wz = k * dot(wduz, d);
        float phase = k * dot(d, p) - omega * t + L.w + k * dot(wu, d);
        float sin_p = sin(phase);
        float cos_p = cos(phase);
        vec2 g = vec2(k * d.x + wx, k * d.y + wz);
        h += amp * sin_p;
        grad += amp * g * cos_p;
        if (uWaveStokes2 != 0) {
            // −½kA²cos2θ：尖峰平谷。cos2θ = 1−2sin²θ，sin2θ = 2sinθcosθ。
            float sin2 = 2.0 * sin_p * cos_p;
            h += -0.5 * k * amp * amp * (1.0 - 2.0 * sin_p * sin_p);
            grad += k * amp * amp * sin2 * g;
        }
    }
}

// 细节短波：**只改法线梯度，不改高度**（不参与求交）。
void WaveDetailGrad(vec2 p, float t, float fade, float footprint,
                    vec2 wu, vec2 wdux, vec2 wduz, out vec2 grad) {
    grad = vec2(0.0);
    if (uWaveDetailCount <= 0 || fade <= 0.0) {
        return;
    }
    for (int i = 0; i < 64; ++i) {
        if (i >= uWaveDetailCount) {
            break;
        }
        vec4 L = uWaveDetailL[i];
        vec2 d = uWaveDetailDir[i];
        float dl = length(d);
        d = (dl > 1.0e-6) ? d / dl : vec2(1.0, 0.0);
        float k = L.x;
        float lod_amp = 1.0;
        if (footprint > 1.0e-6) {
            float wavelength_px = (6.283185307179586 / k) / footprint;
            if (wavelength_px <= kWaveLodPxMin) {
                lod_amp = 0.0;
            } else if (wavelength_px < kWaveLodPxMax) {
                float uu = (wavelength_px - kWaveLodPxMin) / (kWaveLodPxMax - kWaveLodPxMin);
                lod_amp = uu * uu * (3.0 - 2.0 * uu);
            }
        }
        float amp = L.y * fade * lod_amp;
        float omega = sqrt(9.81 * k);
        float phase = k * dot(d, p) - omega * t + L.z + k * dot(wu, d);
        vec2 g = vec2(k * d.x, k * d.y) + vec2(k * dot(wdux, d), k * dot(wduz, d));
        grad += amp * g * cos(phase);
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
// 有效振幅上界 ΣA·fade·lod_amp（**必须和场本身同口径**）。
// ⚠️ 旧版用不衰减的 ΣA：在远处谱已被截断成平面，区间却仍按 ΣA/|rd.y| 撑开，
//    掠射时 span ~ km，12 次二分的量化步长就是几十~几百米 ⇒ 交点 t 在相邻像素间乱跳，
//    而这个 t 会作为透明层深度喂给合成趟去切介质 ⇒ 介质段长度突变 ⇒ 硬边。
float WaveAmplitudeSum(float fade, float footprint) {
    float s = 0.0;
    for (int i = 0; i < 8; ++i) {
        if (i >= uWaveCount) {
            break;
        }
        float lod_amp = 1.0;
        if (footprint > 1.0e-6) {
            float wavelength_px = (6.283185307179586 / uWaveL[i].x) / footprint;
            if (wavelength_px <= kWaveLodPxMin) {
                lod_amp = 0.0;
            } else if (wavelength_px < kWaveLodPxMax) {
                float u = (wavelength_px - kWaveLodPxMin) / (kWaveLodPxMax - kWaveLodPxMin);
                lod_amp = u * u * (3.0 - 2.0 * u);
            }
        }
        s += uWaveL[i].z * lod_amp;
        if (uWaveStokes2 != 0) {
            float aa = uWaveL[i].z * lod_amp;
            s += 0.5 * uWaveL[i].x * aa * aa;
        }
    }
    float flow_len = (uWaveHasFlow != 0) ? uWaveFlowStrength : 0.0;
    return s * fade * (1.0 + uWaveFlowWindGain * clamp(flow_len, 0.0, 1.0));
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
        float fade = WaveFadeFactor(dist);
        float h;
        vec2 grad;
        WaveSampleAt(pos.xz, uWaveTime, fade, footprint, h, grad);
        vec2 wu, wdux, wduz;
        WaveWarpAt(pos.xz, wu, wdux, wduz);
        vec2 dgrad;
        WaveDetailGrad(pos.xz, uWaveTime, fade, footprint, wu, wdux, wduz, dgrad);
        normal = WaveNormalFromGrad(grad + dgrad);
        return true;
    }

    float rdy = max(abs(ray_dir.y), 1.0e-3);
    float t_plane = t;
    // 区间半径用**平面交点处的有效振幅**（含 fade 与足迹截断）：
    // 远处谱已截断成平面 ⇒ span → 0 ⇒ t 连续，不会把量化噪声喂给合成趟的层深。
    vec3 p_plane = cam + ray_dir * t_plane;
    float span = WaveAmplitudeSum(WaveFadeFactor(length(p_plane.xz - cam.xz)), footprint) / rdy;
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
    float fade = WaveFadeFactor(dist);
    float h;
    vec2 grad;
    WaveSampleAt(pos.xz, uWaveTime, fade, footprint, h, grad);
    vec2 wu, wdux, wduz;
    WaveWarpAt(pos.xz, wu, wdux, wduz);
    vec2 dgrad;
    WaveDetailGrad(pos.xz, uWaveTime, fade, footprint, wu, wdux, wduz, dgrad);
    normal = WaveNormalFromGrad(grad + dgrad);
    return true;
}
)glsl";

}  // namespace water
}  // namespace jpov

#endif  // JPOV_SRC_WATER_WATER_WAVE_SHADER_H_
