// JPOV WBO 目标（Weighted Blended OIT）—— FBO / 附件 / clear / draw 状态的唯一实现
//
// 为什么有这个文件（2026-10-20 重构）：
//   在此之前，同一套 McGuire WBO 代数被手抄了三份（particles / volume_fire /
//   object3d 玻璃），每份各自管理四件事：
//     ① 附件格式与过滤（accum RGBA16F / reveal R16F / additive RGBA16F / depth R32F）
//     ② clear 值（accum=0 / reveal=1 / additive=0 / depth=0）
//     ③ 逐 draw buffer 的 blend（accum ONE,ONE / reveal ZERO,1−SRC / additive·depth ONE,ONE）
//     ④ 用完把 blend **显式还原**（GL_COLOR_BUFFER_BIT 的 pop 不保证覆盖 blend func）
//   这四件事抄错任何一处都是「画面悄悄错、gold 未必红」的 bug；第 ④ 条已经以
//   「忘记恢复 blend → 后续 2D/UI 整块变白」的形式在 bloom 上真发生过（PR #204）。
//   收敛成一个类之后：同一处改、同一处测。
//
//   **附件按需申请**（WboTargetSpec 的两个 bool）：不申请就没有对应纹理，
//   从结构上消灭「申请了却从没人读」的死附件 —— volume_fire 曾经就挂着一个
//   从来没人采样的 depth 附件（PR #208 一并删除）。
//
// 槽位是**固定**的（kWboSlot*），不随使用者变化：
//
//   att0 accum    RGBA16F   Σ(w·色·α)
//   att1 reveal   R16F      Π(1−α)
//   att2 additive RGBA16F   Σ(色·α)     （可选）
//   att3 depth    R32F      Σ(z·α·w)    （可选）
//
// 未申请的槽位在 glDrawBuffers 里写 GL_NONE：FS 里对应 location 的写入被丢弃（合法），
// 所以同一个 FS 可以复用到不同附件组合的目标上（不用的附件写恒等值）。
//
// 生命周期：Ensure()（幂等，配置变化才重建）→ Clear() → [BindForDraw() ... EndDraw()]* →
// Destroy()（必须在 GL context 存活时调用，例如渲染器的 Finalize()）。

#ifndef JPOV_SRC_WBO_WBO_TARGET_H_
#define JPOV_SRC_WBO_WBO_TARGET_H_

namespace jpov {

// 固定槽位。改这里 = 改所有 WBO 使用者的 FS location 与 blend 索引，不要随意动。
inline constexpr int kWboSlotAccum = 0;
inline constexpr int kWboSlotReveal = 1;
inline constexpr int kWboSlotAdditive = 2;
inline constexpr int kWboSlotDepth = 3;
inline constexpr int kWboSlotCount = 4;

struct WboTargetSpec {
    int width = 0;
    int height = 0;

    // 是否申请加法集附件（att2）。只有会写「发光体求和」的层才需要。
    bool with_additive = false;

    // 是否申请代表深度附件（att3）。只有要和雾/后续 pass 做逐像素深度合成的层才需要
    // （目前只有 object3d 玻璃用）。**没有任何采样者的深度附件不要申请。**
    bool with_depth = false;

    // accum/reveal/additive 是否线性过滤。
    //   低分辨率层（粒子 1/2）要靠 LINEAR 上采样，否则色块；
    //   1× 层用 NEAREST：省带宽，且避免在 1× 上做无意义的插值。
    // 深度附件恒 NEAREST（它存的是标量代表深度，插值没有物理意义）。
    bool linear_filter = true;
};

class WboTarget {
public:
    WboTarget() = default;
    ~WboTarget() = default;   // 不在这里释放：GL 资源必须在 context 存活时显式 Destroy()

    WboTarget(const WboTarget&) = delete;
    WboTarget& operator=(const WboTarget&) = delete;

    // 确保目标存在且配置与 spec 一致；不一致（含尺寸变化）则重建并清空。
    // Pre-condition: width/height > 0 且不超过 GL_MAX_TEXTURE_SIZE；GL context 已激活。
    void Ensure(const WboTargetSpec& spec);

    // 释放全部 GL 资源并回到未初始化状态。可重复调用。
    void Destroy();

    // 清成 WBO 恒等值：accum=(0,0,0,0)、reveal=(1,0,0,0)、additive/depth=(0,0,0,0)。
    // Pre-condition: 已 Ensure。
    void Clear();

    // 绑 FBO + viewport + 关深度测试 + 开混合 + 设好每个附件的 blend func。
    // **不做**的事（由调用方按自己的语义处理）：cull face、depth mask、纹理绑定、program。
    // Pre-condition: 已 Ensure。
    void BindForDraw();

    // 解绑并把 blend func 还原成引擎默认的 (SRC_ALPHA, ONE_MINUS_SRC_ALPHA)（四个槽位都写）。
    // 为什么要显式还原：非常规混合（ZERO/ONE_MINUS_SRC_COLOR 等）属于
    // GL_COLOR_BUFFER_BIT，glPopAttrib 并不保证覆盖 blend func（实测踩过）。
    void EndDraw();

    bool valid() const { return fbo_ != 0; }
    int width() const { return spec_.width; }
    int height() const { return spec_.height; }

    // 附件句柄；未申请返回 0。
    unsigned int fbo() const { return fbo_; }
    unsigned int accum_tex() const { return slot_tex_[kWboSlotAccum]; }
    unsigned int reveal_tex() const { return slot_tex_[kWboSlotReveal]; }
    unsigned int additive_tex() const { return slot_tex_[kWboSlotAdditive]; }
    unsigned int depth_tex() const { return slot_tex_[kWboSlotDepth]; }

private:
    void CreateTexture(int slot, unsigned int internal_format, unsigned int format,
                       bool linear_filter);
    bool SlotRequested(int slot) const;

    WboTargetSpec spec_;
    unsigned int fbo_ = 0;
    unsigned int slot_tex_[kWboSlotCount] = {0, 0, 0, 0};
};

}  // namespace jpov

#endif  // JPOV_SRC_WBO_WBO_TARGET_H_
