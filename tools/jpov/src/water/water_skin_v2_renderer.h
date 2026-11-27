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

// CSM 阴影输入（与 StripCsmShadow 同构；由 Renderer 填 —— 阴影 pass 属于主链路，
// 水皮只是消费者，不该反过来持有 shadow_fbos_ / 级联矩阵）。
//
// 语义：只有 `sun` 有值且 fbos 非空时**可能**有阴影；具体吃不吃由 `uHasCsm` 决定。
// 水皮是纯镜面（无 diffuse albedo）⇒ 阴影只作用在**太阳的像**上：阴影里的水面不该有
// 太阳闪点（船影/立柱影会切断高光带）。环境反射（天光探针）不吃阴影。
struct WaterCsmShadow {
    std::optional<DirectionalLight> sun;
    ShadowConfig cfg{};
    std::vector<CascadeFBO> fbos;      // 长度 = cfg.cascade_count（仅 sun 时有效）
    float vp[ShadowConfig::kMaxCascades][16] = {};
    float depth_vp[ShadowConfig::kMaxCascades][16] = {};
    float texel_world[ShadowConfig::kMaxCascades] = {};

    bool IsValid() const { return sun.has_value() && !fbos.empty(); }
};

// ── 水皮 SSR 的 Query 产物（2026-11-27）──
// 本类所有（懒创建、尺寸变化才重建），只借给 Renderer 转成 SsrChain 的 SsrQuery。
// 三个字段的语义与 `SsrQuery` 逐条对应（见 ssr_chain.h），但**本类不认识 SSR**
// （不 include 装置的接口）：水皮只负责"自己的表面长什么样"。
struct WaterSsrQueryTextures {
    unsigned int depth_tex = 0;   // R32F：水面窗口深度（1.0 = 这一像素不查询）
    unsigned int normal_tex = 0;  // RGBA16F：**视空间**中心法线，a ≥ 0.25 = 有水面
    unsigned int rough_tex = 0;   // RGBA16F：只读 .a = roughness
    int width = 0;
    int height = 0;
};

// query 趟的三个换算量（属于**装置**的口径，由 Renderer 从 SsrConfig 取）。
// 放这里而不是放进 Inputs：它们只影响"如何向装置描述自己"，不影响水皮出图。
struct WaterSsrQueryTuning {
    float resolution_scale = 0.5f;    // 1/N（> 0），装置的降采样倍率
    float roughness_to_lod = 8.0f;    // > 0，装置的 roughness → mip 级斜率
    float rough_max = 0.35f;          // (0, 1]，rough 上限（≈ 探针 LOD cap）
    // 谱截断乘数（波长 < 3T~8T 像素的波**不进 query 法线**；0 = 不截断）。
    // 见 WaterCommand::ssr_normal_truncate_px 的注释：这是倒影噪点的直接旋钮。
    float normal_truncate_px = 0.0f;
    // 1 = 把 query 法线魔改到"反射方向 ≥ sin1°"（与着色趟 env 的钳位同口径）。
    // 关掉 = 逐字用中心法线（A/B 用；关掉时会有 10%~20% 的射线指到水面以下）。
    bool clamp_normal_above_horizon = true;
};

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
    void Draw(const Inputs& inputs, int sample_count, const WaterCsmShadow* csm = nullptr);

    // 跑 SSR 的 **query 趟**（只算几何：窗口深度 + 视空间中心法线 + rough）。
    // 必须在本帧的 Draw 之前调用，且两次传**同一份 Inputs**（几何口径必须一致）。
    // 产物是本类所有的三张全分辨率纹理（见 query()），生命周期到下一次 DrawQuery。
    // Pre-condition: 已 Init；full_w/full_h 与 Begin 一致；tuning 各字段在范围内。
    void DrawQuery(const Inputs& in, const WaterSsrQueryTuning& tuning);

    // 上一次 DrawQuery 的产物；没跑过 ⇒ 全 0（调用方据此跳过装置）。
    WaterSsrQueryTextures query() const { return query_; }

    // 本帧这一层的三张附件（交给合成器的 TransparentLayerSet）。
    // 未 Begin / 没画过内容时返回的层 invalid()。
    TransparentLayer layer() const;

private:
    void EnsureTarget(int w, int h);
    // query 趟的三附件目标（R32F 深度 / RGBA16F 法线 / RGBA16F rough）。懒创建。
    void EnsureQueryTarget(int w, int h);
    void DestroyQueryTarget();
    // truncate_override_px > 0 ⇒ 覆盖波浪谱自带的截断乘数（只给 query 趟用：
    // 着色趟必须与天球折射同口径，query 趟刻意用更强的截断压掉亚像素噪声）。
    void UploadCommonUniforms(unsigned int prog, const Inputs& in, int sample_count,
                              const WaterCsmShadow* csm, float truncate_override_px = 0.0f);
    // 按「水上/水下 × 生产/调试」选 program（水上水下由相机 y vs level 决定，每帧常量）。
    unsigned int SelectProgram(const Inputs& inputs) const;

    ShaderManager* shader_mgr_ = nullptr;
    unsigned int prog_query_ = 0;         // SSR query 趟（纯几何）
    unsigned int prog_above_ = 0;         // 水上 · 生产
    unsigned int prog_below_ = 0;         // 水下 · 生产
    unsigned int prog_above_debug_ = 0;   // 水上 · 调试
    unsigned int prog_below_debug_ = 0;   // 水下 · 调试
    unsigned int vao_ = 0;
    WboTarget wbo_;
    // SSR query 三附件（本类所有）
    unsigned int query_fbo_ = 0;
    WaterSsrQueryTextures query_;
    bool has_content_ = false;
    bool initialized_ = false;
};

}  // namespace jpov

#endif  // JPOV_SRC_WATER_WATER_SKIN_V2_RENDERER_H_
