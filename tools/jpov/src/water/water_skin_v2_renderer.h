// JPOV WaterSkinV2Renderer —— 水皮 V2（单像素多法线采样）的**整份** pass
//
// 设计：docs/jpov_water_skin_v2_design.md。定位与契约：
//   · 与 WaterSkinRenderer（V1）**互斥**：一帧只画其中一个（由 WaterWaves::v2 决定）；
//   · 输出的 WBO 三槽语义与 V1 **逐项相同**（accum/reveal/att3），所以合成趟不需要知道
//     画的是哪一版（这也是两者能直接 A/B 的前提）；
//   · 输入契约直接复用 V1 的 `Inputs`（相机/介质/波浪/太阳/探针）—— 面板上的滑条对
//     两版是**同一份**，唯一新增的输入是「每像素采样数 S」。
//
// 为什么要另开一个类而不是在 V1 里加分支（Danis 2026-11-25 定）：
//   V2 要**删掉** V1 的 9 个口径（谱截断 / 方差 / 屏幕导数 / far 分流 / env_lod_cap…），
//   在一个函数里用 if 切这两套逻辑，会让 V1 的注释与不变式失效、也让 V2 永远摆脱不掉
//   那些量的编译期存在。两份 pass 各自独立 ⇒ V1 逐字节不变（gold 兜底）、V2 可以自由改。
//
// 画法：全屏三角形 + FS 里解析求交（同 V1，见 water_skin_shader.h 顶部为什么不用网格）。

#ifndef JPOV_SRC_WATER_WATER_SKIN_V2_RENDERER_H_
#define JPOV_SRC_WATER_WATER_SKIN_V2_RENDERER_H_

#include "tools/jpov/src/water/water_skin_renderer.h"   // Inputs（共享输入契约）
#include "tools/jpov/src/shader_manager.h"
#include "tools/jpov/src/transparent_layer.h"
#include "tools/jpov/src/wbo/wbo_target.h"

namespace jpov {

class WaterSkinV2Renderer {
public:
    // 输入契约与 V1 完全相同（同一份 Camera / 介质 / 波浪 / 太阳 / 探针）。
    // S 是 Draw 的独立参数（不属于"场景输入"）。
    using Inputs = WaterSkinRenderer::Inputs;

    // 每像素法线采样数的合法范围（与 GLSL `kMaxSkinSamples` 一致）。
    // 上限 16：再大就只是在 llvmpipe 上烧时间（docs §5.1 定的滑条范围）。
    static constexpr int kMinSamples = 1;
    static constexpr int kMaxSamples = 16;

    WaterSkinV2Renderer();
    ~WaterSkinV2Renderer();

    WaterSkinV2Renderer(const WaterSkinV2Renderer&) = delete;
    WaterSkinV2Renderer& operator=(const WaterSkinV2Renderer&) = delete;

    // Pre-condition: shader_mgr != nullptr；GL context 已激活。
    void Init(ShaderManager* shader_mgr);

    // 释放 WBO 附件 / VAO。可重复调用（须在 GL context 存活时）。
    void Finalize();

    // 建/清本帧的 WBO 目标（尺寸变化会重建）。不画东西。
    // Pre-condition: 已 Init；full_w/full_h > 0。
    void Begin(int full_w, int full_h);

    // 画水面到 WBO 目标（只写层，不碰 HDR 颜色）。
    // Pre-condition: Begin 已调用；inputs.scene_depth_tex 非 0；
    //               camera.fbo_3d_* 与 full_w/h 一致；sample_count ∈ [kMinSamples, kMaxSamples]；
    //               `inputs.waves == nullptr || inputs.waves->v2`（谱口径必须与天球折射同值，
    //               否则 Snell 窗缘错位 —— 见 water_wave_uniforms.h 顶部的不变式）。
    void Draw(const Inputs& inputs, int sample_count);

    // 本帧这一层的三张附件（交给合成器的 TransparentLayerSet）。
    // 未 Begin / 没画过内容时返回的层 invalid()。
    TransparentLayer layer() const;

private:
    void EnsureTarget(int w, int h);
    void UploadCommonUniforms(unsigned int prog, const Inputs& inputs, int sample_count);
    // 按「水上/水下 × 生产/调试」选 program（水上水下由相机 y vs level 决定，每帧常量）。
    unsigned int SelectProgram(const Inputs& inputs) const;

    ShaderManager* shader_mgr_ = nullptr;
    unsigned int prog_above_ = 0;         // 水上 · 生产
    unsigned int prog_below_ = 0;         // 水下 · 生产
    unsigned int prog_above_debug_ = 0;   // 水上 · 调试
    unsigned int prog_below_debug_ = 0;   // 水下 · 调试
    unsigned int vao_ = 0;
    WboTarget wbo_;
    bool has_content_ = false;
    bool initialized_ = false;
};

}  // namespace jpov

#endif  // JPOV_SRC_WATER_WATER_SKIN_V2_RENDERER_H_
