// JPOV WboBuffer —— 加权混合 OIT（McGuire 2013）三附件缓冲的**唯一**实现。
//
// 为什么抽出来：玻璃（1×）、广告牌粒子（1/2×）、体积火（1×）曾各自拷贝一份
// 「建三张纹理 + FBO + 三路 blend + clear」的代码，权重函数与混合状态必须逐字一致，
// 靠拷贝维持纪律迟早漂移。本类只负责**资源与 GL 状态**，不负责画什么、也不负责合成。
//
// 附件语义（三者同构）：
//   att0 accum : RGBA16F，Σ(预乘色×w, α×w)   blend (ONE, ONE)
//   att1 reveal: R16F   ，Π(1−α)             blend (ZERO, ONE_MINUS_SRC_COLOR)
//   att2 depth : R32F   ，Σ(z_view×α×w)      blend (ONE, ONE)
//
// 用法（调用方自己决定渲染什么）：
//   buf.Ensure(w, h);
//   buf.BindForDraw();       // 绑 FBO + viewport + 3 draw buffers + WBO 混合 + 关深度/裁剪
//   buf.Clear();             // accum=0 / reveal=1 / depth=0
//   ...（多次 draw 累加）...
//   buf.RestoreStandardBlend();
//   glBindFramebuffer(GL_FRAMEBUFFER, prev_fbo);   // 复原由调用方负责
//
// 与其他渲染器的关系：本类**不**保存/复原进入前的 GL 状态（FBO / viewport / blend），
// 由调用方按各渲染器的「GL 状态机契约」处理（同 fog / particle 的既有约定）。

#ifndef JPOV_SRC_WBO_BUFFER_H_
#define JPOV_SRC_WBO_BUFFER_H_

#include "tools/jpov/src/transparent_layer.h"

namespace jpov {

class WboBuffer {
public:
    WboBuffer() = default;
    ~WboBuffer();

    WboBuffer(const WboBuffer&) = delete;
    WboBuffer& operator=(const WboBuffer&) = delete;

    // 确保三附件 + FBO 存在且尺寸为 (w, h)；尺寸变化则重建（重建后内容未定义，须 Clear）。
    // Pre-condition: w > 0, h > 0。
    void Ensure(int w, int h);

    // 释放全部 GPU 资源（可重复调用）。
    void Destroy();

    bool valid() const { return fbo_ != 0; }
    int width() const { return width_; }
    int height() const { return height_; }

    // 绑定本 FBO + viewport(0,0,w,h) + 3 个 draw buffer + WBO 混合状态 + 关深度/裁剪。
    // Pre-condition: valid()。
    void BindForDraw() const;

    // 清空为恒等层：accum=(0,0,0,0)、reveal=1、depth=0。
    // Pre-condition: 已 BindForDraw（作用于当前绑定 FBO 的 draw buffer 0/1/2）。
    void Clear() const;

    // 把 3 个 draw buffer 的 blend 复原成标准 alpha 混合（SRC_ALPHA/ONE_MINUS_SRC_ALPHA）。
    // blend func 属 GL_COLOR_BUFFER_BIT，不在 glPushAttrib(GL_ENABLE_BIT) 里 ⇒ 必须显式复原，
    // 否则会沿用到后续 2D / 字体绘制。
    void RestoreStandardBlend() const;

    // 把三张附件绑到连续纹理单元 [first_unit, first_unit+2]。
    // Pre-condition: first_unit + 2 < GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS。
    void BindTextures(unsigned int first_unit) const;

    // 导出为合成器契约（scale 由调用方按自己的解算分辨率填写）。
    TransparentLayer layer() const;

private:
    unsigned int fbo_ = 0;
    unsigned int accum_tex_ = 0;
    unsigned int reveal_tex_ = 0;
    unsigned int depth_tex_ = 0;
    int width_ = 0;
    int height_ = 0;
};

}  // namespace jpov

#endif  // JPOV_SRC_WBO_BUFFER_H_
