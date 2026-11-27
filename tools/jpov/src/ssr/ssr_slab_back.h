// JPOV SSR — 「同面并集柱体」（SSR 分辨率的柱体 back 图）
//
// 为什么需要它（2026-11-12/13）：命中判据把每个 SSRec texel 读成"从前表面 z 朝远处挤出
// 厚度 T 的实心方柱"。但屏幕深度图**逐 texel 独立** ⇒ 一个连续平面在掠射下相邻两行的
// 深度能差几十个百分点，柱子之间就留出了**缝**：射线从缝里穿过去 ⇒ 本该命中一整个平面，
// 结果是周期性的梳齿。
//
// 做法（一次全屏 pass，SSR 分辨率）：
//   对每格，用**它自己的 (P, N)** 造切平面，预测 4 邻居格中心处的 w，残差小于门槛且法线
//   夹角够小 ⇒ 认为"同一张面"，把这一格的 back 往邻居的 front 那边拉（取 min ⇒ 只往
//   **厚**里加，永不变薄、永不造假近面）。
//   **必须双向判**（我判它 + 它判我）：单侧判会让"较近那一格"漏并，缝隙照样留着。
//
// 输出：R32F，每格 back（**w 空间**，0 = 没有并集信息）。这是命中判据的三个厚度源之一
// （见 ssr_shader.h::SsrCellHit），无条件参与。
//
// 位置的说明：它是**源几何**的准备步骤（只用源的 w/法线），与"谁在问"无关 ——
// 因此归 Source 一侧，由 SsrChain 在降采样之后、march 之前跑。

#ifndef JPOV_SRC_SSR_SSR_SLAB_BACK_H_
#define JPOV_SRC_SSR_SSR_SLAB_BACK_H_

namespace jpov {

class ShaderManager;

class SsrSlabBack {
public:
    // 一趟的全部输入（只读）。全部必填：0 = 未设置 ⇒ FATAL。
    // 数值参数越界（面板拖出了范围）会被**钳到合法区间**，不 FATAL —— 它们是观感旋钮，
    // 钳位的本意是"不要让 0 厚柱体/误判把整屏打成无命中"。
    struct Inputs {
        unsigned int w_tex = 0;       // R32F，SSR 分辨率 w = 1/z（0 = 天空）
        unsigned int normal_tex = 0;  // RGBA16F，SSR 分辨率视空间法线（rgb 三分量）
        int width = 0;                // > 0，= SSR 分辨率
        int height = 0;
        float min_thickness_rel = 0.0f;  // 同面并集的基线相对厚度，(0, 0.5]
        float gate_rel = 0.0f;           // 同面判定的深度残差门槛，(0, 1]
        float gate_cos = 0.0f;           // 同面判定的法线夹角门槛 [0, 1]
        float aspect = 0.0f;             // > 0
        float tan_half_fov_y = 0.0f;     // > 0
    };

    SsrSlabBack();
    ~SsrSlabBack();
    SsrSlabBack(const SsrSlabBack&) = delete;
    SsrSlabBack& operator=(const SsrSlabBack&) = delete;

    // Pre-condition: GL context 已激活。可重复调用（先内部 Finalize 再重建）。
    void Init(ShaderManager* shader_mgr);
    void Finalize();

    // 跑一趟，返回本帧的 back 纹理（R32F，尺寸 = width×height，模块自持）。
    // 尺寸变化时内部重建目标（热路径零分配）；返回前恢复调用方的 GL 状态。
    unsigned int Run(const Inputs& in);

private:
    void EnsureTarget(int w, int h);
    void DestroyTarget();

    ShaderManager* shader_mgr_ = nullptr;
    unsigned int prog_ = 0;
    unsigned int fbo_ = 0;
    unsigned int tex_ = 0;
    int width_ = 0;
    int height_ = 0;
};

}  // namespace jpov

#endif  // JPOV_SRC_SSR_SSR_SLAB_BACK_H_
