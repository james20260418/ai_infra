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

    // ── 谱截断的两个乘数（默认 1/1 = V1 同一条线；<= 0 = 该族不截断）──
    // **主波与细节波分开**（Danis 2026-11-26）：两者的角色不同（主波还管几何/剪影，
    // 细节波只进法线），噪点归因必须能分开；一个旋钮一起动就分不清是谁。
    // ⚠️ 天球折射传的必须是**同一对值**（= WaterWaves::v2_*）：谱口径不一致会让 Snell
    //    窗缘用另一套几何 ⇒ 窗缘漏色（见本文件顶部）。请用 ResolveWaveTruncation() 一次算出来。
    float truncate_px_main = 1.0f;
    float truncate_px_detail = 1.0f;
    // 相位局部原点（xz，米）：相位按 `p − origin` 求，初相在这里预算进 k·dot(d, origin)。
    // 只是 float 精度手段（相机离世界原点很远时相位不被大数吃低位）；
    // 0,0（默认）= 与旧版**逐位相同**。由 water::WavePhaseOriginM() 从相机位置算。
    float origin_x = 0.0f;
    float origin_z = 0.0f;
};

// 谱截断口径的**唯一出处**：波浪谱配置 → 上传参数。
//   V1（v2 = false）        : 主波 / 细节波都 3~8 px（T = 1/1）
//   V2 + 某一族 T <= 0      : 该族不截断
//   V2 + T > 0              : 3T~8T px（主波、细节波各自独立）
// 水皮与天球折射**必须**都调这个函数（两边手写会导致 Snell 窗缘用另一套几何，漏色）。
struct WaveTruncation {
    float truncate_px_main = 1.0f;
    float truncate_px_detail = 1.0f;
};
inline WaveTruncation ResolveWaveTruncation(const WaterWaves& waves) {
    WaveTruncation t;
    if (!waves.v2) {
        return t;   // V1：主波 / 细节波都走 3~8 px
    }
    t.truncate_px_main = waves.v2_truncate_px_main;
    t.truncate_px_detail = waves.v2_truncate_px_detail;
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
