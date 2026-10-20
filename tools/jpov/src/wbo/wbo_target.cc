// JPOV WBO 目标实现。见 wbo_target.h。

#define GL_GLEXT_PROTOTYPES

#include "tools/jpov/src/wbo/wbo_target.h"

#include <GL/gl.h>
#include <GL/glext.h>

#ifdef _WIN32
#ifndef GLOG_NO_ABBREVIATED_SEVERITIES
#define GLOG_NO_ABBREVIATED_SEVERITIES
#endif
#include "third_party/gl_loader-mingw/gl_loader.h"
#endif

#include <glog/logging.h>

namespace jpov {

bool WboTarget::SlotRequested(int slot) const {
    switch (slot) {
        case kWboSlotAccum:
        case kWboSlotReveal:
            return true;
        case kWboSlotAdditive:
            return spec_.with_additive;
        case kWboSlotDepth:
            return spec_.with_depth;
        default:
            return false;
    }
}

void WboTarget::CreateTexture(int slot, unsigned int internal_format, unsigned int format,
                              bool linear_filter) {
    glGenTextures(1, &slot_tex_[slot]);
    glBindTexture(GL_TEXTURE_2D, slot_tex_[slot]);
    glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(internal_format),
                 spec_.width, spec_.height, 0, format, GL_FLOAT, nullptr);
    const GLint filter = linear_filter ? GL_LINEAR : GL_NEAREST;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void WboTarget::Ensure(const WboTargetSpec& spec) {
    if (fbo_ != 0 && spec_.width == spec.width && spec_.height == spec.height &&
        spec_.with_additive == spec.with_additive && spec_.with_depth == spec.with_depth &&
        spec_.linear_filter == spec.linear_filter) {
        return;
    }

    CHECK_GT(spec.width, 0) << "WboTarget::Ensure: width 必须 > 0";
    CHECK_GT(spec.height, 0) << "WboTarget::Ensure: height 必须 > 0";

    {
        GLint max_tex = 0;
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_tex);
        CHECK_LE(spec.width, max_tex) << "WboTarget::Ensure: width 超过 GL_MAX_TEXTURE_SIZE";
        CHECK_LE(spec.height, max_tex) << "WboTarget::Ensure: height 超过 GL_MAX_TEXTURE_SIZE";
    }

    Destroy();
    // ⚠️ 必须在 CreateTexture 之前写入 spec_：纹理尺寸/过滤都从 spec_ 读，
    //    先建后写会拿到 0×0（glTexImage2D 空图 → FBO_INCOMPLETE_ATTACHMENT）。
    spec_ = spec;

    // 覆盖集两件套恒存在。
    CreateTexture(kWboSlotAccum, GL_RGBA16F, GL_RGBA, spec.linear_filter);
    CreateTexture(kWboSlotReveal, GL_R16F, GL_RED, spec.linear_filter);
    if (spec.with_additive) {
        CreateTexture(kWboSlotAdditive, GL_RGBA16F, GL_RGBA, spec.linear_filter);
    }
    if (spec.with_depth) {
        // 深度恒 NEAREST（标量代表深度，插值没有物理意义）。
        CreateTexture(kWboSlotDepth, GL_R32F, GL_RED, false);
    }

    glGenFramebuffers(1, &fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);

    GLenum bufs[kWboSlotCount] = {GL_NONE, GL_NONE, GL_NONE, GL_NONE};
    for (int slot = 0; slot < kWboSlotCount; ++slot) {
        if (!SlotRequested(slot)) {
            continue;
        }
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + slot,
                               GL_TEXTURE_2D, slot_tex_[slot], 0);
        bufs[slot] = GL_COLOR_ATTACHMENT0 + slot;
    }
    glDrawBuffers(kWboSlotCount, bufs);

    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    CHECK_EQ(status, GL_FRAMEBUFFER_COMPLETE)
        << "WboTarget FBO incomplete, status=" << status;
}

void WboTarget::Destroy() {
    if (fbo_ != 0) {
        glDeleteFramebuffers(1, &fbo_);
        fbo_ = 0;
    }
    for (int slot = 0; slot < kWboSlotCount; ++slot) {
        if (slot_tex_[slot] != 0) {
            glDeleteTextures(1, &slot_tex_[slot]);
            slot_tex_[slot] = 0;
        }
    }
    spec_ = WboTargetSpec{};
}

void WboTarget::Clear() {
    CHECK(valid()) << "WboTarget::Clear: 未 Ensure";
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);

    // 恒等值：accum=(0,0,0,0)（Σ 初值）、reveal=(1,0,0,0)（Π(1−α) 初值）、其余 0。
    const GLfloat zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    const GLfloat one[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    glClearBufferfv(GL_COLOR, kWboSlotAccum, zero);
    glClearBufferfv(GL_COLOR, kWboSlotReveal, one);
    if (spec_.with_additive) {
        glClearBufferfv(GL_COLOR, kWboSlotAdditive, zero);
    }
    if (spec_.with_depth) {
        glClearBufferfv(GL_COLOR, kWboSlotDepth, zero);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void WboTarget::BindForDraw() {
    CHECK(valid()) << "WboTarget::BindForDraw: 未 Ensure";
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glViewport(0, 0, spec_.width, spec_.height);

    // WBO 目标没有深度缓冲：深度可见性由 FS 手动采样场景深度决定。
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);

    glBlendFunci(kWboSlotAccum, GL_ONE, GL_ONE);                    // accum += 预乘色×w
    glBlendFunci(kWboSlotReveal, GL_ZERO, GL_ONE_MINUS_SRC_COLOR);  // reveal *= (1−α)
    if (spec_.with_additive) {
        glBlendFunci(kWboSlotAdditive, GL_ONE, GL_ONE);             // additive += 色·α
    }
    if (spec_.with_depth) {
        glBlendFunci(kWboSlotDepth, GL_ONE, GL_ONE);                // depth += z·α·w
    }
}

void WboTarget::EndDraw() {
    // 四个槽位都回写默认混合：本 pass 用过的非常规 blend 不能泄漏给后续 pass。
    for (int slot = 0; slot < kWboSlotCount; ++slot) {
        glBlendFunci(slot, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }
    glBlendEquation(GL_FUNC_ADD);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

}  // namespace jpov
