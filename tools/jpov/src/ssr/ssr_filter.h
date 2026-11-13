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

// ── 三个"遮丑"滤波器（2026-11-13，多配置选型用）──
//
// 都跑在 **SSR 分辨率**、level 0 之后、mip 链之前；都默认关闭（关闭时逐字节等于旧行为）。
// 三条各治一种病，可以单独开、也可以叠（顺序固定：补洞 → 离群 → 小波）：
//
//   ① hole_fill     形态学：只补"左右（或上下）两侧都是命中、自己是空"的 1px 缝。
//                   专治周期性梳齿；对宽洞（真没数据）不动，所以不会凭空造出倒影。
//   ② outlier_clamp 统计：3×3 加权均值/方差，把偏离 k·σ 的样本拉回边界。
//                   专治孤立亮点/黑点（"毛刺"），只动离群点 ⇒ 几乎不损失锐度。
//   ③ atrous        频域/多尺度：边缘保持的 à-trous 小波（5×5 B3 核，步长 1/2/4）。
//                   深度 + 法线引导 ⇒ 跨剪影不串色；A 被当作"样本数"一起平均 ⇒
//                   梳齿处的覆盖率由邻域抬起来，真空洞仍然交回 env。
//
// 为什么放在低分侧而不是升采样之后：梳齿的周期 = **SSR texel**。在 SSR 分辨率上
// 覆盖同样的世界半径只要 1/N² 的像素；换到全分辨率需要覆盖 2N 个屏幕像素才能跨过
// 同样的洞（成本 ×N²，模糊量还一样大）。升采样自己产生的那 1~2 px 台阶由融合层的
// 双边核负责，不在这三条的职责里。
//
// 引导纹理（可选）：w = 1/z 与视空间法线，**SSR 分辨率**（就是降采样那两张）。
// 打开了任何一条却没给引导 ⇒ 直接 FATAL（采样未绑定纹理是不可预期的，不静默兜底）。
struct SsrFilterGuides {
    unsigned int down_w_tex = 0;       // R32F，w = 1/z（0 = 天空/无几何）
    unsigned int down_normal_tex = 0;  // RGBA16F，rgb = 三维视空间法线
};

struct SsrFilterConfig {
    bool hole_fill = false;        // ①
    bool outlier_clamp = false;    // ②
    bool atrous = false;           // ③
    int atrous_iterations = 1;     // ③ 趟数（1..3；步长 1/2/4；2 趟起明显发虚）
    float clamp_sigma = 2.0f;      // ② 允许偏离 k·σ（> 0）
    float guide_depth_tol = 0.15f; // 引导的深度软权尺度（相对残差）
    float guide_normal_cos = 0.85f;// 引导的法线门限（低于它视为跨面，权重 0）
};


class SsrFilter {
public:
    SsrFilter();
    ~SsrFilter();
    SsrFilter(const SsrFilter&) = delete;
    SsrFilter& operator=(const SsrFilter&) = delete;

    // Pre-condition: GL context 已激活。
    void Init(ShaderManager* shader_mgr);
    void Finalize();

    // ssr_radiance_tex：RGBA16F，rgb = 命中颜色、a = **A（置信度）**（未命中 rgb 无意义）
    // ssr_type_tex    ：R8UI，SsrResultType（只有 kHit 算命中）
    // 两张图必须都是 w×h。Run 会保存/恢复调用方的 FBO、viewport、program 与纹理绑定。
    //
    // 4 参数重载 = 三个遮丑滤波器全关 + 无引导（等价于 2026-11-13 之前的行为）。
    void Run(unsigned int ssr_radiance_tex, unsigned int ssr_type_tex, int w, int h);
    void Run(unsigned int ssr_radiance_tex, unsigned int ssr_type_tex, int w, int h,
             const SsrFilterGuides& guides, const SsrFilterConfig& cfg);

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
    unsigned int prog_hole_ = 0;   // ① 形态学补洞
    unsigned int prog_clamp_ = 0;  // ② 方差引导离群抑制
    unsigned int prog_atrous_ = 0; // ③ 边缘保持 à-trous
    // 遮丑滤波器之间、以及它们与 mip 链之间的中转（避免"边采样边写"的反馈环）。
    unsigned int post_tex_[2] = {0, 0};
    // 本帧的引导与配置（只在 Run 期间有效，供 PassToTexture 读取）。
    SsrFilterGuides cur_guides_;
    SsrFilterConfig cur_cfg_;
    // 本帧的类型图（R8UI）：补洞/离群都用它区分"结构空位"与"淡出后的低 A"。
    unsigned int cur_type_tex_ = 0;

    // 一趟全屏后处理：src → dst，step 只给 à-trous 用（补洞/离群传 0）。
    // Pre-condition: GL context 已激活；src != dst；尺寸 = width_×height_。
    void PassToTexture(unsigned int prog, unsigned int src_tex, unsigned int dst_tex,
                       int step);
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
