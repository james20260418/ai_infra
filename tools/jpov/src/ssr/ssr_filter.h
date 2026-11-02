// JPOV SSR — 粗糙度预滤波（SSR 结果的 A 预乘 mip 链）
//
// 为什么需要它（2026-11-02）：
//   SSR 每个像素只发**一条**镜面射线，算出来的是"完全光滑"的答案。粗糙表面的正确答案
//   是"以反射方向为中心的一整片区域的平均"（反射瓣 = 微表面法线分布的宽度）。
//   没有这一步，粗糙材质上的命中区域仍然是**像素级锐利**的 —— 也就是"把石头画成镜子"，
//   和 A=0 处那块模糊的 env 天空拼在一起，观感就是拼贴。
//
// 做法（业界非时序流派，见 NVIDIA "Glossy GI" / EA "Hybrid Rendering" 章节）：
//   把 SSR 结果按覆盖率做**加权 mip 链**，合成层按 roughness 采 textureLod。
//   粗糙度 → LOD 是连续量（硬件三线性插值）⇒ 拖滑条不会有 mip 台阶，这才是"逐渐"。
//
// 三条必须守住的细节（都是这一行算法最容易做错的地方）：
//   ① **颜色按覆盖率加权平均**（rgb' = Σ(rgb·a)/Σa），不是简单 box 平均。
//      命中像素的 rgb 有意义、未命中像素的 rgb 是 0（契约 3），直接平均 ⇒ "越粗糙越暗"。
//   ② **空像素要做填充**（3×3 找最近命中的颜色抄过来，但 a 保持 0）。
//      否则双线性/三线性在覆盖率边界上把 rgb 往 0 插，边缘一圈黑。
//      业界原话是 expand out into untraced areas so trilinear fetches don't hit black。
//   ③ **覆盖率本身跟着 mip 一起降**（a' = Σa/4）。合成层用这个 a 当 A：
//      覆盖率的软化 = 反射体边缘的抗锯齿；A 变小 = 自然交接给 env，不需要额外开关。
//
// 契约：确定性（无历史、无抖动、无随机），同一输入逐字节同输出；不改输入纹理。
// 成本：level 0 一趟填充 + (levels−1) 趟降采样，全部在 SSR 分辨率（通常 1:2）上。

#ifndef JPOV_SRC_SSR_SSR_FILTER_H_
#define JPOV_SRC_SSR_SSR_FILTER_H_

#include <vector>

namespace jpov {

class ShaderManager;

class SsrFilter {
public:
    SsrFilter();
    ~SsrFilter();
    SsrFilter(const SsrFilter&) = delete;
    SsrFilter& operator=(const SsrFilter&) = delete;

    // Pre-condition: GL context 已激活。
    void Init(ShaderManager* shader_mgr);
    void Finalize();

    // ssr_radiance_tex：RGBA16F，rgb = 命中颜色、a = ray_ratio（未命中 rgb 无意义）
    // ssr_type_tex    ：R8UI，SsrResultType（只有 kHit 算命中）
    // 两张图必须都是 w×h。Run 会保存/恢复调用方的 FBO、viewport、program 与纹理绑定。
    void Run(unsigned int ssr_radiance_tex, unsigned int ssr_type_tex, int w, int h);

    // RGBA16F 带 mip 链：rgb = 覆盖率加权的平均颜色；a = 该级别的覆盖率。
    // MIN_FILTER = LINEAR_MIPMAP_LINEAR ⇒ 直接用 textureLod(uv, lod) 连续采样。
    unsigned int mip_tex() const { return mip_tex_; }

    // mip 级数（含 level 0）。合成层把 LOD clamp 到 [0, level_count()-1]。
    int level_count() const { return levels_; }

private:
    void EnsureTargets(int w, int h);
    void DestroyTargets();

    ShaderManager* shader_mgr_ = nullptr;
    unsigned int prog_fill_ = 0;   // level 0：覆盖率化 + 3×3 空像素填充
    unsigned int prog_down_ = 0;   // level n → n+1：A 预乘 box
    unsigned int mip_tex_ = 0;     // 最终对外的那张（带完整 mip 链）
    // ⚠️ 每一级先渲到**独立纹理**，最后用 glBlitFramebuffer 拼进 mip_tex_ 的各 level。
    //    不能直接渲进 mip_tex_ 的 level l 再采样它的 level l−1：同一张纹理对象既挂在 FBO
    //    上又被采样 = 未定义的反馈环（驱动通常容忍，但可能给出垃圾且不报错）。
    std::vector<unsigned int> level_tex_;
    unsigned int fbo_src_ = 0;     // 渲染 level / blit 读端
    unsigned int fbo_dst_ = 0;     // blit 写端
    int levels_ = 0;
    int width_ = 0;
    int height_ = 0;
};

}  // namespace jpov

#endif  // JPOV_SRC_SSR_SSR_FILTER_H_
