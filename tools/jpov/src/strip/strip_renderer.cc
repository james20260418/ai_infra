// JPOV StripRenderer 实现（1× WBO 覆盖集）。见 strip_renderer.h。

#define GL_GLEXT_PROTOTYPES

#include "tools/jpov/src/strip/strip_renderer.h"

#include <algorithm>
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
// 平滑 pass 的控制点数据纹理（RGBA32F）。**pass-local**：本 pass 与其他 pass 不同时活跃，
// 所以不占 texture_units.h 的跨 pass 区间（那张表里 unit 2 是材质 roughness）。
constexpr int kSmoothDataUnit = 2;
// 逐条带 tint（RGBA32F，1 texel / 条带）。同样 pass-local。
constexpr int kStripTintUnit = 3;

}  // namespace

void StripRenderer::Init(ShaderManager* shader_mgr) {
    CHECK(shader_mgr != nullptr) << "StripRenderer::Init: shader_mgr 不能为空";
    CHECK(!initialized_) << "StripRenderer::Init 重复调用";
    shader_mgr_ = shader_mgr;

    // 权重函数必须来自共享件（wbo_shader.h）—— 与粒子/体积火/玻璃是同一份数学。
    const std::string fs_draw =
        std::string(kCommonFsHead) + kWboWeightGlsl + kFsEmit + kFsMain;
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

    // 索引：先建 kMaxStripsPerDraw 块（块 k 的顶点基址 = k × 32）；条带更多时按需扩容。
    EnsureIboCapacity(kMaxStripsPerDraw);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    // 顶点缓冲按「一个块」预留容量：避免首帧/扩容时的 vector 重分配。
    // （VBO 本身仍是每帧 glBufferData 重传 —— 见 Draw 里的注释。）
    vertices_.reserve(static_cast<size_t>(kMaxStripsPerDraw) * kStripVerticesPerStrip);

    // uniform 位置只查一次（见头文件里的 CommonUniforms 注释）。
    QueryCommonUniforms(prog_draw_, &draw_uniforms_);
    comp_uniforms_.accum = shader_mgr_->GetUniform(prog_comp_, "uAccum");
    comp_uniforms_.reveal = shader_mgr_->GetUniform(prog_comp_, "uReveal");
    comp_uniforms_.add = shader_mgr_->GetUniform(prog_comp_, "uAdd");
    comp_uniforms_.additive = shader_mgr_->GetUniform(prog_comp_, "uAdditive");

    // ── 平滑 pass（kSmooth）：program + 独立 VAO/VBO + 控制点数据纹理 ──
    // 与折线 pass 完全独立：另一个 program、另一套顶点布局、另一条数据通道；
    // 只共享 WBO 目标与静态索引。
    const std::string fs_smooth =
        std::string(kCommonFsHead) + kWboWeightGlsl + kFsEmit + kSmoothFsMain;
    prog_smooth_ = shader_mgr_->GetOrCreate("strip_wbo_smooth",
                                            {kSmoothVs, fs_smooth.c_str()});
    QueryCommonUniforms(prog_smooth_, &smooth_uniforms_.common);
    smooth_uniforms_.seg_count = shader_mgr_->GetUniform(prog_smooth_, "uSegCount");
    smooth_uniforms_.data_rows = shader_mgr_->GetUniform(prog_smooth_, "uDataRows");
    smooth_uniforms_.strip_data = shader_mgr_->GetUniform(prog_smooth_, "uStripData");

    glGenVertexArrays(1, &smooth_vao_);
    glGenBuffers(1, &smooth_vbo_);
    CHECK_NE(smooth_vao_, 0u);
    CHECK_NE(smooth_vbo_, 0u);
    glBindVertexArray(smooth_vao_);
    glBindBuffer(GL_ARRAY_BUFFER, smooth_vbo_);
    // kSmoothFloatsPerVertex float/顶点：pos(3) + (u, v, alpha) + strip_id(1)
    const GLsizei smooth_stride =
        static_cast<GLsizei>(kSmoothFloatsPerVertex * sizeof(float));
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

    // 逐条带 tint：宽 = 条带数、高 = 1。NEAREST + CLAMP（texelFetch 本来就不走过滤）。
    glGenTextures(1, &strip_tint_tex_);
    CHECK_NE(strip_tint_tex_, 0u);
    glBindTexture(GL_TEXTURE_2D, strip_tint_tex_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    strip_tint_capacity_ = 64;
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, strip_tint_capacity_, 1, 0, GL_RGBA,
                 GL_FLOAT, nullptr);

    glGenTextures(1, &smooth_data_tex_);
    CHECK_NE(smooth_data_tex_, 0u);
    glBindTexture(GL_TEXTURE_2D, smooth_data_tex_);
    // 宽度 = 每列 3 texel × 采样点数；高度 = 一个 draw 块最多多少条带。
    // 关 mipmap / NEAREST：texelFetch 本来就不走过滤（也避免被 TextureManager 的默认选项污染）。
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    // 初始只开一小块，运行中按条带数翻倍增长（避免每帧抖动导致反复重建）。
    GLint max_tex = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_tex);
    smooth_max_rows_ = static_cast<int>(max_tex);
    smooth_data_rows_ = 64;
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F,
                 kStripSampleCount * kSmoothTexelsPerColumn, smooth_data_rows_, 0,
                 GL_RGBA, GL_FLOAT, nullptr);
    glBindTexture(GL_TEXTURE_2D, 0);

    // 顶点/控制点缓冲按需增长（reserve 只增不减），不预先占满 1024 条带；
    // 每次 DrawSmooth 都会先 clear 再填，所以这里不需要重置。

    initialized_ = true;
}

void StripRenderer::QueryCommonUniforms(unsigned int program, CommonUniforms* out) {
    CHECK(program != 0u) << "QueryCommonUniforms: program 不能为 0";
    CHECK(out != nullptr) << "QueryCommonUniforms: out 不能为空";
    // 名字表**只在这里出现一次**：两个 pass 用同名 uniform（同一个 kCommonFsHead）。
    // 抄错/漏改一个名字只会让 GetUniform 返回 -1（静默无效），出图很难定位。
    out->view_proj = shader_mgr_->GetUniform(program, "uViewProj");
    out->tint = shader_mgr_->GetUniform(program, "uTint");
    out->has_tex = shader_mgr_->GetUniform(program, "uHasTex");
    out->tex = shader_mgr_->GetUniform(program, "uTex");
    out->profile = shader_mgr_->GetUniform(program, "uProfile");
    out->scene_depth = shader_mgr_->GetUniform(program, "uSceneDepth");
    out->viewport = shader_mgr_->GetUniform(program, "uViewport");
    out->near_plane = shader_mgr_->GetUniform(program, "uNear");
    out->far_plane = shader_mgr_->GetUniform(program, "uFar");
    out->blend_path = shader_mgr_->GetUniform(program, "uBlendPath");
    out->soft_enabled = shader_mgr_->GetUniform(program, "uSoftDepthEnabled");
    out->soft_m = shader_mgr_->GetUniform(program, "uSoftDepthM");
    out->cam_pos = shader_mgr_->GetUniform(program, "uCamPos");
    out->strip_tint = shader_mgr_->GetUniform(program, "uStripTint");
    out->has_strip_tint = shader_mgr_->GetUniform(program, "uHasStripTint");
    out->fresnel_enabled = shader_mgr_->GetUniform(program, "uFresnelEnabled");
    out->fresnel_rim_color = shader_mgr_->GetUniform(program, "uFresnelRimColor");
    out->fresnel_f0 = shader_mgr_->GetUniform(program, "uFresnelF0");
    out->fresnel_power = shader_mgr_->GetUniform(program, "uFresnelPower");
    out->fresnel_strength = shader_mgr_->GetUniform(program, "uFresnelStrength");
    out->fresnel_graze_alpha = shader_mgr_->GetUniform(program, "uFresnelGrazeAlpha");
}

void StripRenderer::EnsureIboCapacity(int strips) {
    CHECK_GT(strips, 0);
    if (strips <= ibo_capacity_) {
        return;
    }
    const int cap = std::max(strips, ibo_capacity_ * 2);
    std::vector<uint32_t> indices;
    indices.reserve(static_cast<size_t>(cap) * kStripIndicesPerStrip);
    for (int block = 0; block < cap; ++block) {
        AppendStripIndicesForBlock(
            static_cast<uint32_t>(block * kStripVerticesPerStrip), &indices);
    }
    // ELEMENT_ARRAY_BUFFER 的绑定属于 VAO：必须先绑一个 VAO 再改（core profile 没有 VAO 0）。
    // 两个 VAO 共用同一个 ibo_，重传（同一个 buffer 名字）后它们的绑定依然有效。
    glBindVertexArray(vao_);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo_);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(indices.size() * sizeof(uint32_t)),
                 indices.data(), GL_STATIC_DRAW);
    glBindVertexArray(0);
    ibo_capacity_ = cap;
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
    if (strip_tint_tex_ != 0) {
        glDeleteTextures(1, &strip_tint_tex_);
        strip_tint_tex_ = 0;
    }
    strip_tint_capacity_ = 0;
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
    smooth_data_rows_ = 0;
    smooth_max_rows_ = 0;
    // ⚠️ 必须复位：否则 Finalize → Init 的第二次 Init 里 EnsureIboCapacity 会因为
    //    「容量看起来还够」直接返回，而 IBO 已经被删掉 → 后续 draw 索引到已删除的 buffer。
    ibo_capacity_ = 0;
    initialized_ = false;
    has_content_ = false;
    has_alpha_ = false;
    has_additive_ = false;
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

    BatchDrawContext ctx;
    ctx.batch = &batch;
    ctx.want_additive = want_additive;
    ctx.camera = &camera;
    ctx.view_proj = view_proj;
    ctx.scene_depth_tex = scene_depth_tex;
    ctx.full_w = full_w;
    ctx.full_h = full_h;
    if (batch.texture_id != 0) {
        ctx.gl_tex = texture_mgr.GetGLTexture(batch.texture_id);
        CHECK_NE(ctx.gl_tex, 0u) << "StripRenderer::Draw: texture_id " << batch.texture_id
                                 << " 未注册";
    }

    // 逐条带 tint（可选）：整批一次上传（宽 = 条带数、高 = 1）。
    // 放在两个 drawer 之前：它是**批级**数据，两条 pass 用同一个采样器。
    if (!batch.strip_tints.empty()) {
        const int count = static_cast<int>(batch.strip_tints.size());
        if (count > strip_tint_capacity_) {
            const int grown = std::max(count, strip_tint_capacity_ * 2);
            CHECK_LE(grown, smooth_max_rows_)
                << "StripRenderer::Draw: 单批条带数超过 GL_MAX_TEXTURE_SIZE（"
                << smooth_max_rows_ << "），逐条带 tint 放不下";
            strip_tint_capacity_ = grown;
            glBindTexture(GL_TEXTURE_2D, strip_tint_tex_);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, strip_tint_capacity_, 1, 0,
                         GL_RGBA, GL_FLOAT, nullptr);
        }
        // unit 0 归位再绑：上传不该依赖上一个 pass 留下的 active unit
        //（本 pass 的约定同样是「进来时 unit 0 已激活」）。
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, strip_tint_tex_);
        // Color 是紧凑的 4 float（见 strip_types.h 的 static_assert），可直接上传。
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, count, 1, GL_RGBA, GL_FLOAT,
                        batch.strip_tints.data());
        ctx.has_strip_tint = true;
    }

    // 两个独立 drawer，写同一张 WBO 目标（命令顺序 = 前后顺序）。
    if (batch.style == StripStyle::kSmooth) {
        DrawSmooth(ctx);
    } else {
        DrawPolyline(ctx);
    }

    has_content_ = true;
    if (want_additive) {
        has_additive_ = true;
    } else {
        has_alpha_ = true;
    }
}

void StripRenderer::ApplyCommonUniforms(const CommonUniforms& uniforms,
                                        const BatchDrawContext& ctx) {
    CHECK(ctx.batch != nullptr) << "ApplyCommonUniforms: ctx.batch 不能为空";
    CHECK(ctx.camera != nullptr) << "ApplyCommonUniforms: ctx.camera 不能为空";
    CHECK(ctx.view_proj != nullptr) << "ApplyCommonUniforms: ctx.view_proj 不能为空";
    const TranslucentStripBatch& batch = *ctx.batch;

    glUniformMatrix4fv(uniforms.view_proj, 1, GL_FALSE, ctx.view_proj);
    glUniform4f(uniforms.tint, batch.tint.r, batch.tint.g, batch.tint.b, batch.tint.a);
    glUniform1fv(uniforms.profile, 9, batch.width_profile.data());
    glUniform2f(uniforms.viewport, static_cast<float>(ctx.full_w),
                static_cast<float>(ctx.full_h));
    glUniform1f(uniforms.near_plane, ctx.camera->near);
    glUniform1f(uniforms.far_plane, ctx.camera->far);
    glUniform1i(uniforms.blend_path, ctx.want_additive ? 1 : 0);
    if (batch.soft_depth_m.has_value()) {
        glUniform1i(uniforms.soft_enabled, 1);
        glUniform1f(uniforms.soft_m, *batch.soft_depth_m);
    } else {
        glUniform1i(uniforms.soft_enabled, 0);
        // 关闭时也给一个合法值：避免读未初始化 uniform（FS 里该分支不取它）。
        glUniform1f(uniforms.soft_m, 1.0f);
    }

    glActiveTexture(GL_TEXTURE0 + kSceneDepthUnit);
    glBindTexture(GL_TEXTURE_2D, ctx.scene_depth_tex);
    glUniform1i(uniforms.scene_depth, kSceneDepthUnit);

    // 逐条带 tint：没有就置 0，FS 走「只用 uTint」的分支（一次取样都不做）。
    if (ctx.has_strip_tint) {
        glActiveTexture(GL_TEXTURE0 + kStripTintUnit);
        glBindTexture(GL_TEXTURE_2D, strip_tint_tex_);
        glUniform1i(uniforms.strip_tint, kStripTintUnit);
        glUniform1i(uniforms.has_strip_tint, 1);
    } else {
        glUniform1i(uniforms.has_strip_tint, 0);
    }

    // fresnel：关闭时也把 uniform 写成合法值（FS 里那个分支不取它们，但读未初始化的
    // uniform 是无法排查的隐患）；开启时按批取值。
    glUniform3f(uniforms.cam_pos, ctx.camera->position.x(), ctx.camera->position.y(),
                ctx.camera->position.z());
    if (batch.fresnel.has_value()) {
        const StripFresnel& f = *batch.fresnel;
        glUniform1i(uniforms.fresnel_enabled, 1);
        glUniform3f(uniforms.fresnel_rim_color, f.rim_color.x(), f.rim_color.y(),
                    f.rim_color.z());
        glUniform1f(uniforms.fresnel_f0, f.f0);
        glUniform1f(uniforms.fresnel_power, f.power);
        glUniform1f(uniforms.fresnel_strength, f.strength);
        glUniform1f(uniforms.fresnel_graze_alpha, f.graze_alpha);
    } else {
        glUniform1i(uniforms.fresnel_enabled, 0);
        glUniform3f(uniforms.fresnel_rim_color, 1.0f, 1.0f, 1.0f);
        glUniform1f(uniforms.fresnel_f0, 0.04f);
        glUniform1f(uniforms.fresnel_power, 5.0f);
        glUniform1f(uniforms.fresnel_strength, 1.0f);
        glUniform1f(uniforms.fresnel_graze_alpha, 1.0f);
    }

    if (ctx.gl_tex != 0) {
        glActiveTexture(GL_TEXTURE0 + kStripTexUnit);
        glBindTexture(GL_TEXTURE_2D, ctx.gl_tex);
        glUniform1i(uniforms.tex, kStripTexUnit);
        glUniform1i(uniforms.has_tex, 1);
    } else {
        glUniform1i(uniforms.has_tex, 0);
    }
}

void StripRenderer::DrawPolyline(const BatchDrawContext& ctx) {
    // 本 pass 改 enable / blend / 渲染目标：三组状态都入栈。
    glPushAttrib(GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    wbo_.BindForDraw();
    glDisable(GL_CULL_FACE);   // 条带单层双面可见（关剔除；不是两层几何）

    glUseProgram(prog_draw_);
    ApplyCommonUniforms(draw_uniforms_, ctx);

    const std::vector<Strip16>& strips = ctx.batch->strips;
    const size_t total = strips.size();
    // 一次 draw call 画完整批；超过 kMaxStripsPerDraw 才分块（不截断）。
    const int chunks =
        static_cast<int>((total + kMaxStripsPerDraw - 1) / kMaxStripsPerDraw);
    // 只在头几帧打一条（交互模式 60fps 不能每帧刷屏）：给验收提供「一次 draw call」的证据。
    LOG_FIRST_N(INFO, 3) << "StripRenderer(polyline): strips=" << total
                         << " draw_calls=" << chunks;
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    for (size_t begin = 0; begin < total; begin += kMaxStripsPerDraw) {
        const size_t count = std::min<size_t>(kMaxStripsPerDraw, total - begin);

        vertices_.clear();
        vertices_.reserve(count * kStripVerticesPerStrip);
        for (size_t k = begin; k < begin + count; ++k) {
            ExpandStripVertices(strips[k], &vertices_);
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

void StripRenderer::DrawSmooth(const BatchDrawContext& ctx) {
    glPushAttrib(GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    wbo_.BindForDraw();
    glDisable(GL_CULL_FACE);   // 同上：单层双面可见

    glUseProgram(prog_smooth_);
    ApplyCommonUniforms(smooth_uniforms_.common, ctx);
    glUniform1f(smooth_uniforms_.seg_count, static_cast<float>(kStripQuads));

    // 数据纹理占 kSmoothDataUnit（unit0 = 场景深度、unit1 = 条带纹理）。
    glActiveTexture(GL_TEXTURE0 + kSmoothDataUnit);
    glBindTexture(GL_TEXTURE_2D, smooth_data_tex_);
    glUniform1i(smooth_uniforms_.strip_data, kSmoothDataUnit);

    const std::vector<Strip16>& strips = ctx.batch->strips;
    const size_t total = strips.size();
    LOG_FIRST_N(INFO, 3) << "StripRenderer(smooth): strips=" << total
                         << " draw_calls=1";

    // 数据纹理按需扩容（容量翻倍增长，避免条带数每帧抖动时反复重建）。
    if (static_cast<int>(total) > smooth_data_rows_) {
        const int grown = std::max(static_cast<int>(total), smooth_data_rows_ * 2);
        CHECK_LE(grown, smooth_max_rows_)
            << "StripRenderer(smooth): 单批条带数超过 GL_MAX_TEXTURE_SIZE（"
            << smooth_max_rows_ << "），数据纹理放不下";
        smooth_data_rows_ = grown;
        glActiveTexture(GL_TEXTURE0 + kSmoothDataUnit);
        glBindTexture(GL_TEXTURE_2D, smooth_data_tex_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F,
                     kStripSampleCount * kSmoothTexelsPerColumn, smooth_data_rows_, 0,
                     GL_RGBA, GL_FLOAT, nullptr);
    }

    // 整批一次展开（CPU）→ 一次上传 → 一次 draw。
    // 为什么不逐块：逐块会在上传第 2 块时写第 1 块 draw 正在采样的数据纹理
    //（write-after-read），而数据行号本来就等于全局 strip 下标，天然整批连续。
    smooth_vertices_.clear();
    smooth_vertices_.reserve(total * kStripVerticesPerStrip * kSmoothFloatsPerVertex);
    smooth_data_.clear();
    smooth_data_.reserve(total * kStripSampleCount * kSmoothFloatsPerColumn);
    SmoothStripCompute computed;
    for (size_t k = 0; k < total; ++k) {
        // 切线 / 外扩量 / 打包全在 strip_geometry.h（GL-free，可单测）。
        ComputeSmoothStrip(strips[k], &computed);
        AppendSmoothStripVertices(strips[k], computed, static_cast<float>(k),
                                  &smooth_vertices_);
        AppendSmoothStripData(computed, &smooth_data_);
        LOG_FIRST_N(INFO, 6) << "StripSmooth: margin=" << computed.margin
                             << " scale=" << computed.scale;
    }

    // 行号钳位的上界 = 纹理**实际高度**（不是每块条带数）：整批可能远超一块。
    glUniform1f(smooth_uniforms_.data_rows, static_cast<float>(smooth_data_rows_));

    EnsureIboCapacity(static_cast<int>(total));

    glBindVertexArray(smooth_vao_);
    glBindBuffer(GL_ARRAY_BUFFER, smooth_vbo_);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(smooth_vertices_.size() * sizeof(float)),
                 smooth_vertices_.data(), GL_STREAM_DRAW);
    glActiveTexture(GL_TEXTURE0 + kSmoothDataUnit);
    glBindTexture(GL_TEXTURE_2D, smooth_data_tex_);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                    kStripSampleCount * kSmoothTexelsPerColumn,
                    static_cast<GLsizei>(total), GL_RGBA, GL_FLOAT,
                    smooth_data_.data());

    // 整批一次 draw：索引缓冲按需扩到 total 条带（块 j 的顶点基址 = j×32），
    // 顶点与数据纹理都是整批连续的 ⇒ 不分块。
    glDrawElements(GL_TRIANGLES,
                   static_cast<GLsizei>(total) * kStripIndicesPerStrip,
                   GL_UNSIGNED_INT, reinterpret_cast<const void*>(0));
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
