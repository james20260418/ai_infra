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

#include "tools/jpov/src/texture_units.h"
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
// per-instance 数据纹理（逐点 RGBA + 宽度剖面，RGBA32F）。同样 pass-local。
constexpr int kAppearanceUnit = 3;
// 级联 shadow map：**与主链路同一段槽位**（texture_units.h 的 kTexUnitShadowMapBase = 7），
// 因为本 pass 与 object3d 不同时活跃，复用同一区间不会撞槽（与 tile/材质同一约定）。
constexpr int kShadowUnitBase = kTexUnitShadowMapBase;

}  // namespace

void StripRenderer::Init(ShaderManager* shader_mgr) {
    CHECK(shader_mgr != nullptr) << "StripRenderer::Init: shader_mgr 不能为空";
    CHECK(!initialized_) << "StripRenderer::Init 重复调用";
    shader_mgr_ = shader_mgr;

    // 权重函数必须来自共享件（wbo_shader.h）—— 与粒子/体积火/玻璃是同一份数学。
    //
    // 四个 program = 两个 pass × 两套天光定义（关 / 开）。**主函数只有一份**，
    // 两者只差 `ApplySkyLight` 的定义（空实现 vs 真注入）⇒ 不会漂移；
    // 而「不开天光」的那两个 program 二进制上等于改动前（空实现被内联消掉）。
    const std::string fs_draw_off =
        std::string(kCommonFsHead) + kWboWeightGlsl + kFsEmit + kSkyLightOffFs + kFsMain;
    const std::string fs_draw_lit =
        std::string(kCommonFsHead) + kWboWeightGlsl + kFsEmit + kSkyLightFs + kFsMain;
    prog_draw_ = shader_mgr_->GetOrCreate("strip_wbo_draw", {kVs, fs_draw_off.c_str()});
    prog_draw_lit_ =
        shader_mgr_->GetOrCreate("strip_wbo_draw_lit", {kVs, fs_draw_lit.c_str()});
    prog_comp_ = shader_mgr_->GetOrCreate("strip_wbo_composite",
                                          {kWboCompositeVs, kWboCompositeFs});

    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    glGenBuffers(1, &ibo_);
    CHECK_NE(vao_, 0u);
    CHECK_NE(vbo_, 0u);
    CHECK_NE(ibo_, 0u);

    // 顶点布局固定为 StripVertex（5 个连续 float：pos.xyz + u + v）；VBO 每帧重传，
    // 但绑定关系不变。
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(StripVertex),
                          reinterpret_cast<const void*>(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(StripVertex),
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
    const std::string fs_smooth_off = std::string(kCommonFsHead) + kWboWeightGlsl + kFsEmit +
                                      kSkyLightOffFs + kSmoothFsMain;
    const std::string fs_smooth_lit = std::string(kCommonFsHead) + kWboWeightGlsl + kFsEmit +
                                      kSkyLightFs + kSmoothFsMain;
    prog_smooth_ = shader_mgr_->GetOrCreate("strip_wbo_smooth",
                                            {kSmoothVs, fs_smooth_off.c_str()});
    prog_smooth_lit_ = shader_mgr_->GetOrCreate("strip_wbo_smooth_lit",
                                                {kSmoothVs, fs_smooth_lit.c_str()});
    QueryCommonUniforms(prog_smooth_, &smooth_uniforms_.common);
    smooth_uniforms_.seg_count = shader_mgr_->GetUniform(prog_smooth_, "uSegCount");
    smooth_uniforms_.data_rows = shader_mgr_->GetUniform(prog_smooth_, "uDataRows");
    smooth_uniforms_.strip_data = shader_mgr_->GetUniform(prog_smooth_, "uStripData");
    QueryCommonUniforms(prog_smooth_lit_, &smooth_lit_uniforms_.common);
    smooth_lit_uniforms_.seg_count = shader_mgr_->GetUniform(prog_smooth_lit_, "uSegCount");
    smooth_lit_uniforms_.data_rows = shader_mgr_->GetUniform(prog_smooth_lit_, "uDataRows");
    smooth_lit_uniforms_.strip_data = shader_mgr_->GetUniform(prog_smooth_lit_, "uStripData");

    glGenVertexArrays(1, &smooth_vao_);
    glGenBuffers(1, &smooth_vbo_);
    CHECK_NE(smooth_vao_, 0u);
    CHECK_NE(smooth_vbo_, 0u);
    glBindVertexArray(smooth_vao_);
    glBindBuffer(GL_ARRAY_BUFFER, smooth_vbo_);
    // kSmoothFloatsPerVertex float/顶点：pos(3) + (u, v) + strip_id(1)
    const GLsizei smooth_stride =
        static_cast<GLsizei>(kSmoothFloatsPerVertex * sizeof(float));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, smooth_stride,
                          reinterpret_cast<const void*>(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, smooth_stride,
                          reinterpret_cast<const void*>(3 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, smooth_stride,
                          reinterpret_cast<const void*>(5 * sizeof(float)));
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo_);   // 与折线 pass 共用静态索引
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    GLint max_tex = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_tex);
    max_texture_rows_ = static_cast<int>(max_tex);

    // per-instance 数据纹理：一行 = 一条带：
    //   [0..15]  逐点 RGBA（沿长度 16 列，**LINEAR**：列间要平滑过渡）
    //   [16..24] 宽度剖面 9 个 knot（**LINEAR**：9 个等距 knot 的双线性 = 精确分段线性）
    // NEAREST 会让颜色变成 16 段台阶、剖面变成 8 段台阶 —— 这两条都必须过滤。
    glGenTextures(1, &appearance_tex_);
    CHECK_NE(appearance_tex_, 0u);
    glBindTexture(GL_TEXTURE_2D, appearance_tex_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    appearance_capacity_ = 64;
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, kAppearanceTexels, appearance_capacity_,
                 0, GL_RGBA, GL_FLOAT, nullptr);

    glGenTextures(1, &smooth_data_tex_);
    CHECK_NE(smooth_data_tex_, 0u);
    glBindTexture(GL_TEXTURE_2D, smooth_data_tex_);
    // 宽度 = 每列 4 texel × 采样点数；高度 = 一个 draw 块最多多少条带。
    // 关 mipmap / NEAREST：texelFetch 本来就不走过滤（也避免被 TextureManager 的默认选项污染）。
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    // 初始只开一小块，运行中按条带数翻倍增长（避免每帧抖动导致反复重建）。
    smooth_data_rows_ = 64;
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F,
                 kStripSampleCount * kSmoothTexelsPerColumn, smooth_data_rows_, 0,
                 GL_RGBA, GL_FLOAT, nullptr);
    glBindTexture(GL_TEXTURE_2D, 0);

    // 顶点/控制点缓冲按需增长（reserve 只增不减），不预先占满 1024 条带；
    // 每次 DrawSmooth 都会先 clear 再填，所以这里不需要重置。

    // 关闭态的默认 uniform **只在 Init 写一次**：uniform 状态是 per-program 的，
    // 而 FS 里这些值只在对应开关打开时才会被读，所以每批重复上传纯属浪费
    // （1000 批 × 每次 glUniform = 数千次驱动调用/帧）。开关字段本身仍然每批设置。
    auto init_disabled_uniforms = [this](unsigned int program, const CommonUniforms& u) {
        glUseProgram(program);
        glUniform1f(u.soft_m, 1.0f);
        glUniform1f(u.soft_inv, 1.0f);
        // fresnel 的「关闭态」值：uFresnelEnabled=0 时 FS 不会读它们，但留确定值。
        glUniform3f(u.fresnel_rim_color, 1.0f, 1.0f, 1.0f);
        glUniform1f(u.fresnel_f0, 0.04f);
        glUniform1f(u.fresnel_power, 5.0f);
        // 与上面的默认 power 成对：5 ⇒ 特化路径（只改 power 不改它会让默认态落到 pow）。
        glUniform1i(u.fresnel_pow5, 1);
        glUniform1f(u.fresnel_strength, 1.0f);
        glUniform1f(u.fresnel_graze_alpha, 1.0f);
        // 天光：初值全 0（= 零光照，与「不开」视觉一致）；lit program 每次 Draw 都覆盖。
        glUniform3f(u.sky_zenith, 0.0f, 0.0f, 0.0f);
        glUniform3f(u.sky_horizon, 0.0f, 0.0f, 0.0f);
        glUniform3f(u.sky_ground, 0.0f, 0.0f, 0.0f);
        glUniform1f(u.sky_ambient_scale, 0.0f);
        glUniform3f(u.sun_dir, 0.0f, 1.0f, 0.0f);
        glUniform3f(u.sun_color, 1.0f, 1.0f, 1.0f);
        glUniform1f(u.sun_intensity, 0.0f);
        glUniform1f(u.sky_injection, 0.0f);
        glUseProgram(0);
    };
    init_disabled_uniforms(prog_draw_, draw_uniforms_);
    init_disabled_uniforms(prog_smooth_, smooth_uniforms_.common);
    // lit 变体同样给一份关态初值（虽然它们的 FS 一定会读，但留着保证「任何时刻
    // 读到的 uniform 都是确定的」—— 与上面同一理由）。
    init_disabled_uniforms(prog_draw_lit_, draw_lit_uniforms_);
    init_disabled_uniforms(prog_smooth_lit_, smooth_lit_uniforms_.common);

    // GLSL 里硬编码了拓扑常量（顶点属性/条带 = 32、采样点数 = 16、段数 = 15、
    // per-instance 行宽 = 25）：折线 pass 的 `gl_VertexID / 32` 与 PointColor/
    // WidthProfile 的 `15.0 / 25.0 / 16.0` 都依赖它们。改 C++ 常量却忘了改 shader，
    // 是**静默错色/错条带**（不崩、不出图像错误提示），所以在这里拦住。
    CHECK_EQ(kStripVerticesPerStrip, 32) << "kVs 里 gl_VertexID/32 必须同步";
    CHECK_EQ(kStripSampleCount, 16) << "PointColor 里的 16 个 texel 必须同步";
    CHECK_EQ(kStripQuads, 15) << "PointColor 里的 15 段必须同步";
    CHECK_EQ(kAppearanceTexels, 25) << "shader 里 PointColor/WidthProfile 的 25.0 必须同步";
    // 平滑 pass 的 texel 步长：kSmoothFsMain 里 `int tx = seg * 4` 且每列最后一个 texel 是 rgba。
    CHECK_EQ(kSmoothTexelsPerColumn, 4) << "shader 里 kSmoothFsMain 的 seg*4 必须同步";

    initialized_ = true;
}

void StripRenderer::QueryCommonUniforms(unsigned int program, CommonUniforms* out) {
    CHECK(program != 0u) << "QueryCommonUniforms: program 不能为 0";
    CHECK(out != nullptr) << "QueryCommonUniforms: out 不能为空";
    // 名字表**只在这里出现一次**：两个 pass 用同名 uniform（同一个 kCommonFsHead）。
    // 抄错/漏改一个名字只会让 GetUniform 返回 -1（静默无效），出图很难定位。
    out->view_proj = shader_mgr_->GetUniform(program, "uViewProj");
    out->tex = shader_mgr_->GetUniform(program, "uTex");
    out->has_tex = shader_mgr_->GetUniform(program, "uHasTex");
    out->appearance = shader_mgr_->GetUniform(program, "uAppearance");
    out->appearance_rows = shader_mgr_->GetUniform(program, "uAppearanceRows");
    out->scene_depth = shader_mgr_->GetUniform(program, "uSceneDepth");
    out->viewport = shader_mgr_->GetUniform(program, "uViewport");
    out->near_plane = shader_mgr_->GetUniform(program, "uNear");
    out->far_plane = shader_mgr_->GetUniform(program, "uFar");
    out->blend_path = shader_mgr_->GetUniform(program, "uBlendPath");
    out->soft_enabled = shader_mgr_->GetUniform(program, "uSoftDepthEnabled");
    out->soft_m = shader_mgr_->GetUniform(program, "uSoftDepthM");
    out->soft_inv = shader_mgr_->GetUniform(program, "uSoftDepthInv");
    out->cam_pos = shader_mgr_->GetUniform(program, "uCamPos");
    out->fresnel_enabled = shader_mgr_->GetUniform(program, "uFresnelEnabled");
    out->fresnel_rim_color = shader_mgr_->GetUniform(program, "uFresnelRimColor");
    out->fresnel_f0 = shader_mgr_->GetUniform(program, "uFresnelF0");
    out->fresnel_power = shader_mgr_->GetUniform(program, "uFresnelPower");
    out->fresnel_pow5 = shader_mgr_->GetUniform(program, "uFresnelPow5");
    out->fresnel_strength = shader_mgr_->GetUniform(program, "uFresnelStrength");
    out->fresnel_graze_alpha = shader_mgr_->GetUniform(program, "uFresnelGrazeAlpha");
    // 天光（只存在于 lit program；unlit program 里全为 -1，glUniform 对 -1 是 no-op）。
    out->sky_zenith = shader_mgr_->GetUniform(program, "uSkyZenith");
    out->sky_horizon = shader_mgr_->GetUniform(program, "uSkyHorizon");
    out->sky_ground = shader_mgr_->GetUniform(program, "uSkyGround");
    out->sky_ambient_scale = shader_mgr_->GetUniform(program, "uSkyAmbientScale");
    out->sun_dir = shader_mgr_->GetUniform(program, "uSunDir");
    out->sun_color = shader_mgr_->GetUniform(program, "uSunColor");
    out->sun_intensity = shader_mgr_->GetUniform(program, "uSunIntensity");
    out->sky_injection = shader_mgr_->GetUniform(program, "uSkyInjection");
    // CSM（同上天光：unlit program 里全是 -1）。
    out->has_csm = shader_mgr_->GetUniform(program, "uHasCsm");
    out->cascade_count = shader_mgr_->GetUniform(program, "uCascadeCount");
    out->cascade_ranges = shader_mgr_->GetUniform(program, "uCascadeRanges");
    out->shadow_texel_world = shader_mgr_->GetUniform(program, "uShadowTexelWorld");
    out->shadow_bias_cascade = shader_mgr_->GetUniform(program, "uShadowBiasCascade");
    out->shadow_bias_override = shader_mgr_->GetUniform(program, "uShadowBiasOverride");
    out->shadow_fade_start = shader_mgr_->GetUniform(program, "uShadowFadeStart");
    out->shadow_fade_end = shader_mgr_->GetUniform(program, "uShadowFadeEnd");
    // 数组型（sampler / mat4）：逐元素查一次，Draw 里直接用整数。
    for (int c = 0; c < ShadowConfig::kMaxCascades; ++c) {
        const std::string idx = "[" + std::to_string(c) + "]";
        out->shadow_map[c] = shader_mgr_->GetUniform(program, ("uShadowMap" + idx).c_str());
        out->shadow_vp[c] = shader_mgr_->GetUniform(program, ("uShadowVP" + idx).c_str());
        out->shadow_depth_vp[c] =
            shader_mgr_->GetUniform(program, ("uShadowDepthVP" + idx).c_str());
    }
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
    if (appearance_tex_ != 0) {
        glDeleteTextures(1, &appearance_tex_);
        appearance_tex_ = 0;
    }
    appearance_capacity_ = 0;
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
    max_texture_rows_ = 0;
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

void StripRenderer::UploadAppearance(const std::vector<Strip16>& strips) {
    const int count = static_cast<int>(strips.size());
    CHECK_GT(count, 0) << "UploadAppearance: strips 不能为空";
    CHECK_LE(count, max_texture_rows_)
        << "StripRenderer::UploadAppearance: 单批条带数超过 GL_MAX_TEXTURE_SIZE（"
        << max_texture_rows_ << "），per-instance 数据纹理放不下";

    if (count > appearance_capacity_) {
        const int grown = std::max(count, appearance_capacity_ * 2);
        CHECK_LE(grown, max_texture_rows_)
            << "StripRenderer::UploadAppearance: 扩容后高度超过 GL_MAX_TEXTURE_SIZE（"
            << max_texture_rows_ << "）";
        appearance_capacity_ = grown;
        glBindTexture(GL_TEXTURE_2D, appearance_tex_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, kAppearanceTexels,
                     appearance_capacity_, 0, GL_RGBA, GL_FLOAT, nullptr);
    }

    // 逐条带 gather 成「每行 25 texel」的连续块（CPU stride 与 GPU 行布局解耦：
    // 用户面对的是一个 Strip16，纹理只看颜色/剖面这两段）。
    appearance_data_.clear();
    appearance_data_.reserve(static_cast<size_t>(count) * kAppearanceTexels * 4);
    for (const Strip16& strip : strips) {
        for (int k = 0; k < kStripSampleCount; ++k) {
            const Color& c = strip.point_color[static_cast<size_t>(k)];
            appearance_data_.push_back(c.r);
            appearance_data_.push_back(c.g);
            appearance_data_.push_back(c.b);
            appearance_data_.push_back(c.a);
        }
        for (int k = 0; k < kStripProfileKnots; ++k) {
            appearance_data_.push_back(strip.width_profile[static_cast<size_t>(k)]);
            appearance_data_.push_back(0.0f);
            appearance_data_.push_back(0.0f);
            appearance_data_.push_back(0.0f);
        }
    }

    // unit 0 归位再绑：上传不该依赖上一个 pass 留下的 active unit
    //（本 pass 的约定同样是「进来时 unit 0 已激活」）。
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, appearance_tex_);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kAppearanceTexels, count, GL_RGBA,
                    GL_FLOAT, appearance_data_.data());
}

void StripRenderer::Draw(const TranslucentStripBatch& batch,
                         TextureManager& texture_mgr,
                         const Camera& camera,
                         const float view_proj[16],
                         unsigned int scene_depth_tex,
                         const StripCsmShadow* csm,
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
    ctx.want_light = batch.sky_light.has_value();
    ctx.csm = csm;
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

    // 逐条带的颜色/剖面：整批一次上传（两个 pass 共用同一张纹理）。
    UploadAppearance(batch.strips);

    // 两个独立 drawer，写同一张 WBO 目标（命令顺序 = 前后顺序）。
    if (batch.geometry == StripInterpolation::kSmooth) {
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
    glUniform2f(uniforms.viewport, static_cast<float>(ctx.full_w),
                static_cast<float>(ctx.full_h));
    glUniform1f(uniforms.near_plane, ctx.camera->near);
    glUniform1f(uniforms.far_plane, ctx.camera->far);
    glUniform1i(uniforms.blend_path, ctx.want_additive ? 1 : 0);
    // 关闭态的值已在 Init 写过（见 init_disabled_uniforms）：这里只设开关与**开启时**的值。
    glUniform1i(uniforms.soft_enabled, batch.soft_depth_m.has_value() ? 1 : 0);
    if (batch.soft_depth_m.has_value()) {
        const float m = *batch.soft_depth_m;
        glUniform1f(uniforms.soft_m, m);
        glUniform1f(uniforms.soft_inv, 1.0f / m);
    }

    glActiveTexture(GL_TEXTURE0 + kSceneDepthUnit);
    glBindTexture(GL_TEXTURE_2D, ctx.scene_depth_tex);
    glUniform1i(uniforms.scene_depth, kSceneDepthUnit);

    // per-instance 数据纹理：行号钳位用**实际纹理高度**（不是本批条带数 ——
    // 扩容后两者不同，用条带数会让行坐标落错 texel）。
    glActiveTexture(GL_TEXTURE0 + kAppearanceUnit);
    glBindTexture(GL_TEXTURE_2D, appearance_tex_);
    glUniform1i(uniforms.appearance, kAppearanceUnit);
    glUniform1f(uniforms.appearance_rows, static_cast<float>(appearance_capacity_));

    // fresnel：关闭时也把 uniform 写成合法值（FS 里那个分支不取它们，但读未初始化的
    // uniform 是无法排查的隐患）；开启时按批取值。
    glUniform3f(uniforms.cam_pos, ctx.camera->position.x(), ctx.camera->position.y(),
                ctx.camera->position.z());
    glUniform1i(uniforms.fresnel_enabled, batch.fresnel.has_value() ? 1 : 0);
    if (batch.fresnel.has_value()) {
        const StripFresnel& f = *batch.fresnel;
        glUniform3f(uniforms.fresnel_rim_color, f.rim_color.x(), f.rim_color.y(),
                    f.rim_color.z());
        glUniform1f(uniforms.fresnel_f0, f.f0);
        glUniform1f(uniforms.fresnel_power, f.power);
        // 恰好 5 ⇒ 走五次乘法特化（FresnelFactor）。这里用**精确相等**：5.0f 是 Schlick 的
        // 默认指数，也是所有预设的取值；|p−5|<eps 这种近似语义只会让「走哪条路」取决于
        // 浮点偶然，反而难排查。要特化别的整数指数再另开分支。
        glUniform1i(uniforms.fresnel_pow5, (f.power == 5.0f) ? 1 : 0);
        glUniform1f(uniforms.fresnel_strength, f.strength);
        glUniform1f(uniforms.fresnel_graze_alpha, f.graze_alpha);
    }

    // 天光：只在开启时上传（与 fresnel 同一约定：关态靠 Init 的零值，不逐批重传）。
    // 注意这组 uniform 在 unlit program 里全是 -1（glUniform 是静默 no-op），所以
    // 这段代码只在 lit program 上真正生效，不会给不开天光的批增加驱动调用。
    if (batch.sky_light.has_value()) {
        const StripSkyLight& s = *batch.sky_light;
        glUniform3f(uniforms.sky_zenith, s.tricolor[0].r, s.tricolor[0].g, s.tricolor[0].b);
        glUniform3f(uniforms.sky_horizon, s.tricolor[1].r, s.tricolor[1].g, s.tricolor[1].b);
        glUniform3f(uniforms.sky_ground, s.tricolor[2].r, s.tricolor[2].g, s.tricolor[2].b);
        glUniform1f(uniforms.sky_ambient_scale, s.ambient_scale);
        const float sun_len = s.sun_direction.Norm();
        glUniform3f(uniforms.sun_dir, s.sun_direction.x() / sun_len,
                    s.sun_direction.y() / sun_len, s.sun_direction.z() / sun_len);
        glUniform3f(uniforms.sun_color, s.sun_color.x(), s.sun_color.y(), s.sun_color.z());
        glUniform1f(uniforms.sun_intensity, s.sun_intensity);
        glUniform1f(uniforms.sky_injection, s.injection);

        // CSM：与天光**同一个开关**（sky_light 开着就吃阴影）。没有太阳/级联时写 0，
        // 太阳项退化为完全受照 —— 与「不开天光」的差别只在颜色，不在是否出错。
        const bool has_csm = ctx.csm != nullptr && ctx.csm->valid();
        glUniform1i(uniforms.has_csm, has_csm ? 1 : 0);
        if (has_csm) {
            const StripCsmShadow& csm = *ctx.csm;
            const int n = std::min(csm.cfg.cascade_count, ShadowConfig::kMaxCascades);
            CHECK_GT(n, 0) << "StripCsmShadow: cascade_count 必须 > 0（没有级联就别传 csm）";
            CHECK_EQ(static_cast<int>(csm.fbos.size()), n)
                << "StripCsmShadow: fbos.size() 必须等于 cascade_count";
            glUniform1i(uniforms.cascade_count, n);
            glUniform1fv(uniforms.cascade_ranges, ShadowConfig::kMaxCascades,
                         csm.cfg.cascade_ranges);
            glUniform1fv(uniforms.shadow_texel_world, ShadowConfig::kMaxCascades,
                         csm.texel_world);
            glUniform1fv(uniforms.shadow_bias_cascade, ShadowConfig::kMaxCascades,
                         csm.cfg.cascade_bias);
            glUniform1i(uniforms.shadow_bias_override,
                        csm.cfg.override_cascade_bias ? 1 : 0);
            glUniform1f(uniforms.shadow_fade_start, csm.cfg.fade_start);
            glUniform1f(uniforms.shadow_fade_end, csm.cfg.fade_end);
            for (int c = 0; c < n; ++c) {
                const int unit = kShadowUnitBase + c;
                glActiveTexture(GL_TEXTURE0 + unit);
                glBindTexture(GL_TEXTURE_2D, csm.fbos[static_cast<size_t>(c)].tex);
                glUniform1i(uniforms.shadow_map[c], unit);
                glUniformMatrix4fv(uniforms.shadow_vp[c], 1, GL_FALSE, csm.vp[c]);
                glUniformMatrix4fv(uniforms.shadow_depth_vp[c], 1, GL_FALSE, csm.depth_vp[c]);
            }
        }
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

    // 选 program：开天光走 lit 变体（两套 shader 只差 ApplySkyLight 的定义）。
    glUseProgram(ctx.want_light ? prog_draw_lit_ : prog_draw_);
    ApplyCommonUniforms(ctx.want_light ? draw_lit_uniforms_ : draw_uniforms_, ctx);

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

    // 选 program / uniform 集：开天光走 lit 变体（两套 shader 只差 ApplySkyLight 的定义）。
    const SmoothUniforms& u = ctx.want_light ? smooth_lit_uniforms_ : smooth_uniforms_;
    glUseProgram(ctx.want_light ? prog_smooth_lit_ : prog_smooth_);
    ApplyCommonUniforms(u.common, ctx);
    glUniform1f(u.seg_count, static_cast<float>(kStripQuads));

    // 数据纹理占 kSmoothDataUnit（unit0 = 场景深度、unit1 = 条带纹理、
    // unit3 = per-instance 数据）。
    glActiveTexture(GL_TEXTURE0 + kSmoothDataUnit);
    glBindTexture(GL_TEXTURE_2D, smooth_data_tex_);
    glUniform1i(u.strip_data, kSmoothDataUnit);

    const std::vector<Strip16>& strips = ctx.batch->strips;
    const size_t total = strips.size();
    LOG_FIRST_N(INFO, 3) << "StripRenderer(smooth): strips=" << total
                         << " draw_calls=1";

    // 数据纹理按需扩容（容量翻倍增长，避免条带数每帧抖动时反复重建）。
    if (static_cast<int>(total) > smooth_data_rows_) {
        const int grown = std::max(static_cast<int>(total), smooth_data_rows_ * 2);
        CHECK_LE(grown, max_texture_rows_)
            << "StripRenderer(smooth): 单批条带数超过 GL_MAX_TEXTURE_SIZE（"
            << max_texture_rows_ << "），数据纹理放不下";
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
        AppendSmoothStripData(strips[k], computed, &smooth_data_);
        LOG_FIRST_N(INFO, 6) << "StripSmooth: margin=" << computed.margin
                             << " scale=" << computed.scale;
    }

    // 行号钳位的上界 = 纹理**实际高度**（不是每块条带数）：整批可能远超一块。
    glUniform1f(u.data_rows, static_cast<float>(smooth_data_rows_));

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
