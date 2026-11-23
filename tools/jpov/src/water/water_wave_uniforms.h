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
    // 亚像素细节波的法线是否按足迹截断（默认 true；false 只用于 A/B 对照）。
    bool cull_subpixel = true;
    // 谱截断窗口（px）。默认值 = CPU 的 kWaveLodPxMin / kWaveLodPxMax（3 / 8）。
    // min 越小 ⇒ 波死得越远（远处波纹保留更多），但越容易走样；max 越大 ⇒ 淡出越缓。
    float lod_px_min = 3.0f;
    float lod_px_max = 8.0f;
};

// waves == nullptr ⇒ 全部按"关"上传（uWaveCount = 0）。
// Pre-condition: waves 非空时 num_waves ∈ [0, kMaxWaves]、num_detail_waves ∈
//               [0, kMaxDetailWaves]，且所有 wavelength_m > 0（非法值直接 CHECK）。
void UploadWaterWaveUniforms(ShaderManager& shader_mgr, unsigned int prog,
                             const WaterWaves* waves, const WaveUniformParams& params);

}  // namespace water
}  // namespace jpov

#endif  // JPOV_SRC_WATER_WATER_WAVE_UNIFORMS_H_
