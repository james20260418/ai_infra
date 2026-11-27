// JPOV SSR —— 屏幕空间反射**装置**（即插即用的 util）
//
// ───────────────────────────────────────────────────────────────────────────
// 它是什么
// ───────────────────────────────────────────────────────────────────────────
// 一个长生命周期的类，**只认两样输入、只给一样输出**：
//
//     Source（被反射的世界）  ┐
//                             ├──>  SsrChain  ──> 全分辨率反射图（辐亮度 + 覆盖率）
//     Query （谁在问）        ┘
//
// 内部依次跑：降采样 → 同面并集柱体 → march/resolve → 粗糙度预滤波（mip 链）→ 升采样。
// 五步全封装，调用方看不到中间任何一张缓冲。
//
// ── 位置无关（这是这次重构的全部理由）──
// 本类**不知道**自己在渲染链路的哪个位置，代码里也不该看出位置：
//   · 它不读环境探针、不乘 Fresnel/AO、不写任何 HDR 目标 —— "怎么用这份结果"是消费端的事；
//   · 它不假设 Source 与 Query 是同一份数据，也不假设 Query 是不透明的；
//   · 输入输出都是**全分辨率屏幕图**（不是某个 pass 的 FBO），所以位置可以是
//     「不透明 3D → 不透明合成」（实心地板/人身上的镜面），也可以是
//     「不透明 3D → 水皮/其它透明层」（水面上按波浪扰动后的法线做反射）。
//
// 于是有两个消费者：
//   · 不透明场景（现役）：ssr_opaque_reflection.h —— 它还负责属性 pass 与"用 SSR 替换
//     环境镜面项"的合成公式（那是**位置知识**，刻意留在装置之外）。
//   · 水皮（下一个）：自己写一份"源 = 不透明场景颜色/深度，查询 = 水面位置/波浪法线/粗糙度"，
//     拿到反射图后按自己的介质/菲涅尔去混。
//
// ───────────────────────────────────────────────────────────────────────────
// Source / Query 的确切含义（这一层必须说死，否则每次调用都要重新猜）
// ───────────────────────────────────────────────────────────────────────────
// Source —— "屏幕上有哪些东西可以被反射" + "它们长什么样"：
//   color  : 命中时取的颜色（**纯辐亮度**：未加雾、未 tonemap、未乘任何材质项）
//   depth  : **窗口深度**（gl_FragCoord.z，1 = 天空/无几何）—— 装置自己换算成 1/z
//   normal : 命中面的视空间法线（只用于"射线是不是扎进这张面"的朝向判据）
//            .a ≥ 0.25 = 这一格有法线数据（没有就不判朝向，维持旧行为）
//
// Query —— "哪些像素要反射、它们的表面长什么样"：
//   depth  : **窗口深度**（反射面自己的深度，1 = 这一像素不查询）
//   normal : 反射面的视空间法线（rgb 三分量）。.a ≥ 0.25 = 这一像素参与查询
//   rough  : 逐像素 roughness，**只读 .a**（与属性纹理附件 1 同构，可以直接传它）
//
// 两者必须**同分辨率、同一相机、逐像素同屏**（屏幕参数化靠这个前提）；
// 实心→实心时把同一批纹理传两遍即可（内部会各跑一趟降采样，结果逐字节相同）。
//
// ───────────────────────────────────────────────────────────────────────────
// 输出
// ───────────────────────────────────────────────────────────────────────────
//   reflection_tex（全分辨率 RGBA32F）：rgb = 命中辐亮度（未乘材质项），a = 覆盖率 A
//   backface_tex  （全分辨率 R8UI）     ：bit0 = 这个像素的答案来自"背面命中"
//   a == 0 ⇒ 没有可用结果，rgb 无意义（消费端退回自己的兜底，通常是 env 探针）
//
// 消费端的标准式子（不透明场景就是这样用的）：
//     color += mask · ao · (A·L·weight(F0, rough) − A·env_term(F0, rough))
// 其中 mask/材质项/兜底全部属于消费端 —— 装置只报事实。
//
// ───────────────────────────────────────────────────────────────────────────
// 生命周期与开销
// ───────────────────────────────────────────────────────────────────────────
//   Init(shader_mgr) → 每帧 Render(source, query, params) → Finalize()（析构也会调）
//   · 所有 GPU 目标**懒创建、尺寸变化才重建**；Render 的热路径上只有绑定 + draw。
//   · 内部每一趟都自己保存/恢复 GL 状态（FBO / viewport / program / 纹理绑定 / 开关），
//     调用方不需要收拾，也不必知道里面有几趟。
//   · 帧成本 ≈ 面积 × (降采样 + 柱体 + march + 滤波 + 升采样)；唯一的成本旋钮是
//     params.resolution_scale（1/N，全链路同倍率）。
//
// 契约：同输入同输出，逐字节。无历史、无抖动、无时序、无随机。

#ifndef JPOV_SRC_SSR_SSR_CHAIN_H_
#define JPOV_SRC_SSR_SSR_CHAIN_H_

#include "tools/jpov/src/ssr/ssr_downsample.h"
#include "tools/jpov/src/ssr/ssr_filter.h"
#include "tools/jpov/src/ssr/ssr_renderer.h"
#include "tools/jpov/src/ssr/ssr_slab_back.h"
#include "tools/jpov/src/ssr/ssr_upsample.h"

namespace jpov {

class ShaderManager;

// ==================== Source：被反射的世界 ====================
// 全分辨率，与 Query 同相机、同屏幕网格。所有字段 0 = 未设置 ⇒ Render 直接 FATAL。
struct SsrSource {
    unsigned int color_tex = 0;   // RGBA16F：纯辐亮度（未加雾/未 tonemap/未乘材质）
    unsigned int depth_tex = 0;   // R32F：窗口深度 [0,1]，1 = 天空/无几何
    unsigned int normal_tex = 0;  // RGBA16F：rgb = 视空间法线，a ≥ 0.25 = 有法线数据
    int width = 0;                // > 0
    int height = 0;               // > 0
};

// ==================== Query：谁在问 ====================
// 尺寸必须与 Source 相同（Render 会校验）。
struct SsrQuery {
    unsigned int depth_tex = 0;   // R32F：反射面自己的窗口深度，1 = 这一像素不查询
    unsigned int normal_tex = 0;  // RGBA16F：rgb = 视空间法线，a ≥ 0.25 = 参与查询
    unsigned int rough_tex = 0;   // RGBA16F：**只读 .a** = roughness（[0,1]）
};

// ==================== 参数 ====================
//
// 0 一律表示「未设置」⇒ FATAL（漏设参数应该立刻炸）。三个例外（有合法默认值）：
// tile_size（并发粒度，不影响画面）、grazing_fade_start/end（相等 = 关闭）、
// filter_* 三个 bool（默认关）。
//
// 这些参数与"管线位置"无关：全是算法/成本旋钮。
struct SsrParams {
    // ---- 相机（针孔）----
    float near_plane = 0.0f;      // > 0
    float far_plane = 0.0f;       // > near
    float tan_half_fov_y = 0.0f;  // > 0
    float aspect = 0.0f;          // > 0

    // ---- 分辨率：1/N（全链路同倍率，唯一的成本旋钮）----
    float resolution_scale = 0.0f;  // 1.0 / 0.5 / 0.3333… / 0.25

    // ---- 厚度模型 / 行进预算（见 ssr_renderer.h 的字段注释）----
    float thickness_pixels = 0.0f;  // > 0，单位 = SSR 像素（"倒影比真物体粗多少"）
    float min_thickness_m = 0.0f;   // > 0，米
    float max_distance_m = 0.0f;    // > 0，米
    int max_ray_pixels = 0;         // > 0，SSR 像素（覆盖 = 成本）
    int tile_size = 2;              // 1/2/4/8（必须是偶数且是 2 的幂）
    float roughness_cutoff = 0.0f;  // [0,1]：> 它的像素不发射线（1 = 从不跳过）

    // ---- 同面并集柱体（治掠射梳齿；三源取最厚之一）----
    float slab_min_thickness = 0.0f;  // (0, 0.5)，基线相对厚度
    float slab_gate_rel = 0.0f;       // (0, 1)，同面判定的深度残差门槛
    float slab_gate_cos = 0.0f;       // [0, 1]，同面判定的法线夹角门槛（cos）

    // ---- A（覆盖率）的淡出 ----
    // 只改 A：颜色/命中/距离都不变 ⇒ 消费端看到的是"这里别信我，用你的兜底"。
    // 前两条与位置无关（是无量纲的视角压缩比 / 距离边框的像素带）；
    // 三个 fade_* 的单位是**全分辨率屏幕像素**，装置内部按 1/N 折成 SSR 像素。
    // 0 = 关闭该条；grazing: end <= start 即关闭。
    float grazing_fade_start = 2.0f;     // kappa ≥ 它开始淡出
    float grazing_fade_end = 10.0f;      // kappa ≥ 它完全交给兜底
    float fade_budget_pixels = 0.0f;     // 命中点离 march 预算还剩多少
    float fade_edge_pixels = 0.0f;       // 反射体像素贴边
    float fade_hit_edge_pixels = 0.0f;   // 命中点贴边

    // ---- 遮丑滤波器（SSR 分辨率，mip 链之前；默认全关）----
    // 详见 ssr_filter.h 顶部：① 补洞（梳齿）② 离群抑制（毛刺）③ à-trous（多尺度）。
    bool filter_hole_fill = false;
    bool filter_outlier_clamp = false;
    bool filter_atrous = false;
    float filter_clamp_sigma = 2.0f;  // > 0
    int filter_atrous_iterations = 1; // [1,3]

    // ---- 升采样（值域项 + 粗糙度→LOD）----
    float upsample_depth_tolerance = 0.0f;  // > 0，软权尺度
    float upsample_hard_cut = 0.0f;         // > 0，超过 ⇒ 硬拒
    float upsample_weight_floor = 0.0f;     // ≥ 0，权重下限（防孔洞）
    float roughness_to_lod = 0.0f;          // ≥ 0，0 = 永远锐利
};

// ==================== 输出 ====================
// 模块自持（懒创建，尺寸变化时重建）。**别跨帧缓存这些纹理 id**，也别把下一次
// Render 的输入指到它们上面（那是反馈环）。
struct SsrChainResult {
    // 全分辨率 RGBA32F。rgb = 命中辐亮度（纯辐亮度，未乘 Fresnel/AO），a = 覆盖率 A。
    // A 已经包含：命中覆盖率（mip 链）+ 掠射/预算/贴边三种淡出 + 升采样的软权。
    unsigned int reflection_tex = 0;
    // 全分辨率 R8UI：bit0 = 这个像素的答案来自**背面命中**（几何对、颜色借用正面色）。
    // 装置只报事实：用不用、压暗多少，由消费端决定。
    unsigned int backface_tex = 0;
    // SSR 分辨率 RGBA16F：rgb = 命中颜色，a = A。**仅供调试/统计**（正确消费方式是
    // reflection_tex）；尺寸见 ssr_width/ssr_height。
    unsigned int raw_tex = 0;
    int width = 0;       // = Source/Query 的全分辨率宽度
    int height = 0;
    int ssr_width = 0;   // 内部 SSR 分辨率（= ceil(width/N)）
    int ssr_height = 0;
};

// ==================== 装置 ====================
class SsrChain {
public:
    SsrChain();
    ~SsrChain();

    SsrChain(const SsrChain&) = delete;
    SsrChain& operator=(const SsrChain&) = delete;

    // Pre-condition: GL context 已激活。可重复调用（先内部 Finalize 再重建）。
    void Init(ShaderManager* shader_mgr);

    // 释放所有 GL 对象。幂等；析构会调用。
    void Finalize();

    // 跑一遍，返回本次结果（模块自持，别跨帧缓存）。
    //
    // Pre-condition（任一不满足 ⇒ LOG(FATAL)）：
    //   · 已 Init；Source/Query 的五张纹理都 ≠ 0，且 Source 两张尺寸 == 声明的 w×h，
    //     Query 三张尺寸 == Source 的尺寸；
    //   · 相机 / 厚度 / 预算 / 升采样参数全部合法（见 SsrParams 每条的范围）；
    //   · resolution_scale ∈ {1, 1/2, 1/3, 1/4}；tile_size ∈ {1,2,4,8}；
    //   · 输入纹理都不是本类自己的输出（反馈环）。
    const SsrChainResult& Render(const SsrSource& source, const SsrQuery& query,
                            const SsrParams& params);

private:
    ShaderManager* shader_mgr_ = nullptr;

    // 五段流水（各自拥有一份 GPU 资源）。顺序见 .cc 的 Render —— 那是唯一编排点。
    SsrDownsample downsample_source_;  // 全分辨率 → SSR 分辨率（源：color + w + normal）
    SsrDownsample downsample_query_;   // 全分辨率 → SSR 分辨率（查询：w + normal + rough）
    SsrSlabBack slab_back_;            // SSR 分辨率：同面并集柱体的 back
    SsrRenderer renderer_;             // SSR 分辨率：march + resolve（纯算法）
    SsrFilter filter_;                 // SSR 分辨率：A 预乘 mip 链（+ 可选三个遮丑滤波器）
    SsrUpsample upsample_;             // SSR 分辨率 → 全分辨率（双边升采样）

    SsrChainResult result_;                 // 每帧刷新（只改字段，不重新分配）
    bool initialized_ = false;
};

}  // namespace jpov

#endif  // JPOV_SRC_SSR_SSR_CHAIN_H_
