// JPOV flipbook 图集格式约定 —— 引擎侧唯一权威
//
// 「flipbook」= 把一段逐帧动画排进一张纹理，运行时按时间取其中一格（相邻两格可混合）。
// 本文件只定义**格式**与**时间 → 帧号**的换算，不定义怎么生成（生成侧见
// `tools/jpov/assets/sp_ui/marble/README.md` 与 `genVolume/make_flipbook.py`）。
//
// ==================== 格式约定（必须逐条遵守，错一条就是“能跑但看着怪”）====================
//
// 1) 布局：`tiles_x × tiles_y` **规则网格**，**行主序**，**帧 0 在图片左上角**。
//    第 f 帧 = (col = f % tiles_x, row = f / tiles_x)，从图片左上向右铺、再下一行。
//    单行横条就是 `tiles_y = 1` 的退化情况。
//
// 2) 采样时要**半像素内缩**：cell 内边距 1 像素。
//    `uv_tex = (uv * (cell_px - 2) + 1 + cell_origin_px) / tex_size`
//    不内缩时双线性会在 cell 边界取到邻居帧的 texel ⇒ 透明底 sprite 出现一像素彩边。
//
// 3) 材质平铺（repeat）与取 cell 是**两次 UV 变换，顺序不能换**：
//    先 `fract(uv * tiling + scroll)` 平铺，**再**进 cell。
//    并且 `fract` 的导数在接缝处断，mip 会自动选到最粗一级 ⇒ 接缝糊一条线；
//    UI 这类 1:1 像素场景请直接**不开 mipmap**。
//
// 4) 纹理过滤/包裹：`repeat = true`（要平铺）、`mipmap = false`（见 3）。
//
// 5) 时间 → 帧号：**GPU 不取时钟**。调用方给「秒」（零点自定），这里只做乘法：
//    `frame = time_seconds * fps`，再 wrap 到 `[0, frame_count)`。
//    这样同一份命令 + 同一秒数必然产出同一张图（gold 可复现）。
//
// 6) 帧间混合：用**分数**帧号，`mix(cell(floor(f)), cell(mod(floor(f)+1, N)), frac(f))`。
//    末帧回绕到首帧。分数帧号同时给了「慢动作插值」，不必靠加帧数。
//
// 7) 循环闭合：生成侧必须让**第 N 帧的图案等价于第 0 帧**（例如位移场相位走满整圈）。
//    可判定判据：以 `t = i / fps` 逐拍渲染，`t = N / fps` 必须与 `t = 0` **逐像素相同**。
//    ⚠️ 注意「闭合」不等于「最后一格与第一格长得一样」——那是重复帧（会顿一下）；
//    正确的是「最后一格是第 0 帧**前一拍**」。
//
// 8) 行序与消费者 UV 的 v 方向**成对**：若消费者的 `uv.y = 0` 就在图片顶部
//    （例如用屏幕坐标算 UV 的 2D 板材），**不要再翻 row**；反过来才需要翻。
//    翻错的症状是「每到一个行边界就跳一下」，看起来像不闭合。
//
// ==================== 现状 ====================
//
// 目前有**两套独立实现**用了同一套约定，但只有本文件是共享的格式描述：
//   * 粒子：字段散在 `EmitterConfig`（tiles_x/tiles_y/sheet_time_mode/sheet_fps…）
//     与 `ParticleRenderCommand`（tiles_x/tiles_y/flipbook/frame_blend），
//     帧号由 `ParticleEmitter::ResolveFrame()` 算（支持 5 种时间模式）。
//     粒子**尚未**改用本结构（没有需求，且它的五种时间模式比本结构表达的多）。
//   * 2D 板材：`PlateLayer`（见 render_command.h）直接携带 `FlipbookDesc` 需要的信息，
//     帧号由 `ResolveFlipbookFrame()` 算。
// 共用点是**格式约定**，不是代码。哪天粒子要重构，以本文件为准。

#ifndef JPOV_INTERFACE_FLIPBOOK_H_
#define JPOV_INTERFACE_FLIPBOOK_H_

#include <cmath>

#include <glog/logging.h>

namespace jpov {

// 一张 flipbook 图集的格式描述（纯数据，无资源句柄）。
struct FlipbookDesc {
    int tiles_x = 1;    // 列数（>= 1）
    int tiles_y = 1;    // 行数（>= 1）
    float fps = 0.0f;   // 播放帧率（帧/秒）。0 = 静态（**整张图当一帧**，不裁 UV）
                        // ⚠️ 所以图集纹理必须给 fps > 0：否则 4x4 图集会整张当作一幅图显示。

    // 总帧数（= 网格格数）。
    int frame_count() const { return tiles_x * tiles_y; }

    // 是否按帧裁切（fps > 0 且格数 > 1）。
    bool IsAnimated() const { return fps > 0.0f && frame_count() > 1; }

    // Pre-condition: tiles_x >= 1；tiles_y >= 1；fps >= 0；fps > 0 时需有限值
    void Validate() const {
        CHECK_GE(tiles_x, 1) << "FlipbookDesc tiles_x 必须 >= 1";
        CHECK_GE(tiles_y, 1) << "FlipbookDesc tiles_y 必须 >= 1";
        CHECK_GE(fps, 0.0f) << "FlipbookDesc fps 必须 >= 0";
        CHECK(std::isfinite(fps)) << "FlipbookDesc fps 必须是有限值";
    }
};

// 把「秒」换成 `[0, frame_count)` 的**分数**帧号（与着色器同一套语义）。
//
// 返回 false = 这张图集不走翻书（静态，整张图当一帧），调用方应关闭裁 UV 分支；
// 返回 true 时 *out_frame 已写入分数帧号。
//
// Pre-condition: out_frame != nullptr；desc 已 Validate
inline bool ResolveFlipbookFrame(const FlipbookDesc& desc, float time_seconds,
                                 float* out_frame) {
    CHECK(out_frame != nullptr) << "ResolveFlipbookFrame: out_frame 不能为空";
    CHECK(std::isfinite(time_seconds))
        << "ResolveFlipbookFrame: time_seconds 必须是有限值";
    if (!desc.IsAnimated()) {
        *out_frame = 0.0f;
        return false;
    }
    const float total = static_cast<float>(desc.frame_count());
    const float f = time_seconds * desc.fps;
    // wrap 到 [0, total)：f 可能为负（调用方传负时间做倒放）也可能非常大。
    float frame = f - total * std::floor(f / total);
    if (frame >= total) {
        frame = total - 1.0f;   // 浮点边界兜底
    }
    *out_frame = frame;
    return true;
}

}  // namespace jpov

#endif  // JPOV_INTERFACE_FLIPBOOK_H_
