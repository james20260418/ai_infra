#define GL_GLEXT_PROTOTYPES

#include "tools/jpov/src/water/water_wave_uniforms.h"

#include <GL/gl.h>
#include <GL/glext.h>

#ifdef _WIN32
#include "third_party/gl_loader-mingw/gl_loader.h"
#endif

#include <algorithm>
#include <cmath>

#include <glog/logging.h>

#include "tools/jpov/src/shader_manager.h"
#include "tools/jpov/src/water/water_wave_math.h"

namespace jpov {
namespace water {
namespace {

constexpr float kTwoPi = 6.28318530717958647692f;

// 非法数值直接 crash：NaN/Inf 会经 sin/cos 污染整片法线（画面变黑/花屏但不报错），
// 比"静默画错"更难查，所以在这里一次性拦住。
void CheckWaveFinite(const WaterWave& wv, const char* what) {
    CHECK(std::isfinite(wv.wavelength_m)) << what << ".wavelength_m 必须是有限值";
    CHECK(std::isfinite(wv.amplitude_m)) << what << ".amplitude_m 必须是有限值";
    CHECK(std::isfinite(wv.phase_rad)) << what << ".phase_rad 必须是有限值";
}

// 方向归一化：零向量（配置忘填）回落到 +x，而不是产生 0/0。
void NormalizedDir(float dx, float dz, float* out_x, float* out_z) {
    const float len = std::sqrt(dx * dx + dz * dz);
    if (len > 1.0e-6f) {
        *out_x = dx / len;
        *out_z = dz / len;
    } else {
        *out_x = 1.0f;
        *out_z = 0.0f;
    }
}

}  // namespace

void UploadWaterWaveUniforms(ShaderManager& shader_mgr, unsigned int prog,
                             const WaterWaves* w, const WaveUniformParams& p) {
    const bool on = (w != nullptr) && w->enabled && w->num_waves > 0;
    if (on) {
        // 时间/深度是**每帧输入**，不校验的话 NaN 时间会一路变成 sin(NaN)=NaN：
        // 画面退化成平面（二分判不出变号）而不报错 —— 和 plate2d 的 time_seconds 同一条规矩。
        CHECK(std::isfinite(p.time_s)) << "WaveUniformParams.time_s 必须是有限值";
        CHECK_GT(p.depth_m, 0.0f)
            << "WaveUniformParams.depth_m 必须 > 0（<=0 会静默按深水算，掩盖调用方错误）";
    }
    const int count = on ? std::min(w->num_waves, WaterWaves::kMaxWaves) : 0;
    glUniform1i(shader_mgr.GetUniform(prog, "uWaveCount"), count);

    // 波表先算好 k / ω 再上传：GPU 不重算色散（与 CPU 参考同一口径）。
    // ⚠️ 逐个 glUniform4f/2f 上传，不用 glUniform4fv/2fv —— MinGW 的 gl_loader 只导出了
    //    标量版本（数组版本 undefined reference，Windows 交叉编译直接链失败）。
    //    GL 保证数组 uniform 的 location 连续 ⇒ loc+i 就是第 i 个元素。
    const int wave_l_loc = shader_mgr.GetUniform(prog, "uWaveL[0]");
    const int wave_dir_loc = shader_mgr.GetUniform(prog, "uWaveDir[0]");
    for (int i = 0; i < count; ++i) {
        const WaterWave& wv = w->waves[i];
        CHECK_GT(wv.wavelength_m, 0.0f) << "WaterWave.wavelength_m 必须 > 0";
        CHECK_GE(wv.amplitude_m, 0.0f) << "WaterWave.amplitude_m 必须 >= 0";
        CheckWaveFinite(wv, "WaterWave");
        const float k = kTwoPi / wv.wavelength_m;
        glUniform4f(wave_l_loc + i, k, DispersionOmega(k, p.depth_m), wv.amplitude_m,
                    wv.phase_rad);
        float dx = 0.0f;
        float dz = 0.0f;
        NormalizedDir(wv.dir_x, wv.dir_z, &dx, &dz);
        glUniform2f(wave_dir_loc + i, dx, dz);
    }

    glUniform1f(shader_mgr.GetUniform(prog, "uWaveTime"), on ? p.time_s : 0.0f);

    // ── 域扭曲（domain warp）：4 条低频载波 → (k, dir_x, dir_z, phase) ──
    const float warp_amp = on ? w->warp_amplitude_m : 0.0f;
    glUniform1f(shader_mgr.GetUniform(prog, "uWaveWarpAmp"), warp_amp > 0.0f ? warp_amp : 0.0f);
    const int warp_loc = shader_mgr.GetUniform(prog, "uWaveWarp[0]");
    for (int j = 0; j < WaterWaves::kWarpCarriers; ++j) {
        if (warp_amp <= 0.0f) {
            glUniform4f(warp_loc + j, 1.0f, 1.0f, 0.0f, 0.0f);
            continue;
        }
        CHECK_GT(w->warp_wavelength_m[j], 0.0f)
            << "WaterWaves.warp_wavelength_m 必须 > 0（关掉请用 warp_amplitude_m = 0）";
        float dx = 0.0f;
        float dz = 0.0f;
        NormalizedDir(w->warp_dir_x[j], w->warp_dir_z[j], &dx, &dz);
        glUniform4f(warp_loc + j, kTwoPi / w->warp_wavelength_m[j], dx, dz,
                    w->warp_phase_rad[j]);
    }
    glUniform1i(shader_mgr.GetUniform(prog, "uWaveStokes2"),
                (on && w->stokes_second_order) ? 1 : 0);

    // ── 细节短波：只进法线（不进几何/求交）──
    // .w = ω：色散在 CPU 算一次（与主波同一份 DispersionOmega），GPU 不重算
    //（逐像素 ×64 次 sqrt 是不必要的；更要紧的是与 CPU 参考必须同一个 ω）。
    glUniform1i(shader_mgr.GetUniform(prog, "uWaveCullSubpixel"), p.cull_subpixel ? 1 : 0);
    // 谱截断窗口：min 必须 < max（否则 smoothstep 分母为 0）。
    const float lod_min = std::max(p.lod_px_min, 1.0f);
    const float lod_max = std::max(p.lod_px_max, lod_min + 0.5f);
    glUniform1f(shader_mgr.GetUniform(prog, "uWaveLodPxMin"), lod_min);
    glUniform1f(shader_mgr.GetUniform(prog, "uWaveLodPxMax"), lod_max);
    const int detail_count =
        on ? std::clamp(w->num_detail_waves, 0, WaterWaves::kMaxDetailWaves) : 0;
    glUniform1i(shader_mgr.GetUniform(prog, "uWaveDetailCount"), detail_count);
    const int detail_l_loc = shader_mgr.GetUniform(prog, "uWaveDetailL[0]");
    const int detail_dir_loc = shader_mgr.GetUniform(prog, "uWaveDetailDir[0]");
    for (int i = 0; i < detail_count; ++i) {
        const WaterWave& dv = w->detail_waves[i];
        CHECK_GT(dv.wavelength_m, 0.0f) << "detail_waves.wavelength_m 必须 > 0";
        CHECK_GE(dv.amplitude_m, 0.0f) << "detail_waves.amplitude_m 必须 >= 0";
        CheckWaveFinite(dv, "detail_waves");
        const float k = kTwoPi / dv.wavelength_m;
        glUniform4f(detail_l_loc + i, k, dv.amplitude_m, dv.phase_rad,
                    DispersionOmega(k, p.depth_m));
        float dx = 0.0f;
        float dz = 0.0f;
        NormalizedDir(dv.dir_x, dv.dir_z, &dx, &dz);
        glUniform2f(detail_dir_loc + i, dx, dz);
    }

    // 远处的两个**正交**旋钮（互不依赖，任意组合）：
    //   ① 掠射足迹护栏 graze_floor —— 控制远处「还有多少波活着」（大 = 保细节 / 小 = 早变平）；
    //   ② 距离淡出 fade_start/end —— end <= start ⇒ GLSL 侧恒 1（= 关闭）。
    // 护栏 clamp 到合法区间的责任在这里（CPU WaveFootprintM 里 clamp 同一区间 = 同口径）。
    glUniform1f(shader_mgr.GetUniform(prog, "uWaveFadeStart"), on ? w->fade_start_m : 0.0f);
    glUniform1f(shader_mgr.GetUniform(prog, "uWaveFadeEnd"), on ? w->fade_end_m : 0.0f);
    glUniform1f(shader_mgr.GetUniform(prog, "uWaveGrazeFloor"),
                on ? std::clamp(w->graze_floor, kGrazeFloorPhysical, kGrazeFloorMax)
                   : kGrazeFloorPhysical);
    glUniform1f(shader_mgr.GetUniform(prog, "uWaveFlowAlign"), on ? w->flow_align : 0.0f);
    glUniform1f(shader_mgr.GetUniform(prog, "uWaveFlowWindGain"),
                on ? w->flow_wind_gain : 0.0f);
    // 没有绑 flow 纹理时强度/尺度一律 0（WaveFlowAt 本来就会因 uWaveHasFlow=0 提前返回，
    // 这里把数值也置 0，免得以后有人只看 uniform 值时被误导）。
    const bool flow_on = on && p.has_flow_tex;
    glUniform1f(shader_mgr.GetUniform(prog, "uWaveFlowStrength"),
                flow_on ? w->flow_strength : 0.0f);
    glUniform2f(shader_mgr.GetUniform(prog, "uWaveFlowUvScale"),
                flow_on ? w->flow_uv_scale : 0.0f, flow_on ? w->flow_uv_scale : 0.0f);
    // debug 档与"有没有波"无关（debug=3 是"跳过深度拒绝"，波浪关掉时也要生效）。
    glUniform1i(shader_mgr.GetUniform(prog, "uWaveDebug"), p.debug);
    glUniform1i(shader_mgr.GetUniform(prog, "uWaveDisplace"),
                (on && p.displacement) ? 1 : 0);
    glUniform1i(shader_mgr.GetUniform(prog, "uWaveHasFlow"), flow_on ? 1 : 0);

    // 短波：只进方差（不进几何）。方向与风速由谱给，MSS 用 Cox–Munk 现场算。
    const float swx = on ? w->short_wave.dir_x : 0.0f;
    const float swz = on ? w->short_wave.dir_z : 0.0f;
    const float swwind = on ? w->short_wave.wind_speed_ms : 0.0f;
    glUniform2f(shader_mgr.GetUniform(prog, "uShortWaveDir"), swx, swz);
    glUniform1f(shader_mgr.GetUniform(prog, "uShortWaveMss"), CoxMunkMeanSquareSlope(swwind));
}

}  // namespace water
}  // namespace jpov
