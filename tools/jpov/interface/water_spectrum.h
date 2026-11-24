// 水面波浪的**谱**（WaterWaves）——“展开后的形式”，不是用户接口。
//
// 分层（2026-11-20 定，2026-11-24 收口）：
//   WaterConfig（艺术/物理层，用户只碰它，见 water_config.h）
//        │  BuildWater()
//        ▼
//   WaterWaves（本文件：8 主波 + 64 细节 + 短波 MSS）  ← 渲染器内部单位
//        │  每帧上传 uniform
//        ▼
//   GLSL（src/water/water_wave_shader.h）
//
// 为什么单独一个头：`RenderCommandList`（render_command.h）要对调用方暴露的是
// **WaterConfig**（风/涌/水色…），不是这张波表；把波表拆出来之后
//   water_spectrum.h  ←  water_config.h  ←  render_command.h
// 这条依赖链是单向的，不会成环（以前 WaterConfig 反过来 include render_command.h）。

#ifndef JPOV_INTERFACE_WATER_SPECTRUM_H_
#define JPOV_INTERFACE_WATER_SPECTRUM_H_

#include <cstdint>

namespace jpov {

// ── 水面波浪（法线扰动 / 高度置换）──
//
// 纯高度场 Gerstner 谱（**无水平位移/choppy** —— 求交必须是单值高度图）：
//   h(p, t) = Σ A_i · sin(k_i·dot(dir_i, p.xz) − ω_i·t + φ_i)
//   水面世界高度 = level_y + h；法线 = normalize(−∂h/∂x, 1, −∂h/∂z)。
//   深水色散 ω = sqrt(g·k)（g = 9.81），所以「波长」决定传播速度，不另给周期。
//
// `enabled = false` 或 `num_waves = 0` ⇒ h ≡ 0 ⇒ 与旧的纯平面水皮**逐字节一致**。
struct WaterWave {
    // 传播方向（水平单位向量，xz 平面）。只取 xz，y 由调用方保证为 0。
    float dir_x = 1.0f;
    float dir_z = 0.0f;
    // 波长（米，> 0）。决定波数 k = 2π/λ 与色散速度。
    float wavelength_m = 10.0f;
    // 振幅（米，>= 0）。
    float amplitude_m = 0.1f;
    // 初相（弧度）。
    float phase_rad = 0.0f;
};

// 波浪谱 + 大尺度流向（flow map）配置。
// 与 src/water/water_wave_math.h（CPU 参考 + GLSL 同源）一一对应。
struct WaterWaves {
    // false（默认）= 不启用：零回归。
    bool enabled = false;

    // 波数上限（与 GLSL uniform 数组长度一致）。
    static constexpr int kMaxWaves = 8;

    // 实际使用的波数 [0, kMaxWaves]。0 视同关闭。
    int num_waves = 0;

    // 波表。只有前 num_waves 项有效。
    WaterWave waves[kMaxWaves];

    // true（默认）= 真位移（求交沿高度场）；false = **法线 only**（求交仍用平面，
    // 只把法线换成高度场的梯度）。后者物理上不一致，**只用于 debug 二分**：
    // 用来判断「噪点」是求交引起的还是法线/着色引起的。
    bool displacement = true;

    // debug：0 = 正常；1 = 把交点法线编码成颜色输出（N*0.5+0.5）。
    // 只用于定位「法线噪点」，不属于产品配置。
    int debug = 0;

    // ── 诊断：逐成分开关（bisect 用，2026-11-24）─────────────────
    // 高度场/法线/着色由若干**独立成分**叠加而成；出"硬边/噪点"时按位剥离，看哪一步出现。
    //  bit0 主波高度（sin 位移）      bit1 主波法线（梯度）
    //  bit2 细节短波法线（64 条）    bit3 域扭曲 warp（只改相位）
    //  bit4 二阶 Stokes 谐波         bit5 短波 MSS（只进 var_p/粗糙度）
    //  bit6 探针反射（天光）         bit7 太阳的像（解析 GGX）
    //  bit8 F_eff（粗糙反射率重分配；关掉=平面菲涅尔）
    // 0xFFFFFFFF = 全开（默认，产品行为）。
    uint32_t components = 0xFFFFFFFFu;
    // >= 0 ⇒ **只保留这一条主波**（其余主波清零；细节波不受影响）。
    // 用来把"8 条波叠加"退化成单波，隔离出是哪条波/哪个方向造的边。
    int solo_wave = -1;

    // ── 大尺度流向（flow map）──
    // 一张 RG 纹理：uv = p.xz * flow_uv_scale，rg ∈ [0,1] 解码成 (rg−0.5)*2。
    // texture_id = 0 ⇒ 无 flow（各向同性谱）。
    uint32_t flow_texture_id = 0;      // TextureManager id；0 = 无 flow
    float flow_uv_scale = 1.0f / 200.0f;  // 世界米 → uv（默认一张纹理覆盖 200 m）
    float flow_strength = 1.0f;        // flow 向量整体增益（|flow| ≤ strength）
    // 波方向朝 flow 对齐的强度 [0,1]：0 = 不调方向，1 = 完全顺流。
    float flow_align = 0.5f;
    // flow 强度对振幅的增益：A *= (1 + flow_wind_gain·|flow|)。
    float flow_wind_gain = 1.0f;

    // 没有任何「按距离淡出」参数（2026-11-24 删除）：远处怎么变平由像素足迹的谱截断
    // 决定（λ/footprint 落到 3 px 以下就消失，方差转进 BRDF）。距离上的硬性压平既没有
    // 物理依据，也掩盖了「远场不该走求交」这个事实（见 docs/jpov_water_farfield_design.md）。

    // ── 大尺度相位扭曲（domain warp）──
    // 把整片波场按低频位移场 u(p) 平移：θ_i += k_i·dot(u, d_i)，u = Σ_j A_j·dir_j·sin(...)。
    // 为什么要它：h 是**平面波之和**，每条波峰都是无限长直线，4 个平面波的干涉必然是
    // 晶格（“周期性花纹”）。纯靠加波数只能把晶格变密，不能消掉。低频位移让波峰弯曲/
    // 分叉/长度不一，这是**唯一**在不引入 choppy 的前提下打破平面波特性的手段。
    // 仍然是解析、单值高度场：求交结构一行不用改，|h| ≤ ΣA 也不变。
    // warp_amplitude_m = 0 ⇒ 关（默认，逐字节旧行为）。
    float warp_amplitude_m = 0.0f;
    // 低频载波（最多 4 条）：波长（米，> 0）与方向（xz，单位向量）。
    // 4 条时波长取不可公比（37/64/112/197 m）⇒ 位移场本身接近噪声，不再是
    // 「一个大格子掰弯小格子」（2 条载波时仍会看出单一尺度的起伏）。
    static constexpr int kWarpCarriers = 4;
    float warp_wavelength_m[kWarpCarriers] = {37.0f, 64.0f, 112.0f, 197.0f};
    float warp_dir_x[kWarpCarriers] = {0.83f, -0.47f, 0.35f, -0.92f};
    float warp_dir_z[kWarpCarriers] = {0.56f, 0.88f, 0.94f, -0.39f};
    // 每条载波的初相（弧度）。不同场景给不同值即可错开图案。
    float warp_phase_rad[kWarpCarriers] = {0.9f, 2.7f, 4.4f, 1.6f};

    // ── 二阶 Stokes 谐波（深水，尖峰平谷）──
    // h_i += −½·k_i·A_i²·cos(2θ_i)。真实风浪的非线性最低阶修正：峰更尖、谷更平，
    // 且峰谷不再对称（线性叠加的偏度恒为 0）。解析、单值 ⇒ 求交结构不变；
    // 用已有 sinθ/cosθ 的双角恒等式求值 ⇒ **0 次额外三角函数**。
    // 代价：求交区间上界从 ΣA 变成 Σ(A + ½kA²)（陡度 0.07 时约 +3.5%）。
    bool stokes_second_order = false;

    // ── 细节短波（只进法线，不进几何）──
    // 与 `short_wave`（只进 MSS→粗糙度）正交：这里的波**真的改法线**，
    // 但**不参与高度/求交**（求交只走 waves[]，否则二分成本 ×N）。
    // 用途：近场 0.3~2 m 的 chop —— 照片里的“棱角/碎光”主要来自这一档。
    // 被像素足迹截断的部分照旧折进斜率方差（WaveFilteredVariance），不会丢能量。
    static constexpr int kMaxDetailWaves = 64;
    int num_detail_waves = 0;
    WaterWave detail_waves[kMaxDetailWaves];

    // ── 短波（毛细波/短重力波）────────────────────────────────────────
    // 它们在任意可见距离上都被像素吞掉 ⇒ **不进几何**（不改 displacement / 法线 / 求交），
    // 只贡献斜率方差。配置 = 2D 风向 + 风速（MSS 由 Cox–Munk 实测关系给出）。
    // 本 PR 只有它参与产品逻辑：MSS → 反射率/透射率重分配（F_eff）。
    struct ShortWave {
        float dir_x = 1.0f;          // 风向（xz，单位）；0,0 ⇒ 退回各向同性
        float dir_z = 0.0f;
        float wind_speed_ms = 0.0f;  // 10 m 高处风速（m/s）；0 = 不加短波
    };
    ShortWave short_wave;
};

}  // namespace jpov

#endif  // JPOV_INTERFACE_WATER_SPECTRUM_H_
