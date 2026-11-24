// JPOV 水面波浪 —— GLSL 片段（与 water_wave_math.h 同源）
//
// 提供：
//   WaveFlowAt(p_xz)              采样 flow map（无纹理时恒 0）
//   WaveSampleAt(p_xz, t, footprint, need_grad, out h, out grad_xz)
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
// 细节短波（只进法线，不进几何）：(k, amplitude, phase, omega) + 方向。
// omega 由 CPU 按有限水深算好传上来（与主波同一个 DispersionOmega）：GPU 不重算 ⇒
// 每像素省 64 次 sqrt，且 CPU 参考与 GPU 逐位同源（"同源"不能只写在注释里）。
uniform int   uWaveDetailCount;
uniform vec4  uWaveDetailL[64];    // (k, amplitude, phase, omega)
uniform vec2  uWaveDetailDir[64];
// 法线场过滤（远处亚像素波 → 粗糙度）
uniform vec2  uShortWaveDir;         // 短波风向（xz，单位；0,0 = 各向同性）
uniform float uShortWaveMss;         // 短波总斜率方差（σ²）

const int kWaveBisectIters = 12;
// 抗锯齿：一个波长占的像素数 ≤ kMin 就消失、≥ kMax 就全幅（当前 3 / 8）。
// 2 px 是 Nyquist 的理论极限，但逐像素中心采样 + 双线性插值实际需要 ≥ 4 px 才不走样
//（2026-11-23「低风速海面硬边」= 2~4 px 的细节波在反射里走样）。
// ⚠️ 与 CPU 的 water::kWaveLodPxMin / kWaveLodPxMax 同值（GPU 对拍测试兜底）。
const float kWaveLodPxMin = 3.0;
const float kWaveLodPxMax = 8.0;
// 掠射护栏（＝ CPU 的 water::kGrazeFloorSin，**只是除零护栏**）：|rd.y| 更小时
// 足迹按这个下限算，再被 kWaveMaxFootprintM 钳住。2026-11-24 之前这里是 0.15（≈8.6°），
// 那会把足迹系统性算小 3 倍（远处"看起来很细"其实是亚像素波没被杀）。
const float kWaveGrazeFloorSin = 1.0e-4;
const float kWaveMaxFootprintM = 1.0e6;

// 像素在水面上覆盖的世界尺寸（米）—— 与 CPU 的 water::WaveFootprintM 同一公式。
float WaveFootprint(vec3 cam_pos, vec3 ray_dir, float level_y, float pixel_angle) {
    float t_plane = (level_y - cam_pos.y) / ray_dir.y;
    if (t_plane <= 0.0) {
        return 0.0;   // 交点在相机后方 ⇒ 不做截断（上层自己 discard）
    }
    float graze = max(abs(ray_dir.y), kWaveGrazeFloorSin);
    return min(t_plane * pixel_angle / graze, kWaveMaxFootprintM);
}
// warp 限带：每条波的 warp 相位/梯度乘 min(1, kWarpSlopeLimit/(k·U))。
// 与 CPU 的 water::kWarpSlopeLimit 同值（有测试锁一致性）。
const float kWarpSlopeLimit = 0.25;

vec2 WaveFlowAt(vec2 p) {
    if (uWaveHasFlow == 0) {
        return vec2(0.0);
    }
    vec2 rg = texture(uWaveFlowMap, p * uWaveFlowUvScale).rg;
    return (rg * 2.0 - 1.0) * uWaveFlowStrength;
}

// warp 位移场的梯度上界 U = A·Σ k_j（无量纲）。与 CPU 的 WaveWarpSlopeBound 同源。
// 每条波的 warp 相位/梯度乘 min(1, kWarpSlopeLimit/(k·U))：长波照旧掰弯，
// 短波（k·U 大）几乎不动，避免相位被 k·u 打散成拉丝/漩涡。
float WaveWarpSlopeBound() {
    if (uWaveWarpAmp <= 0.0) {
        return 0.0;
    }
    float sum_k = 0.0;
    for (int j = 0; j < 4; ++j) {
        sum_k += uWaveWarp[j].x;
    }
    return uWaveWarpAmp * sum_k;
}

// 低频位移场 u(p) 与它的空间导数（域扭曲）。
// u = Σ_j A·dir_j·sin(k_j·dot(dir_j,p) + φ_j)，∂u/∂x = Σ_j A·dir_j·k_j·cos(·)·dir_j.x。
// need_grad=false ⇒ 只算 u（不求导、不调 cos）：二分只查高度，导数白算。
void WaveWarpAt(vec2 p, bool need_grad, out vec2 u, out vec2 du_dx, out vec2 du_dz) {
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
        float c = need_grad ? cos(ph) : 0.0;
        u += uWaveWarpAmp * d * s;
        du_dx += uWaveWarpAmp * d * (k * c) * d.x;
        du_dz += uWaveWarpAmp * d * (k * c) * d.y;
    }
}

// ── 法线场过滤：远处亚像素波 → BRDF 粗糙度（斜率方差张量）─────────────
// 与 CPU 的 WaveFilteredVariance 同源。输出 xz 平面的 2x2 张量：
//   xx = E[(∂h/∂x)²]、xz = E[∂h/∂x·∂h/∂z]、zz = E[(∂h/∂z)²]
// 来源 = 被足迹截断的 4 波（不再贡献几何的那部分）+ 短波（2.7:1 各向异性）。
// ⚠️ 能量守恒（2026-11-20）：足迹截断只决定「这条波还剩多少在几何里」，
//    它**没进几何的那部分必须整份进 BRDF**，否则远处会退化成镜面（映灰色天空）。
//    旧写法把方差也乘了衰减系数 ⇒ 远场归零。
//    正确式：w = ½(kA)²·(1 − lod²)。
void WaveSlopeTensor(float footprint, out float sxx, out float sxz, out float szz) {
    sxx = 0.0; sxz = 0.0; szz = 0.0;
    if (uWaveCount > 0) {
        for (int i = 0; i < 8; ++i) {
            if (i >= uWaveCount) {
                break;
            }
            float k = uWaveL[i].x;
            float amp = uWaveL[i].z;          // 完整振幅（方差与几何无关）
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
            float geom = lod_amp;             // 仍在几何里的比例
            float a = amp * k;
            float w = 0.5 * a * a * (1.0 - geom * geom);
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
            float amp = uWaveDetailL[i].y;    // 完整振幅
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
            float geom = lod_amp;
            float a = amp * k;
            float w = 0.5 * a * a * (1.0 - geom * geom);
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
// need_grad=false ⇒ 不调 cos（grad 输出恒 0）。二分求根只需要 h：
// 每次 WaveSurfaceHit 要评估 14 次，其中一半的三角函数（cos）本来是白算的。
void WaveSampleAt(vec2 p, float t, float footprint, bool need_grad,
                  out float h, out vec2 grad) {
    h = 0.0;
    grad = vec2(0.0);
    if (uWaveCount <= 0) {
        return;
    }

    vec2 flow = WaveFlowAt(p);
    float flen = length(flow);
    vec2 fdir = (flen > 1.0e-6) ? flow / flen : vec2(0.0);
    float blend = (flen > 1.0e-6) ? clamp(uWaveFlowAlign * flen, 0.0, 1.0) : 0.0;
    float amp_gain = 1.0 + uWaveFlowWindGain * clamp(flen, 0.0, 1.0);

    // 域扭曲：整片场沿低频位移 u(p) 平移，θ += k·dot(u, d)。
    vec2 wu, wdux, wduz;
    WaveWarpAt(p, need_grad, wu, wdux, wduz);
    float warp_u_slope = WaveWarpSlopeBound();

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
        float amp = L.z * amp_gain * lod_amp;
        if (amp == 0.0) {
            continue;   // 亚像素（足迹截断）整条消失：省掉下面的 sin/cos
        }
        float warp_scale = (warp_u_slope > 0.0)
                               ? min(1.0, kWarpSlopeLimit / (k * warp_u_slope))
                               : 1.0;
        float wk = k * warp_scale;
        float wx = wk * dot(wdux, d);
        float wz = wk * dot(wduz, d);
        float phase = k * dot(d, p) - omega * t + L.w + wk * dot(wu, d);
        float sin_p = sin(phase);
        float cos_p = need_grad ? cos(phase) : 0.0;
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
void WaveDetailGrad(vec2 p, float t, float footprint,
                    vec2 wu, vec2 wdux, vec2 wduz, out vec2 grad) {
    grad = vec2(0.0);
    if (uWaveDetailCount <= 0) {
        return;
    }
    float warp_u_slope_d = WaveWarpSlopeBound();
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
        float amp = L.y * lod_amp;
        if (amp == 0.0) {
            continue;   // 亚像素（lod_amp=0）或已淡出：省掉下面的 cos
        }
        float omega = L.w;
        float warp_scale = (warp_u_slope_d > 0.0)
                               ? min(1.0, kWarpSlopeLimit / (k * warp_u_slope_d))
                               : 1.0;
        float wk = k * warp_scale;
        float phase = k * dot(d, p) - omega * t + L.z + wk * dot(wu, d);
        vec2 g = vec2(k * d.x, k * d.y) + vec2(wk * dot(wdux, d), wk * dot(wduz, d));
        grad += amp * g * cos(phase);
    }
}

vec3 WaveNormalFromGrad(vec2 grad) {
    return normalize(vec3(-grad.x, 1.0, -grad.y));
}

// 采样射线参数 tt 处的完整法线（主波梯度 + 细节波梯度）。
// ⚠️ **必须与“求交怎么定 t”解耦**：细节波不进求交，主波被足迹截断后它们仍在几何里。
//    旧版把法线求值塞在二分成功之后，一旦走进兜底分支就整片退化成平法线
//    （2026-11-23 报障的「波在固定距离硬消失」）。与 CPU 的 sample_at/finish 同口径。
vec3 WaveNormalAt(vec3 cam, vec3 ray_dir, float tt, float footprint) {
    vec3 p = cam + ray_dir * tt;
    float h;
    vec2 grad;
    WaveSampleAt(p.xz, uWaveTime, footprint, /*need_grad=*/true, h, grad);
    vec2 wu, wdux, wduz;
    WaveWarpAt(p.xz, /*need_grad=*/true, wu, wdux, wduz);
    vec2 dgrad;
    WaveDetailGrad(p.xz, uWaveTime, footprint, wu, wdux, wduz, dgrad);
    return WaveNormalFromGrad(grad + dgrad);
}

// f(t) = (cam.y + rd.y·t − level) − h(p.xz)
float WaveFAt(vec3 cam, vec3 ray_dir, float level, float footprint, float tt) {
    vec3 p = cam + ray_dir * tt;
    float h;
    vec2 grad;
    // 只查高度（grad 丢掉）：省掉每条波一次 cos。
    WaveSampleAt(p.xz, uWaveTime, footprint, /*need_grad=*/false, h, grad);
    return (p.y - level) - h;
}

// 高度上界 ΣA·(1 + wind·|flow|)：二分区间半径的依据（|h| 永远不会超过它）。
// 有效振幅上界 ΣA·lod_amp（**必须和场本身同口径**）。
// ⚠️ 旧版用不衰减的 ΣA：在远处谱已被截断成平面，区间却仍按 ΣA/|rd.y| 撑开，
//    掠射时 span ~ km，12 次二分的量化步长就是几十~几百米 ⇒ 交点 t 在相邻像素间乱跳，
//    而这个 t 会作为透明层深度喂给合成趟去切介质 ⇒ 介质段长度突变 ⇒ 硬边。
float WaveAmplitudeSum(float footprint) {
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

    // 法线 only（debug）：求交仍用平面解，法线照常按梯度求。
    if (uWaveDisplace == 0) {
        pos = cam + ray_dir * t;
        normal = WaveNormalAt(cam, ray_dir, t, footprint);
        return true;
    }

    // span 的上界护栏（1e-3，与足迹护栏**无关**）：|rd.y| 极小的行 span 会爆到公里级，
    // 那里的二分本来就无意义 —— 下一版由 far 分支直接跳过求交（docs/jpov_water_farfield_design.md）。
    float rdy = max(abs(ray_dir.y), 1.0e-3);
    float t_plane = t;
    // 区间半径用**平面交点处的有效振幅**（含足迹截断）：
    // 远处谱已截断成平面 ⇒ span → 0 ⇒ t 连续，不会把量化噪声喂给合成趟的层深。
    vec3 p_plane = cam + ray_dir * t_plane;
    float amp_sum = WaveAmplitudeSum(footprint);
    // 位移波被足迹**全部**截断（浪被抗锯齿杀光）⇒ 曲面就是平面，没有高度修正。
    // 但法线仍必须由还解析得出来的细节波给：旧版在这里直接 return 平法线 (0,1,0)，
    // 等于把远场做成完美镜面，且断点随 kWaveLodPxMin 平移（2 px→80 m、3 px→63 m，
    // report_cam_75m 实测）——这就是 2026-11-23 报障的「波在固定距离硬消失」。
    // 与 CPU 的 IntersectWaveSurface 同口径。
    if (!(amp_sum > 0.0)) {
        pos = p_plane;
        normal = WaveNormalAt(cam, ray_dir, t_plane, footprint);
        return true;
    }
    // 振幅沿射线是常数（谱截断只看像素足迹），所以区间半宽一次算定 ——
    // 旧版这里还有一段"按最近端点重估上界"，那是给 fade 用的，fade 已删。
    float span = amp_sum / rdy;
    float t_lo = max(t_plane - span, 1.0e-4);
    float t_hi = t_plane + span;
    if (t_hi <= t_lo) {
        t_hi = t_lo + 1.0e-4;
    }
    float f_lo = WaveFAt(cam, ray_dir, level, footprint, t_lo);
    float f_hi = WaveFAt(cam, ray_dir, level, footprint, t_hi);
    if ((f_lo > 0.0) == (f_hi > 0.0)) {
        // 数值兜底（极端参数）：t 回退平面解，但**法线照常按梯度求** ——
        // 绝不返回平法线（否则远场变镜面 + 硬分界）。
        t = t_plane;
        pos = cam + ray_dir * t;
        normal = WaveNormalAt(cam, ray_dir, t, footprint);
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
    normal = WaveNormalAt(cam, ray_dir, t, footprint);
    return true;
}
)glsl";

}  // namespace water
}  // namespace jpov

#endif  // JPOV_SRC_WATER_WATER_WAVE_SHADER_H_
