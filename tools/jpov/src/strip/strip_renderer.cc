// JPOV StripRenderer 实现（1× WBO 覆盖集）。见 strip_renderer.h。

#define GL_GLEXT_PROTOTYPES

#include "tools/jpov/src/strip/strip_renderer.h"

#include <algorithm>
#include <cmath>
#include <string>

#include <GL/gl.h>
#include <GL/glext.h>

#ifdef _WIN32
#ifndef GLOG_NO_ABBREVIATED_SEVERITIES
#define GLOG_NO_ABBREVIATED_SEVERITIES
#endif
#include "third_party/gl_loader-mingw/gl_loader.h"
// windef.h 把 near / far 定义成**空宏**（16 位分段时代的遗留），会让
// `camera.near` / `camera.far` 直接展开成 `camera.` → 「expected unqualified-id」。
// 同 particle_renderer.cc 的处理：在这里撤掉。
#undef near
#undef far
#endif

#include <glog/logging.h>

#include "tools/jpov/src/wbo/wbo_shader.h"

namespace jpov {

namespace {

// 条带 pass 是独立 pass（不透明 3D 之后），单元 0 = 场景深度、单元 1 = 条带纹理，
// 与粒子 pass 同样的划分。
constexpr int kSceneDepthUnit = 0;
constexpr int kStripTexUnit = 1;

}  // namespace

void StripRenderer::Init(ShaderManager* shader_mgr) {
    CHECK(shader_mgr != nullptr) << "StripRenderer::Init: shader_mgr 不能为空";
    CHECK(!initialized_) << "StripRenderer::Init 重复调用";
    shader_mgr_ = shader_mgr;

    // 权重函数必须来自共享件（wbo_shader.h）—— 与粒子/体积火/玻璃是同一份数学。
    const std::string fs_draw =
        std::string(kFsHead) + kWboWeightGlsl + kFsTail;
    prog_draw_ = shader_mgr_->GetOrCreate("strip_wbo_draw", {kVs, fs_draw.c_str()});
    prog_comp_ = shader_mgr_->GetOrCreate("strip_wbo_composite",
                                          {kWboCompositeVs, kWboCompositeFs});

    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    glGenBuffers(1, &ibo_);
    CHECK_NE(vao_, 0u);
    CHECK_NE(vbo_, 0u);
    CHECK_NE(ibo_, 0u);

    // 顶点布局固定为 StripVertex（6 个连续 float）；VBO 每帧重传，但绑定关系不变。
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(StripVertex),
                          reinterpret_cast<const void*>(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(StripVertex),
                          reinterpret_cast<const void*>(3 * sizeof(float)));

    // 静态索引：kMaxStripsPerDraw 个固定块（块 k 的顶点基址 = k * 32）。
    std::vector<uint32_t> indices;
    indices.reserve(static_cast<size_t>(kMaxStripsPerDraw) * kStripIndicesPerStrip);
    for (int block = 0; block < kMaxStripsPerDraw; ++block) {
        AppendStripIndicesForBlock(
            static_cast<uint32_t>(block * kStripVerticesPerStrip), &indices);
    }
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo_);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(indices.size() * sizeof(uint32_t)),
                 indices.data(), GL_STATIC_DRAW);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    // 顶点缓冲按「一个块」预留容量：避免首帧/扩容时的 vector 重分配。
    // （VBO 本身仍是每帧 glBufferData 重传 —— 见 Draw 里的注释。）
    vertices_.reserve(static_cast<size_t>(kMaxStripsPerDraw) * kStripVerticesPerStrip);

    // uniform 位置只查一次（见头文件里的 DrawUniforms 注释）。
    draw_uniforms_.view_proj = shader_mgr_->GetUniform(prog_draw_, "uViewProj");
    draw_uniforms_.tint = shader_mgr_->GetUniform(prog_draw_, "uTint");
    draw_uniforms_.has_tex = shader_mgr_->GetUniform(prog_draw_, "uHasTex");
    draw_uniforms_.tex = shader_mgr_->GetUniform(prog_draw_, "uTex");
    draw_uniforms_.profile = shader_mgr_->GetUniform(prog_draw_, "uProfile");
    draw_uniforms_.scene_depth = shader_mgr_->GetUniform(prog_draw_, "uSceneDepth");
    draw_uniforms_.viewport = shader_mgr_->GetUniform(prog_draw_, "uViewport");
    draw_uniforms_.near_plane = shader_mgr_->GetUniform(prog_draw_, "uNear");
    draw_uniforms_.far_plane = shader_mgr_->GetUniform(prog_draw_, "uFar");
    draw_uniforms_.blend_path = shader_mgr_->GetUniform(prog_draw_, "uBlendPath");
    draw_uniforms_.soft_enabled =
        shader_mgr_->GetUniform(prog_draw_, "uSoftDepthEnabled");
    draw_uniforms_.soft_m = shader_mgr_->GetUniform(prog_draw_, "uSoftDepthM");
    comp_uniforms_.accum = shader_mgr_->GetUniform(prog_comp_, "uAccum");
    comp_uniforms_.reveal = shader_mgr_->GetUniform(prog_comp_, "uReveal");
    comp_uniforms_.add = shader_mgr_->GetUniform(prog_comp_, "uAdd");
    comp_uniforms_.additive = shader_mgr_->GetUniform(prog_comp_, "uAdditive");

    initialized_ = true;
}

void StripRenderer::Finalize() {
    wbo_.Destroy();
    if (ibo_ != 0) {
        glDeleteBuffers(1, &ibo_);
        ibo_ = 0;
    }
    if (vbo_ != 0) {
        glDeleteBuffers(1, &vbo_);
        vbo_ = 0;
    }
    if (vao_ != 0) {
        glDeleteVertexArrays(1, &vao_);
        vao_ = 0;
    }
    initialized_ = false;
    has_content_ = false;
}

void StripRenderer::EnsureTarget(int w, int h, bool with_additive) {
    wbo_.Ensure({w, h, /*with_additive=*/with_additive, /*with_depth=*/false,
                 /*linear_filter=*/false});
    target_with_additive_ = with_additive;
}

void StripRenderer::Begin(int full_w, int full_h, bool with_additive) {
    CHECK(initialized_) << "StripRenderer::Begin: 未 Init";
    CHECK_GT(full_w, 0);
    CHECK_GT(full_h, 0);
    EnsureTarget(full_w, full_h, with_additive);
    wbo_.Clear();
    has_content_ = false;
    has_alpha_ = false;
    has_additive_ = false;
}

void StripRenderer::Draw(const TranslucentStripBatch& batch,
                         TextureManager& texture_mgr,
                         const Camera& camera,
                         const float view_proj[16],
                         unsigned int scene_depth_tex,
                         int full_w, int full_h) {
    CHECK(initialized_) << "StripRenderer::Draw: 未 Init";
    CHECK_NE(scene_depth_tex, 0u) << "StripRenderer::Draw: scene_depth_tex 不能为 0";

    // 全部校验放在任何 GL 调用之前：非法输入在改状态前就崩掉。
    ValidateStripBatch(batch);
    // 注意：这里**不再** EnsureTarget。尺寸由 Begin 定；若这里按 full_w/h 重新 Ensure，
    // 尺寸不一致时会重建目标并把本帧已累积的内容清掉（静默丢内容）。
    CHECK_EQ(wbo_.width(), full_w) << "StripRenderer::Draw: 与 Begin 的宽度不一致";
    CHECK_EQ(wbo_.height(), full_h) << "StripRenderer::Draw: 与 Begin 的高度不一致";

    // 加法批必须有加法集附件（否则 att2 被 GL_NONE 丢弃 → 光静默消失）；
    // 反过来不成立：目标带加法集时，覆盖批照常写 att0/1、把 att2 写恒等值。
    const bool want_additive = (batch.blend == StripBlend::kEmissive);
    CHECK(!want_additive || target_with_additive_)
        << "StripRenderer::Draw: 本批是 kEmissive，但 Begin(with_additive=false) "
           "没申请加法集附件（光会被静默丢弃）";

    unsigned int gl_tex = 0;
    if (batch.texture_id != 0) {
        gl_tex = texture_mgr.GetGLTexture(batch.texture_id);
        CHECK_NE(gl_tex, 0u) << "StripRenderer::Draw: texture_id " << batch.texture_id
                             << " 未注册";
    }

    // 本 pass 改 enable / blend / 渲染目标：三组状态都入栈。
    glPushAttrib(GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    wbo_.BindForDraw();
    glDisable(GL_CULL_FACE);   // 条带单层双面可见（关剔除；不是两层几何）

    glUseProgram(prog_draw_);
    glUniformMatrix4fv(draw_uniforms_.view_proj, 1, GL_FALSE, view_proj);
    glUniform4f(draw_uniforms_.tint, batch.tint.r, batch.tint.g, batch.tint.b,
                batch.tint.a);
    glUniform1fv(draw_uniforms_.profile, 9, batch.width_profile.data());
    glUniform2f(draw_uniforms_.viewport, static_cast<float>(full_w),
                static_cast<float>(full_h));
    glUniform1f(draw_uniforms_.near_plane, camera.near);
    glUniform1f(draw_uniforms_.far_plane, camera.far);
    glUniform1i(draw_uniforms_.blend_path, want_additive ? 1 : 0);
    if (batch.soft_depth_m.has_value()) {
        glUniform1i(draw_uniforms_.soft_enabled, 1);
        glUniform1f(draw_uniforms_.soft_m, *batch.soft_depth_m);
    } else {
        glUniform1i(draw_uniforms_.soft_enabled, 0);
        // 关闭时也给一个合法值：避免读未初始化 uniform（FS 里该分支不取它）。
        glUniform1f(draw_uniforms_.soft_m, 1.0f);
    }

    glActiveTexture(GL_TEXTURE0 + kSceneDepthUnit);
    glBindTexture(GL_TEXTURE_2D, scene_depth_tex);
    glUniform1i(draw_uniforms_.scene_depth, kSceneDepthUnit);

    if (gl_tex != 0) {
        glActiveTexture(GL_TEXTURE0 + kStripTexUnit);
        glBindTexture(GL_TEXTURE_2D, gl_tex);
        glUniform1i(draw_uniforms_.tex, kStripTexUnit);
        glUniform1i(draw_uniforms_.has_tex, 1);
    } else {
        glUniform1i(draw_uniforms_.has_tex, 0);
    }

    // 一次 draw call 画完整批；超过 kMaxStripsPerDraw 才分块（不截断）。
    const size_t total = batch.strips.size();
    const int chunks =
        static_cast<int>((total + kMaxStripsPerDraw - 1) / kMaxStripsPerDraw);
    // 只在头几帧打一条（交互模式 60fps 不能每帧刷屏）：给验收提供「一次 draw call」的证据。
    LOG_FIRST_N(INFO, 3) << "StripRenderer: strips=" << total
                         << " draw_calls=" << chunks;
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    for (size_t begin = 0; begin < total; begin += kMaxStripsPerDraw) {
        const size_t count = std::min<size_t>(kMaxStripsPerDraw, total - begin);

        vertices_.clear();
        vertices_.reserve(count * kStripVerticesPerStrip);
        for (size_t k = begin; k < begin + count; ++k) {
            ExpandStripVertices(batch.strips[k], &vertices_);
        }
        // GL_STREAM_DRAW：每帧重写一次（语义比 DYNAMIC 更准，驱动可据此选择
        // 更适合流式写入的存放方式）。不用 glBufferSubData：那需要预先分配 +
        // 复用同一块存储，GPU 仍在读上一帧内容时会产生同步等待（stall）；
        // glBufferData 会 orphan 旧存储，天然避免这个坑。
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(vertices_.size() * sizeof(StripVertex)),
                     vertices_.data(), GL_STREAM_DRAW);
        // 每块都从索引缓冲的 0 号块开始（顶点数据也从头上传），所以无需 base vertex。
        glDrawElements(GL_TRIANGLES,
                       static_cast<GLsizei>(count) * kStripIndicesPerStrip,
                       GL_UNSIGNED_INT, reinterpret_cast<const void*>(0));
    }
    glBindVertexArray(0);

    wbo_.EndDraw();                 // 还原 blend + 解绑 FBO
    glActiveTexture(GL_TEXTURE0);   // 归位：调用方后续按「unit 0 已激活」写 pass
    glPopAttrib();

    has_content_ = true;
    if (want_additive) {
        has_additive_ = true;
    } else {
        has_alpha_ = true;
    }
}

void StripRenderer::Composite(int full_w, int full_h) {
    if (!has_content_ || !wbo_.valid()) {
        return;
    }
    // 本 pass 改 enable + blend func/equation：分属两个 attrib 组，必须一起入栈。
    glPushAttrib(GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT);
    glViewport(0, 0, full_w, full_h);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);

    glUseProgram(prog_comp_);
    glBindVertexArray(vao_);

    // ① 覆盖集：C = accum.rgb/accum.a、cov = 1−reveal，预乘 over 进目标。
    if (has_alpha_) {
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glActiveTexture(GL_TEXTURE0 + 0);
        glBindTexture(GL_TEXTURE_2D, wbo_.accum_tex());
        glUniform1i(comp_uniforms_.accum, 0);
        glActiveTexture(GL_TEXTURE0 + 1);
        glBindTexture(GL_TEXTURE_2D, wbo_.reveal_tex());
        glUniform1i(comp_uniforms_.reveal, 1);
        glUniform1i(comp_uniforms_.additive, 0);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    // ② 加法集：直接求和到目标之上（发光介质不被覆盖集衰减 —— 单层 OIT 的固有近似）。
    if (has_additive_) {
        glBlendFunc(GL_ONE, GL_ONE);
        glActiveTexture(GL_TEXTURE0 + 0);
        glBindTexture(GL_TEXTURE_2D, wbo_.additive_tex());
        glUniform1i(comp_uniforms_.add, 0);
        glUniform1i(comp_uniforms_.additive, 1);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    glBindVertexArray(0);
    glActiveTexture(GL_TEXTURE0);
    glPopAttrib();          // 还原 enable / blend（含 equation）
    has_content_ = false;   // 本帧消费掉
    has_alpha_ = false;
    has_additive_ = false;
}

}  // namespace jpov
