// 波浪谱 → GLSL uniform 的**唯一一份**上传实现。
//
// 消费者有两个，且**必须完全一致**：
//   · 水皮（water_skin_renderer）—— 界面几何 / 法线 / 菲涅尔；
//   · 天球折射（sky_renderer）  —— 水下 Snell 窗边缘（调同一个 WaveSurfaceHit）。
// 两趟拿到的谱只要差一个字段（时间 / 深度 / warp / Stokes / 细节短波），窗缘就会
// 用两套几何 ⇒ 历史上出现过「窗缘漏出一块天空色」（俗称"漏牙龈"）。
//
// 2026-11-20 的教训：这两份上传代码原本各写一遍，水皮加了 warp / Stokes / 细节短波，
// 天球那份没跟上 ⇒ 天球那边 `uWaveStokes2 = 0`、`uWaveDetailCount = 0`（默认 0）。
// 所以现在抽成一份：**加字段只改这里**。

#ifndef JPOV_SRC_WATER_WATER_WAVE_UNIFORMS_H_
#define JPOV_SRC_WATER_WATER_WAVE_UNIFORMS_H_

#include "tools/jpov/interface/render_command.h"

namespace jpov {

class ShaderManager;

namespace water {

// 非谱字段（谱本身在 WaterWaves 里）。默认值 = 水皮的正常渲染口径。
struct WaveUniformParams {
    float time_s = 0.0f;                  // 唯一的水面时钟（WaterCommand::time_s）
    float depth_m = 300.0f;               // 平底深度（有限水深色散）
    int debug = 0;                        // 水皮 debug 档（天球固定 0）
    bool displacement = true;             // false = 法线 only（debug 二分用）
    bool has_flow_tex = false;            // 只有水皮会绑 flow map

    // ── 谱截断的两个参数（默认 = V1 / 旧行为）──
    // 表内 72 条波是否**全部参与**（不做按足迹的谱截断）。
    // V2 水皮在 T=0 时传 true；V1 与天球默认 false。
    // ⚠️ 天球折射传的必须是**同一值**（= WaterWaves::v2）：谱口径不一致会让 Snell 窗缘
    //    用另一套几何 ⇒ 窗缘漏色（见本文件顶部）。请用 ResolveWaveTruncation() 一次算出来。
    bool no_truncation = false;
    // 截断阈值对 (3, 8 px) 的**乘数** T（见 water_wave_shader.h::uWaveTruncatePx）。
    // 1.0（默认）= V1 同一条线（乘 1.0 是 IEEE 精确的 ⇒ 逐字节不变）。
    float truncate_px = 1.0f;
    // 相位局部原点（xz，米）：相位按 `p − origin` 求，初相在这里预算进 k·dot(d, origin)。
    // 只是 float 精度手段（相机离世界原点很远时相位不被大数吃低位）；
    // 0,0（默认）= 与旧版**逐位相同**。由 water::WavePhaseOriginM() 从相机位置算。
    float origin_x = 0.0f;
    float origin_z = 0.0f;
};

// 谱截断口径的**唯一出处**：波浪谱配置 → 上传参数。
//   V1（v2 = false）        : 3~8 px（no_truncation = false, T = 1）
//   V2 + T <= 0             : 不截断
//   V2 + T > 0              : 3T~8T px
// 水皮与天球折射**必须**都调这个函数（两边手写会导致 Snell 窗缘用另一套几何，漏色）。
struct WaveTruncation {
    bool no_truncation = false;
    float truncate_px = 1.0f;
};
inline WaveTruncation ResolveWaveTruncation(const WaterWaves& waves) {
    WaveTruncation t;
    if (!waves.v2) {
        return t;   // V1：3~8 px
    }
    if (!(waves.v2_truncation_px > 0.0f)) {
        t.no_truncation = true;   // V2 + T=0：全采
        return t;
    }
    t.truncate_px = waves.v2_truncation_px;
    return t;
}

// waves == nullptr ⇒ 全部按"关"上传（uWaveCount = 0）。
// Pre-condition: waves 非空时 num_waves ∈ [0, kMaxWaves]、num_detail_waves ∈
//               [0, kMaxDetailWaves]，且所有 wavelength_m > 0（非法值直接 CHECK）。
void UploadWaterWaveUniforms(ShaderManager& shader_mgr, unsigned int prog,
                             const WaterWaves* waves, const WaveUniformParams& params);

}  // namespace water
}  // namespace jpov

#endif  // JPOV_SRC_WATER_WATER_WAVE_UNIFORMS_H_
