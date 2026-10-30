// JPOV DebugInfo — 引擎级 debug 信息快照（左上角常驻 HUD 的内容）
//
// 定位：**引擎持有的一份状态**，不是渲染指令。JPOV 在每帧的 RunOnceInternal 里
// 就地更新它（帧号 / 分辨率 / fps / 本帧相机），Init 时填一次硬件信息；
// 显示与否由 `JPOV::Config::debug_overlay`（初始值，**默认为开**）+ `F3`
//（运行时临时收起）决定。
//
// 为什么不走 RenderCommandList：debug HUD 必须在**所有** app 上都可用，包括完全
// 不写绘制指令的 app；而且它是引擎自身状态的可视化，混进用户的指令列表会污染
// 「用户给了什么就画什么」的契约（见 docs/jpov_engine_integration.md）。
//
// 文本编码：**纯 ASCII**（内置 8x8 点阵字体，不依赖 app 注册的任何 TTF），
// 见 src/debug_overlay/debug_font8x8.h。
//
// 行长契约：**每行 <= kMaxLineChars 个字符**。8x8 点阵字体的前进量就是 8 像素，
// 所以一行的像素宽 = 字符数 x 8。面板（半透明黑底）宽度 = 8(左留白) + 行宽 + 4(右留白)，
// 于是 kMaxLineChars = 61 ⇒ 8 + 488 + 4 = **500px**，正好是约定的上限。
// 这条契约让 HUD 在任何窗口宽度下都有确定的占地，也是 kMaxGlFieldChars 存在的
// 唯一理由 —— 驱动字符串长度不受我们控制。

#ifndef JPOV_DEBUG_INFO_H_
#define JPOV_DEBUG_INFO_H_

#include <cstdint>
#include <string>

#include "tools/jpov/interface/camera.h"
#include "tools/jpov/interface/gpu_info.h"

namespace jpov {

// 上一帧 / 当前帧的引擎状态快照。
//
// 生命周期：
//   * gpu          —— Init() 采一次，此后不变（进程内单 GL context）
//   * camera       —— 每帧取自用户填的 RenderCommandList::camera
//   * width/height —— 每帧的渲染分辨率（= BeginFrame 使用的尺寸）
//   * frame_count  —— 每帧的帧号（Run() 自增；RunOnce() 由调用方给）
//   * fps          —— 最近一帧的**瞬时**帧率（1 / 相邻两帧墙钟间隔），不是平滑平均
struct DebugInfo {
    GpuInfo gpu;

    // 本帧实际生效的相机（用户填进 cmds->camera 的那份）。
    // 未设置时是 Camera 的默认构造值（不是"无相机"的哨兵）。
    Camera camera;

    // 渲染分辨率（像素）。0 = 尚未渲染过任何一帧。
    float width  = 0.0f;
    float height = 0.0f;

    // 帧号（>= 0）。RunOnce() 由调用方给，Run() 从 0 递增。
    int64_t frame_count = 0;

    // 最近一帧的瞬时帧率。首帧（还没有上一帧）为 0。
    float fps = 0.0f;

    // 转成可直接绘制/打印的多行 ASCII 文本（行间用 '\n'，结尾不带换行）。
    //
    // 格式（示例）：
    //   [JPOV] 1280x720 frame 42 fps 59.8
    //   GPU  cpu-raster
    //        llvmpipe (LLVM 15.0.7, 256 bits)
    //        vendor Mesa/X.org
    //        GL 4.5 (Core Profile) Mesa 23.0.4
    //        glsl 4.50
    //        max_tex 32768 max_attribs 16 max_color_attach 8
    //   CAM  pos (1.20, 2.30, 3.40) -> (0.00, 0.00, 0.00)
    //        up (0.00, 1.00, 0.00) fov 60.0 near 0.10 far 1000.00
    //
    // （一个 GL 字段一行：原先把 vendor 和 GL 版本拼成一行，最坏情况是两个
    //   56 字符字段相加，必然破 500px 预算。）
    // 每条 GL 原始字符串**最终渲染宽度**最多 kMaxGlFieldChars 个字符，**包含**结尾的
    // "..." —— 驱动字符串长度不受我们控制，截断是为了保证 HUD 不会被一条超长字符串
    // 撑出屏幕（截断是有意的、写在这里，不是静默行为）。
    std::string DebugString() const;

    // 单条 GL 字段（含 "..." 在内的最终长度）上限。推导：
    //   kMaxLineChars(61) - 缩进(5) = 56，正好取 56。
    //
    // 为什么是 56 而不是随手写大：真实驱动里最长的常见串
    // "4.5 (Core Profile) Mesa 23.0.4" 只有 29 字符，56 有一倍余量；
    // 而旧值 96 会让「vendor | GL 版本」合并行到 216 字符 ≈ 1728px，直接飞出屏幕。
    static constexpr int kMaxGlFieldChars = 56;

    // 单行字符数上限。HUD 的宽度契约就是这一条数字：所有行长（标签 + 缩进 +
    // 截断后的字段）都必须满足它。61 = (500 - 8 - 4) / 8。
    static constexpr int kMaxLineChars = 61;
};

}  // namespace jpov

#endif  // JPOV_DEBUG_INFO_H_
