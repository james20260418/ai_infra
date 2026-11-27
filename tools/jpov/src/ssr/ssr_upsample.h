// JPOV SSR — 升采样（SSR 分辨率的 A 预乘结果 → 全分辨率）
//
// 输入是 SsrFilter 的**覆盖率预乘 mip 链**（rgb = 覆盖率加权平均辐亮度，a = 该级覆盖率）
// 加上**查询像素自己的**全分辨率几何（深度/法线/roughness），输出全分辨率的两张图：
//   reflection_tex —— RGBA32F：rgb = 平均辐亮度（未乘 Fresnel/AO），a = 覆盖率 A
//   backface_tex   —— R8UI：bit0 = 该像素采用的是**背面命中**（见 ssr_shader.h 的编码）
//
// 为什么需要它（不在低分辨率直接合成）：
//   ① 反射体边界要抗锯齿。A 是"这一格有多少像素真的是命中"，覆盖率随 mip 一起下降 ⇒
//      边缘天然软化，不需要额外开关。
//   ② 低分辨率格子跨到另一张表面时不能把颜色串过来 —— 这就是**双边**值域项
//      （1/z 的屏幕仿射预测 + 法线夹角），见下面 WScreenGrad 的注释。
//
// 三条必须守住的细节（都是这一行算法最容易做错的地方）：
//   ① 颜色按覆盖率**预乘加权**（Σ(w·A·L) / Σ(w·A)），不是简单 box 平均。
//      未命中像素的 rgb 无意义（写 0），直接平均 ⇒ "越粗糙越暗"。
//   ② 空间权用**双线性**（2×2），值域权用软权 + 权重下限 ⇒ Σw 永不为 0，不出孔洞。
//   ③ A 从预乘和里**除回去**（den/wsum），不是直接相加。

#ifndef JPOV_SRC_SSR_SSR_UPSAMPLE_H_
#define JPOV_SRC_SSR_SSR_UPSAMPLE_H_

namespace jpov {

class ShaderManager;

class SsrUpsample {
public:
    // 一趟的全部输入（只读）。全部必填：0 = 未设置 ⇒ FATAL。
    struct Inputs {
        unsigned int mip_tex = 0;           // RGBA16F，带 mip 链（SsrFilter::mip_tex()）
        int mip_levels = 0;                 // ≥ 1（LOD 上限 = 级数 − 1）
        unsigned int down_w_tex = 0;        // R32F，**查询**的 SSR 分辨率 w = 1/z（0 = 空）
        unsigned int down_normal_tex = 0;   // RGBA16F，查询的 SSR 分辨率视空间法线
        unsigned int query_depth_tex = 0;   // R32F，全分辨率窗口深度（1 = 无数据）
        unsigned int query_normal_tex = 0;  // RGBA16F，全分辨率视空间法线
        unsigned int query_rough_tex = 0;   // RGBA16F，全分辨率，**只读 .a** = roughness
        unsigned int backface_tex = 0;      // R8UI，SSR 分辨率 bit0 = 背面命中
        int full_w = 0;                     // > 0
        int full_h = 0;                     // > 0
        int ssr_w = 0;                      // > 0
        int ssr_h = 0;
        int scale = 0;                      // 1..4（全分辨率 / SSR 分辨率的整数倍率）
        float near_plane = 0.0f;            // > 0
        float far_plane = 0.0f;             // > near
        float roughness_to_lod = 0.0f;      // ≥ 0：roughness → LOD 的斜率
        float depth_tolerance = 0.0f;       // > 0：值域软权尺度（相对残差）
        float hard_cut = 0.0f;              // > 0：残差超过它 ⇒ 硬拒（跨剪影）
        float weight_floor = 0.0f;          // ≥ 0：非硬拒像素的权重下限（相对空间权）
    };

    // 本帧的输出（模块自持，别跨帧缓存）。
    struct Outputs {
        unsigned int reflection_tex = 0;  // RGBA32F：rgb = 辐亮度，a = 覆盖率
        unsigned int backface_tex = 0;    // R8UI：bit0
        int width = 0;
        int height = 0;
    };

    SsrUpsample();
    ~SsrUpsample();
    SsrUpsample(const SsrUpsample&) = delete;
    SsrUpsample& operator=(const SsrUpsample&) = delete;

    // Pre-condition: GL context 已激活。可重复调用（先内部 Finalize 再重建）。
    void Init(ShaderManager* shader_mgr);
    void Finalize();

    // 跑一趟。尺寸变化时内部重建目标（热路径零分配）；返回前恢复调用方的 GL 状态。
    const Outputs& Run(const Inputs& in);

private:
    void EnsureTargets(int w, int h);
    void DestroyTargets();

    ShaderManager* shader_mgr_ = nullptr;
    unsigned int prog_ = 0;
    unsigned int fbo_ = 0;
    Outputs outputs_;
    int width_ = 0;
    int height_ = 0;
};

}  // namespace jpov

#endif  // JPOV_SRC_SSR_SSR_UPSAMPLE_H_
