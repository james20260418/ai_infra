// JPOV StripRenderer 实现（1× WBO 覆盖集）。见 strip_renderer.h。

#define GL_GLEXT_PROTOTYPES

#include "tools/jpov/src/strip/strip_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
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

// 三次 Hermite（m0/m1 是 ∂P/∂t，t ∈ [0,1]）。平滑 pass 用它重建中心线与半宽矢量。
inline Vec3f Lerp(const Vec3f& a, const Vec3f& b, float t) {
    return Vec3f{a.x() + (b.x() - a.x()) * t, a.y() + (b.y() - a.y()) * t,
                 a.z() + (b.z() - a.z()) * t};
}

inline Vec3f Hermite(const Vec3f& p0, const Vec3f& m0, const Vec3f& p1,
                     const Vec3f& m1, float t) {
    const float t2 = t * t;
    const float t3 = t2 * t;
    const float h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;
    const float h10 = t3 - 2.0f * t2 + t;
    const float h01 = -2.0f * t3 + 3.0f * t2;
    const float h11 = t3 - t2;
    return Vec3f{h00 * p0.x() + h10 * m0.x() + h01 * p1.x() + h11 * m1.x(),
                 h00 * p0.y() + h10 * m0.y() + h01 * p1.y() + h11 * m1.y(),
                 h00 * p0.z() + h10 * m0.z() + h01 * p1.z() + h11 * m1.z()};
}

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

    // ── 平滑 pass（kSmooth）：program + 独立 VAO/VBO + 控制点数据纹理 ──
    // 与折线 pass 完全独立：另一个 program、另一套顶点布局、另一条数据通道；
    // 只共享 WBO 目标与静态索引。
    const std::string fs_smooth =
        std::string(kSmoothFsHead) + kWboWeightGlsl + kSmoothFsTail;
    prog_smooth_ = shader_mgr_->GetOrCreate("strip_wbo_smooth",
                                            {kSmoothVs, fs_smooth.c_str()});
    smooth_uniforms_.view_proj = shader_mgr_->GetUniform(prog_smooth_, "uViewProj");
    smooth_uniforms_.tint = shader_mgr_->GetUniform(prog_smooth_, "uTint");
    smooth_uniforms_.has_tex = shader_mgr_->GetUniform(prog_smooth_, "uHasTex");
    smooth_uniforms_.tex = shader_mgr_->GetUniform(prog_smooth_, "uTex");
    smooth_uniforms_.profile = shader_mgr_->GetUniform(prog_smooth_, "uProfile");
    smooth_uniforms_.scene_depth = shader_mgr_->GetUniform(prog_smooth_, "uSceneDepth");
    smooth_uniforms_.viewport = shader_mgr_->GetUniform(prog_smooth_, "uViewport");
    smooth_uniforms_.near_plane = shader_mgr_->GetUniform(prog_smooth_, "uNear");
    smooth_uniforms_.far_plane = shader_mgr_->GetUniform(prog_smooth_, "uFar");
    smooth_uniforms_.blend_path = shader_mgr_->GetUniform(prog_smooth_, "uBlendPath");
    smooth_uniforms_.soft_enabled =
        shader_mgr_->GetUniform(prog_smooth_, "uSoftDepthEnabled");
    smooth_uniforms_.soft_m = shader_mgr_->GetUniform(prog_smooth_, "uSoftDepthM");
    smooth_uniforms_.seg_count = shader_mgr_->GetUniform(prog_smooth_, "uSegCount");
    smooth_uniforms_.data_rows = shader_mgr_->GetUniform(prog_smooth_, "uDataRows");
    smooth_uniforms_.strip_data = shader_mgr_->GetUniform(prog_smooth_, "uStripData");

    glGenVertexArrays(1, &smooth_vao_);
    glGenBuffers(1, &smooth_vbo_);
    CHECK_NE(smooth_vao_, 0u);
    CHECK_NE(smooth_vbo_, 0u);
    glBindVertexArray(smooth_vao_);
    glBindBuffer(GL_ARRAY_BUFFER, smooth_vbo_);
    // 7 float/顶点：pos(3) + (u, v, alpha) + strip_id(1)
    const GLsizei smooth_stride = static_cast<GLsizei>(7 * sizeof(float));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, smooth_stride,
                          reinterpret_cast<const void*>(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, smooth_stride,
                          reinterpret_cast<const void*>(3 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, smooth_stride,
                          reinterpret_cast<const void*>(6 * sizeof(float)));
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo_);   // 与折线 pass 共用静态索引
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    glGenTextures(1, &smooth_data_tex_);
    CHECK_NE(smooth_data_tex_, 0u);
    glBindTexture(GL_TEXTURE_2D, smooth_data_tex_);
    // 宽度 = 每列 3 texel × 采样点数；高度 = 一个 draw 块最多多少条带。
    // 关 mipmap / NEAREST：texelFetch 本来就不走过滤（也避免被 TextureManager 的默认选项污染）。
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, kStripSampleCount * 3,
                 kMaxStripsPerDraw, 0, GL_RGBA, GL_FLOAT, nullptr);
    glBindTexture(GL_TEXTURE_2D, 0);

    smooth_vertices_.reserve(static_cast<size_t>(kMaxStripsPerDraw) *
                             kStripVerticesPerStrip * 7);
    smooth_data_.assign(static_cast<size_t>(kMaxStripsPerDraw) *
                            kStripSampleCount * 12, 0.0f);

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
    if (smooth_data_tex_ != 0) {
        glDeleteTextures(1, &smooth_data_tex_);
        smooth_data_tex_ = 0;
    }
    if (smooth_vbo_ != 0) {
        glDeleteBuffers(1, &smooth_vbo_);
        smooth_vbo_ = 0;
    }
    if (smooth_vao_ != 0) {
        glDeleteVertexArrays(1, &smooth_vao_);
        smooth_vao_ = 0;
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

    // 两个独立 drawer，写同一张 WBO 目标（命令顺序 = 前后顺序）。
    if (batch.style == StripStyle::kSmooth) {
        DrawSmooth(batch, gl_tex, want_additive, camera, view_proj,
                   scene_depth_tex, full_w, full_h);
    } else {
        DrawPolyline(batch, gl_tex, want_additive, camera, view_proj,
                     scene_depth_tex, full_w, full_h);
    }

    has_content_ = true;
    if (want_additive) {
        has_additive_ = true;
    } else {
        has_alpha_ = true;
    }
}

void StripRenderer::DrawPolyline(const TranslucentStripBatch& batch,
                                 unsigned int gl_tex, bool want_additive,
                                 const Camera& camera, const float view_proj[16],
                                 unsigned int scene_depth_tex,
                                 int full_w, int full_h) {
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

}

void StripRenderer::DrawSmooth(const TranslucentStripBatch& batch,
                               unsigned int gl_tex, bool want_additive,
                               const Camera& camera, const float view_proj[16],
                               unsigned int scene_depth_tex,
                               int full_w, int full_h) {
    glPushAttrib(GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    wbo_.BindForDraw();
    glDisable(GL_CULL_FACE);

    glUseProgram(prog_smooth_);
    glUniformMatrix4fv(smooth_uniforms_.view_proj, 1, GL_FALSE, view_proj);
    glUniform4f(smooth_uniforms_.tint, batch.tint.r, batch.tint.g, batch.tint.b,
                batch.tint.a);
    glUniform1fv(smooth_uniforms_.profile, 9, batch.width_profile.data());
    glUniform2f(smooth_uniforms_.viewport, static_cast<float>(full_w),
                static_cast<float>(full_h));
    glUniform1f(smooth_uniforms_.near_plane, camera.near);
    glUniform1f(smooth_uniforms_.far_plane, camera.far);
    glUniform1i(smooth_uniforms_.blend_path, want_additive ? 1 : 0);
    glUniform1f(smooth_uniforms_.seg_count, static_cast<float>(kStripQuads));
    glUniform1f(smooth_uniforms_.data_rows, static_cast<float>(kMaxStripsPerDraw));
    if (batch.soft_depth_m.has_value()) {
        glUniform1i(smooth_uniforms_.soft_enabled, 1);
        glUniform1f(smooth_uniforms_.soft_m, *batch.soft_depth_m);
    } else {
        glUniform1i(smooth_uniforms_.soft_enabled, 0);
        glUniform1f(smooth_uniforms_.soft_m, 1.0f);
    }

    glActiveTexture(GL_TEXTURE0 + kSceneDepthUnit);
    glBindTexture(GL_TEXTURE_2D, scene_depth_tex);
    glUniform1i(smooth_uniforms_.scene_depth, kSceneDepthUnit);
    if (gl_tex != 0) {
        glActiveTexture(GL_TEXTURE0 + kStripTexUnit);
        glBindTexture(GL_TEXTURE_2D, gl_tex);
        glUniform1i(smooth_uniforms_.tex, kStripTexUnit);
        glUniform1i(smooth_uniforms_.has_tex, 1);
    } else {
        glUniform1i(smooth_uniforms_.has_tex, 0);
    }
    // 数据纹理占 unit2（unit0 = 场景深度、unit1 = 条带纹理）。
    constexpr int kStripDataUnit = 2;
    glActiveTexture(GL_TEXTURE0 + kStripDataUnit);
    glBindTexture(GL_TEXTURE_2D, smooth_data_tex_);
    glUniform1i(smooth_uniforms_.strip_data, kStripDataUnit);

    glBindVertexArray(smooth_vao_);
    glBindBuffer(GL_ARRAY_BUFFER, smooth_vbo_);

    const size_t total = batch.strips.size();
    const int chunks =
        static_cast<int>((total + kMaxStripsPerDraw - 1) / kMaxStripsPerDraw);
    LOG_FIRST_N(INFO, 3) << "StripRenderer(smooth): strips=" << total
                         << " draw_calls=" << chunks;

    for (size_t begin = 0; begin < total; begin += kMaxStripsPerDraw) {
        const size_t count = std::min<size_t>(kMaxStripsPerDraw, total - begin);

        smooth_vertices_.clear();
        for (size_t k = 0; k < count; ++k) {
            const Strip16& strip = batch.strips[begin + k];
            const uint32_t last = strip.point_count - 1;

            // ① 控制点：中心 c（Hermite 的过点）与半宽矢量 h（未外扩的真实半宽）。
            //    尾部塌缩列复制最后一个有效点（与折线 pass 的塌缩规则一致）。
            Vec3f cen[kStripSampleCount];
            Vec3f half[kStripSampleCount];
            for (int col = 0; col < kStripSampleCount; ++col) {
                const uint32_t src = (static_cast<uint32_t>(col) < strip.point_count)
                                         ? static_cast<uint32_t>(col)
                                         : last;
                const Vec3f& l = strip.left[src];
                const Vec3f& r = strip.right[src];
                cen[col] = Vec3f{0.5f * (l.x() + r.x()), 0.5f * (l.y() + r.y()),
                                 0.5f * (l.z() + r.z())};
                half[col] = Vec3f{0.5f * (l.x() - r.x()), 0.5f * (l.y() - r.y()),
                                  0.5f * (l.z() - r.z())};
            }
            // ② 切线：中心差分，端点退到单侧（C1 的来源：相邻段共用同一列切线）。
            Vec3f tan_c[kStripSampleCount];
            Vec3f tan_h[kStripSampleCount];
            for (int col = 0; col < kStripSampleCount; ++col) {
                const int im = std::max(col - 1, 0);
                const int ip = std::min(col + 1, kStripSampleCount - 1);
                tan_c[col] = Vec3f{0.5f * (cen[ip].x() - cen[im].x()),
                                   0.5f * (cen[ip].y() - cen[im].y()),
                                   0.5f * (cen[ip].z() - cen[im].z())};
                tan_h[col] = Vec3f{0.5f * (half[ip].x() - half[im].x()),
                                   0.5f * (half[ip].y() - half[im].y()),
                                   0.5f * (half[ip].z() - half[im].z())};
            }

            // ③ 自动外扩量（**不是参数**）：片元只能削不能补，所以先把多边形按
            //    「真实曲线相对弦在宽度方向上的最大外凸量」撑大，再由 FS 用 v* 裁回去。
            //    按**绝对量（米）**算而不是比例：拖尾收窄到 0 的段落，比例会失真。
            //
            //    ⚠️ 必须是**整条带一个值**（不能逐列）：外扩被折进了顶点属性 v
            //    （见 ⑤），而 FS 假定 `position = lerp(c) + (2v−1)·lerp(h)`；
            //    只有 scale 在整条带上相同时，`lerp(scale·h) == scale·lerp(h)` 才成立，
            //    映射才精确。逐列不同会在窄处露亚像素缺口（实测可见毛边）。
            float margin = 0.0f;
            for (int seg = 0; seg < kStripQuads; ++seg) {
                const Vec3f& c0 = cen[seg];
                const Vec3f& c1 = cen[seg + 1];
                const Vec3f& m0 = tan_c[seg];
                const Vec3f& m1 = tan_c[seg + 1];
                const Vec3f& h0 = half[seg];
                const Vec3f& h1 = half[seg + 1];
                const Vec3f& n0 = tan_h[seg];
                const Vec3f& n1 = tan_h[seg + 1];
                // 等宽条带（拖尾/剑气最常见）：半宽矢量是常量 ⇒ 省掉一次 Hermite。
                const bool varying_half =
                    (h1.x() - h0.x()) * (h1.x() - h0.x()) +
                        (h1.y() - h0.y()) * (h1.y() - h0.y()) +
                        (h1.z() - h0.z()) * (h1.z() - h0.z()) > 1e-12f;
                for (int si = 1; si <= 7; ++si) {
                    const float t = 0.125f * static_cast<float>(si);
                    const Vec3f lc = Lerp(c0, c1, t);
                    const Vec3f lh = Lerp(h0, h1, t);
                    const float len = lh.Norm();
                    if (len < 1e-5f) {
                        continue;   // 退化段（宽度为 0）：没有"外侧"可言
                    }
                    const Vec3f sc = Hermite(c0, m0, c1, m1, t);
                    const Vec3f sh = varying_half ? Hermite(h0, n0, h1, n1, t) : h0;
                    const Vec3f w{lh.x() / len, lh.y() / len, lh.z() / len};
                    const Vec3f dc{sc.x() - lc.x(), sc.y() - lc.y(), sc.z() - lc.z()};
                    const Vec3f dh{sh.x() - lh.x(), sh.y() - lh.y(), sh.z() - lh.z()};
                    // 左轨（v=1）外法向 = +w；右轨（v=0）外法向 = −w。
                    const float dl = dc.x() * w.x() + dc.y() * w.y() + dc.z() * w.z();
                    const float dhw = dh.x() * w.x() + dh.y() * w.y() + dh.z() * w.z();
                    margin = std::max(margin, std::max(dl + dhw, -dl + dhw));
                }
            }
            // 采样是最值近似，再留 2% + 1mm 余量：宁可多撑一点（FS 会裁回去），
            // 也不要外弯侧漏出一条白边。
            margin = std::max(margin, 0.0f) * 1.02f + 0.001f;

            // 统一 scale（见上面的 ⚠️）。按**最细**的一列算，保证每一列都撑得住；
            // 上限 8×：再细的尖（< margin/7，通常已是亚像素）不值得为它铺满屏幕。
            float min_half = 1e30f;
            for (int col = 0; col < kStripSampleCount; ++col) {
                const float hl = half[col].Norm();
                if (hl > 1e-5f) {
                    min_half = std::min(min_half, hl);
                }
            }
            const float scale =
                (min_half < 1e30f) ? std::min(1.0f + margin / min_half, 8.0f) : 1.0f;
            LOG_FIRST_N(INFO, 6) << "StripSmooth: margin=" << margin
                                 << " scale=" << scale
                                 << " (min half width≈" << min_half << ")";

            // ④ 数据纹理：每列 12 float = (c.xyz, m.x)(m.yz, h.xy)(h.z, n.xyz)。
            float* row = &smooth_data_[(static_cast<size_t>(k) * kStripSampleCount) * 12];
            for (int col = 0; col < kStripSampleCount; ++col) {
                float* p = row + col * 12;
                p[0] = cen[col].x();
                p[1] = cen[col].y();
                p[2] = cen[col].z();
                p[3] = tan_c[col].x();
                p[4] = tan_c[col].y();
                p[5] = tan_c[col].z();
                p[6] = half[col].x();
                p[7] = half[col].y();
                p[8] = half[col].z();
                p[9] = tan_h[col].x();
                p[10] = tan_h[col].y();
                p[11] = tan_h[col].z();
            }

            // ⑤ 顶点：外扩沿半宽矢量（绝对量 margin），并把外扩**折进 v 属性** ——
            //    这样 FS 不需要任何 widen 参数：插值出来的 v 就是真实参数。
            for (int col = 0; col < kStripSampleCount; ++col) {
                const uint32_t src = (static_cast<uint32_t>(col) < strip.point_count)
                                         ? static_cast<uint32_t>(col)
                                         : last;
                const float u = kStripColumnU.v[col];
                const float alpha = strip.alpha[src];
                const Vec3f& c = cen[col];
                // scale 是整条带统一值（见 ③ 的说明），逐列只差 |h| 本身的长度。
                // 左右角对应的**真实参数**：右 = 0.5 − scale/2、左 = 0.5 + scale/2。
                const float v_right = 0.5f - 0.5f * scale;
                const float v_left = 0.5f + 0.5f * scale;
                const Vec3f& rp = strip.right[src];
                const Vec3f& lp = strip.left[src];
                for (int side = 0; side < 2; ++side) {
                    const Vec3f& e = (side == 0) ? rp : lp;
                    smooth_vertices_.push_back(c.x() + (e.x() - c.x()) * scale);
                    smooth_vertices_.push_back(c.y() + (e.y() - c.y()) * scale);
                    smooth_vertices_.push_back(c.z() + (e.z() - c.z()) * scale);
                    smooth_vertices_.push_back(u);
                    smooth_vertices_.push_back(side == 0 ? v_right : v_left);
                    smooth_vertices_.push_back(alpha);
                    smooth_vertices_.push_back(static_cast<float>(k));
                }
            }
        }

        // ⑥ 上传：顶点（orphan 整块重传）+ 数据纹理（只写用到的那几行）。
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(smooth_vertices_.size() * sizeof(float)),
                     smooth_vertices_.data(), GL_STREAM_DRAW);
        glActiveTexture(GL_TEXTURE0 + kStripDataUnit);
        glBindTexture(GL_TEXTURE_2D, smooth_data_tex_);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kStripSampleCount * 3,
                        static_cast<GLsizei>(count), GL_RGBA, GL_FLOAT,
                        smooth_data_.data());

        // ⑦ 整块一次 draw call（≤ kMaxStripsPerDraw 时就是 1 次）。
        glDrawElements(GL_TRIANGLES,
                       static_cast<GLsizei>(count) * kStripIndicesPerStrip,
                       GL_UNSIGNED_INT, reinterpret_cast<const void*>(0));
    }
    glBindVertexArray(0);

    wbo_.EndDraw();
    glActiveTexture(GL_TEXTURE0);
    glPopAttrib();
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
