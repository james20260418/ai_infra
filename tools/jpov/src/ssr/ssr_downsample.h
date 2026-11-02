// JPOV SSR — 输入准备（降采样）
//
// 职责：把**全分辨率**的三张图（HDR 颜色 / 场景深度 / 属性纹理）降到 SSR 分辨率，
// 供 SsrRenderer 使用。它是 SSR 唯一的"输入准备"环节。
//
// 选点规则（**三张图必须取自同一个 texel**）：
//   在 scale×scale 的方块里挑**窗口深度最小**（= 离相机最近）的那一个全分辨率 texel，
//   颜色 / 深度 / 法线全部取它。
//   为什么不能各自取（颜色平均 + 深度取最小）：两者会分属**不同表面**，出错极隐蔽
//   （倒影里出现"别的物体的颜色"）。为什么取最近：深度缓冲只记最前表面，镜子里能看见的
//   也是它；被挡住的背景不该发射射线。min 对连续函数连续 ⇒ 相机移动时不会跳变。
//
// 属性纹理的 .a（掩码编码）与 .rgb（三维视空间法线）都要**原样带下去**：
//   · rgb 是命中面朝向判据与被反射物法线；
//   · a 告诉合成层"这一格是不是反射体"（模块本身不读 a）。
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

// GL 侧：拥有降采样目标纹理与 FBO，一趟把三张全分辨率图降到 SSR 分辨率。
//
// 生命周期：Init(shader_mgr) → 每帧 Run(...) → Finalize()。
// Run 会保存/恢复调用方的 FBO、viewport、活动纹理单元与纹理绑定。
class SsrDownsample {
public:
    SsrDownsample();
    ~SsrDownsample();
    SsrDownsample(const SsrDownsample&) = delete;
    SsrDownsample& operator=(const SsrDownsample&) = delete;

    // Pre-condition: GL context 已激活。
    void Init(ShaderManager* shader_mgr);
    void Finalize();

    // 跑一趟。scale = 1/N（用 IsValidScale 校验）。
    // 返回后 color_tex()/w_tex()/normal_tex()/rough_tex() 为本帧结果，尺寸见 width()/height()。
    // near/far：窗口深度 → w = 1/z 的换算需要（见 ssr_math.h::WFromWindowDepth）。
    void Run(unsigned int full_color_tex, unsigned int full_depth_tex,
             unsigned int full_attr_tex, unsigned int full_attr_mat_tex,
             int full_w, int full_h, int scale, float near_plane, float far_plane);

    unsigned int color_tex() const { return color_tex_; }    // RGBA16F，命中源色
    // R32F，**w = 1/z_view**（0 = 天空/无几何）。SSR 链路内部一律用 w。
    unsigned int w_tex() const { return w_tex_; }
    unsigned int normal_tex() const { return normal_tex_; }  // RGBA16F，rgb=视空间法线, a=掩码
    // R16F：反射层**逐像素** roughness（属性纹理附件 1 的 .a）。
    // 只用于 march/resolve 的"太粗糙就不发射线"截止；材质本身（F0）由合成层在全分辨率读。
    unsigned int rough_tex() const { return rough_tex_; }
    int width() const { return width_; }
    int height() const { return height_; }

private:
    void EnsureTargets(int w, int h);
    void DestroyTargets();

    ShaderManager* shader_mgr_ = nullptr;
    unsigned int prog_ = 0;
    unsigned int fbo_ = 0;
    unsigned int color_tex_ = 0;
    unsigned int w_tex_ = 0;       // R32F：w = 1/z_view（0 = 空）
    unsigned int normal_tex_ = 0;
    unsigned int rough_tex_ = 0;
    int width_ = 0;
    int height_ = 0;
};

}  // namespace jpov

#endif  // JPOV_SRC_SSR_SSR_DOWNSAMPLE_H_
