// JPOV SSR — 输入准备（降采样）
//
// 职责：把**全分辨率**的一组屏幕图降到 SSR 分辨率，供 SsrRenderer 使用。
//
// 2026-11-27：同一套选点规则要服务**两种**输入（见 ssr_chain.h 的 Source/Query 之分）：
//   kSource —— 被反射的世界：color + depth(window) + normal  →  color + w + normal
//   kQuery  —— 谁在问（反射面自己）：depth(window) + normal + rough →  w + normal + rough
// 两者的**选点规则完全一样**（同一个 N×N 方块里挑窗口深度最小的 texel，各图同源），
// 所以只写一份着色器、用 `SSR_DOWNSAMPLE_QUERY` 宏切输出布局。
//
// 选点规则（**同一 kind 内的几张图必须取自同一个 texel**）：
//   在 scale×scale 的方块里挑**窗口深度最小**（= 离相机最近）的那一个全分辨率 texel，
//   颜色 / 深度 / 法线 / 粗糙度全部取它。
//   为什么不能各自取（颜色平均 + 深度取最小）：两者会分属**不同表面**，出错极隐蔽
//   （倒影里出现"别的物体的颜色"）。为什么取最近：深度缓冲只记最前表面，镜子里能看见的
//   也是它；被挡住的背景不该发射射线。min 对连续函数连续 ⇒ 相机移动时不会跳变。
//
// 法线纹理的 .a（掩码编码）与 .rgb（三维视空间法线）都要**原样带下去**：
//   · rgb 是命中面朝向判据（源）与反射方向（查询）；
//   · a ≥ 0.25 = 这一格有法线数据（模块用它决定"要不要判朝向 / 要不要发射线"）。
//
// 纯数学部分（IsValidScale / DownsampleTargetSize / PickSourceTexel）不依赖 GL，
// 单测落在 ssr_downsample_test.cc。

#ifndef JPOV_SRC_SSR_SSR_DOWNSAMPLE_H_
#define JPOV_SRC_SSR_SSR_DOWNSAMPLE_H_

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace jpov {

class ShaderManager;

// 只接受 1/N（N = 1..4，即 1.0 / 0.5 / 0.3333… / 0.25）。其它值一律视为配置错误。
// 用 lround 反推 N，容差 1e-3 吃掉面板滑条/浮点表示误差。
inline bool IsValidScale(float scale) {
    if (!(scale > 0.0f)) {
        return false;
    }
    const int n = static_cast<int>(std::lround(1.0f / scale));
    return n >= 1 && n <= 4 && std::fabs(1.0f / static_cast<float>(n) - scale) < 1.0e-3f;
}

// 降采样后的目标尺寸：向上取整保证覆盖全屏，且至少 1×1。
inline void DownsampleTargetSize(int full_w, int full_h, int scale, int* out_w, int* out_h) {
    const int s = std::max(1, scale);
    *out_w = std::max(1, (full_w + s - 1) / s);
    *out_h = std::max(1, (full_h + s - 1) / s);
}

// 一组 scale×scale 里选出的源 texel（窗口深度 = [0,1]，1.0 = 远平面/天空）。
struct DownsamplePick {
    int x = 0;
    int y = 0;
    float depth = 2.0f;   // 2.0 = 全部越界时的哨兵（实际不会出现：至少有一个夹取后的 texel）
};

// 选点规则的 CPU 参照（着色器逐字同式）。depth_at(x, y) 返回该全分辨率像素的窗口深度。
// 越界坐标夹到边缘（与着色器的 clamp 一致）。
template <typename DepthFn>
DownsamplePick PickSourceTexel(int dx, int dy, int scale, int full_w, int full_h,
                               DepthFn&& depth_at) {
    const int s = std::max(1, scale);
    const int base_x = dx * s;
    const int base_y = dy * s;
    DownsamplePick best;
    best.x = std::min(std::max(base_x, 0), std::max(full_w - 1, 0));
    best.y = std::min(std::max(base_y, 0), std::max(full_h - 1, 0));
    best.depth = depth_at(best.x, best.y);
    for (int j = 0; j < s; ++j) {
        for (int i = 0; i < s; ++i) {
            const int p = std::min(std::max(base_x + i, 0), std::max(full_w - 1, 0));
            const int q = std::min(std::max(base_y + j, 0), std::max(full_h - 1, 0));
            const float z = depth_at(p, q);
            if (z < best.depth) {
                best.depth = z;
                best.x = p;
                best.y = q;
            }
        }
    }
    return best;
}

// 降采样的两种角色。kind 在 Init 时定死（它决定着色器变体与目标纹理的布局）：
//   kSource → color_tex()/w_tex()/normal_tex() 有效，rough_tex() == 0
//   kQuery  → w_tex()/normal_tex()/rough_tex() 有效，color_tex() == 0
enum class SsrDownsampleKind {
    kSource,
    kQuery,
};

// GL 侧：拥有降采样目标纹理与 FBO，一趟把全分辨率图降到 SSR 分辨率。
//
// 生命周期：Init(shader_mgr, kind) → 每帧 Run(inputs) → Finalize()。
// Run 自己保存/恢复调用方的全部相关 GL 状态（FBO、viewport、program、活动纹理单元与
// 纹理绑定、blend/depth/cull/scissor）—— 调用方不需要收拾。
// 纹理所有权：输入纹理是**调用方的**（只借不解绑）；输出三张是本类持有的（Finalize 释放，
// 下一次 Run 改写），只借给调用方读。
class SsrDownsample {
public:
    // 一趟的全部输入（只读）。各字段的"未设置 = 0"一律 FATAL，不做兜底。
    struct Inputs {
        unsigned int color_tex = 0;   // 仅 kSource：RGBA16F 源颜色
        unsigned int depth_tex = 0;   // 必填：R32F **窗口深度**（1 = 天空/无几何）
        unsigned int normal_tex = 0;  // 必填：RGBA16F rgb = 视空间法线，a = 掩码编码
        unsigned int rough_tex = 0;   // 仅 kQuery：RGBA16F，**只读 .a** = roughness
        int full_w = 0;               // > 0
        int full_h = 0;               // > 0
        int scale = 0;                // 1..4（= 1 / resolution_scale 的整数倍率）
        float near_plane = 0.0f;      // > 0（窗口深度 → w 的换算需要）
        float far_plane = 0.0f;       // > near
    };

    SsrDownsample();
    ~SsrDownsample();
    SsrDownsample(const SsrDownsample&) = delete;
    SsrDownsample& operator=(const SsrDownsample&) = delete;

    // Pre-condition: GL context 已激活。可重复调用（先内部 Finalize 再重建）。
    void Init(ShaderManager* shader_mgr, SsrDownsampleKind kind);
    void Finalize();

    // 跑一趟。尺寸 / scale 变化时内部重建目标（Render 热路径上零分配）。
    void Run(const Inputs& in);

    unsigned int color_tex() const { return color_tex_; }    // RGBA16F（kSource 才有）
    // R32F，**w = 1/z_view**（0 = 天空/无几何）。SSR 链路内部一律用 w。
    unsigned int w_tex() const { return w_tex_; }
    // RGBA16F，rgb = 三维视空间法线，a = 掩码编码（**原样带下去**）。
    unsigned int normal_tex() const { return normal_tex_; }
    // R16F：逐像素 roughness（kQuery 才有），只采样 .r。
    unsigned int rough_tex() const { return rough_tex_; }
    int width() const { return width_; }
    int height() const { return height_; }

private:
    void EnsureTargets(int w, int h);
    void DestroyTargets();

    ShaderManager* shader_mgr_ = nullptr;
    SsrDownsampleKind kind_ = SsrDownsampleKind::kSource;
    unsigned int prog_ = 0;
    unsigned int fbo_ = 0;
    unsigned int color_tex_ = 0;   // kSource：RGBA16F 源颜色（命中取色用）
    unsigned int w_tex_ = 0;       // R32F：w = 1/z_view（0 = 空）
    unsigned int normal_tex_ = 0;  // RGBA16F：rgb = 三维视空间法线，a = 掩码编码
    unsigned int rough_tex_ = 0;   // kQuery：R16F 逐像素 roughness
    int width_ = 0;
    int height_ = 0;
};

}  // namespace jpov

#endif  // JPOV_SRC_SSR_SSR_DOWNSAMPLE_H_
