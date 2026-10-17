// JPOV WboBuffer 实现。见 wbo_buffer.h。

#define GL_GLEXT_PROTOTYPES

#include "tools/jpov/src/wbo_buffer.h"

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

namespace {

// 三附件的 draw buffer 顺序（att0/1/2 = accum/reveal/depth）。
const GLenum kDrawBufs[3] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1,
                             GL_COLOR_ATTACHMENT2};

}  // namespace

WboBuffer::~WboBuffer() {
    Destroy();
}

void WboBuffer::Ensure(int w, int h) {
    CHECK_GT(w, 0);
    CHECK_GT(h, 0);
    if (fbo_ != 0 && width_ == w && height_ == h) {
        return;
    }
    Destroy();

    // att0 accum：RGBA16F。LINEAR —— 低分缓冲升到主视口时做双线性（1× 时无影响）。
    glGenTextures(1, &accum_tex_);
    glBindTexture(GL_TEXTURE_2D, accum_tex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // att1 reveal：R16F。
    glGenTextures(1, &reveal_tex_);
    glBindTexture(GL_TEXTURE_2D, reveal_tex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R16F, w, h, 0, GL_RED, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // att2 depth：R32F。NEAREST —— 代表深度不插值（插值会跨轮廓造出假深度）。
    glGenTextures(1, &depth_tex_);
    glBindTexture(GL_TEXTURE_2D, depth_tex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, w, h, 0, GL_RED, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenFramebuffers(1, &fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           accum_tex_, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D,
                           reveal_tex_, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_TEXTURE_2D,
                           depth_tex_, 0);
    glDrawBuffers(3, kDrawBufs);
    CHECK_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), GL_FRAMEBUFFER_COMPLETE)
        << "WboBuffer: FBO incomplete (" << w << "x" << h << ")";

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    width_ = w;
    height_ = h;
}

void WboBuffer::Destroy() {
    if (fbo_ != 0) {
        glDeleteFramebuffers(1, &fbo_);
        fbo_ = 0;
    }
    if (accum_tex_ != 0) {
        glDeleteTextures(1, &accum_tex_);
        accum_tex_ = 0;
    }
    if (reveal_tex_ != 0) {
        glDeleteTextures(1, &reveal_tex_);
        reveal_tex_ = 0;
    }
    if (depth_tex_ != 0) {
        glDeleteTextures(1, &depth_tex_);
        depth_tex_ = 0;
    }
    width_ = 0;
    height_ = 0;
}

void WboBuffer::BindForDraw() const {
    CHECK(valid()) << "WboBuffer::BindForDraw: Ensure 未调用";
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glViewport(0, 0, width_, height_);
    glDrawBuffers(3, kDrawBufs);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunci(0, GL_ONE, GL_ONE);                    // accum += 预乘色×w
    glBlendFunci(1, GL_ZERO, GL_ONE_MINUS_SRC_COLOR);   // reveal *= (1−α)
    glBlendFunci(2, GL_ONE, GL_ONE);                    // depth += z_view×α×w
}

void WboBuffer::Clear() const {
    CHECK(valid()) << "WboBuffer::Clear: Ensure 未调用";
    const GLfloat clear_accum[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    const GLfloat clear_reveal[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    const GLfloat clear_depth[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    glClearBufferfv(GL_COLOR, 0, clear_accum);
    glClearBufferfv(GL_COLOR, 1, clear_reveal);
    glClearBufferfv(GL_COLOR, 2, clear_depth);
}

void WboBuffer::RestoreStandardBlend() const {
    glBlendFunci(0, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBlendFunci(1, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBlendFunci(2, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBlendEquation(GL_FUNC_ADD);
}

void WboBuffer::BindTextures(unsigned int first_unit) const {
    CHECK(valid()) << "WboBuffer::BindTextures: Ensure 未调用";
    glActiveTexture(GL_TEXTURE0 + first_unit + 0);
    glBindTexture(GL_TEXTURE_2D, accum_tex_);
    glActiveTexture(GL_TEXTURE0 + first_unit + 1);
    glBindTexture(GL_TEXTURE_2D, reveal_tex_);
    glActiveTexture(GL_TEXTURE0 + first_unit + 2);
    glBindTexture(GL_TEXTURE_2D, depth_tex_);
}

TransparentLayer WboBuffer::layer() const {
    TransparentLayer out;
    out.accum_tex = accum_tex_;
    out.reveal_tex = reveal_tex_;
    out.depth_tex = depth_tex_;
    return out;
}

}  // namespace jpov
