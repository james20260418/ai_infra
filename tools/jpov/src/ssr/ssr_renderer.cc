// JPOV SsrRenderer 实现（屏幕空间反射 v2）。设计见 docs/jpov_ssr_v2_design.md；
// 算法算式见 src/ssr/ssr_math.h；两趟 GLSL 见 src/ssr/ssr_shader.h。
//
// 两趟，没有第三趟：
//   ① march   —— 画 (W·T)×(H·T)，每 texel = 一个 lane，写本段最早命中格内的中点 s
//   ② resolve —— 画 W×H，T² 取 min → s → 命中格 → 取色 + A（调试时顺手写原因位）
//
// 资源全部在 Init / 尺寸变化时创建：**Render 里零分配**（热路径上只有绑定 + draw）。

#define GL_GLEXT_PROTOTYPES

#include "tools/jpov/src/ssr/ssr_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include <GL/gl.h>
#include <GL/glext.h>

#ifdef _WIN32
#ifndef GLOG_NO_ABBREVIATED_SEVERITIES
#define GLOG_NO_ABBREVIATED_SEVERITIES
#endif
#include "third_party/gl_loader-mingw/gl_loader.h"
#endif

#include <glog/logging.h>

#include "tools/jpov/src/ssr/ssr_math.h"
#include "tools/jpov/src/ssr/ssr_shader.h"

namespace jpov {
namespace {

// 后处理 pass 自 unit 0 起绑自己的输入（见 texture_units.h 顶部约定）。
// ⚠️ 两趟的槽位**必须分开编号**：写成"march 的 depth=0、resolve 的 slice=0"会让 resolve 的
//    `glBindTexture(unit0, depth)` 把 slice 顶掉 ⇒ uSlice 采到深度值（≈0.95 < 哨兵）
//    ⇒ **每个像素都被判成命中**。这一条是 CPU/GPU 对拍当场抓出来的（2026-10-29）。
constexpr int kTexUnitMarchDepth = 0;
constexpr int kTexUnitMarchNormal = 1;
constexpr int kTexUnitResolveSlice = 0;
constexpr int kTexUnitResolveDepth = 1;
constexpr int kTexUnitResolveNormal = 2;
constexpr int kTexUnitResolveColor = 3;
constexpr int kTexUnitSize = 4;

bool IsValidTile(int tile) {
    return tile == 1 || tile == 2 || tile == 4 || tile == 8;
}

void MakeTexture(unsigned int* tex, int w, int h, GLint internal_format, GLenum format,
                 GLenum type) {
    glGenTextures(1, tex);
    glBindTexture(GL_TEXTURE_2D, *tex);
    glTexImage2D(GL_TEXTURE_2D, 0, internal_format, w, h, 0, format, type, nullptr);
    // 全部 NEAREST：本模块只用 texelFetch（不插值），且 R32F 在 GL 3.3 下本来就不可线性过滤。
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
}

int TextureWidth(unsigned int tex) {
    GLint w = 0;
    glBindTexture(GL_TEXTURE_2D, tex);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
    glBindTexture(GL_TEXTURE_2D, 0);
    return w;
}

int TextureHeight(unsigned int tex) {
    GLint h = 0;
    glBindTexture(GL_TEXTURE_2D, tex);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
    glBindTexture(GL_TEXTURE_2D, 0);
    return h;
}

}  // namespace

SsrRenderer::SsrRenderer() = default;

SsrRenderer::~SsrRenderer() {
    Finalize();
}

void SsrRenderer::Init(ShaderManager* shader_mgr) {
    CHECK(shader_mgr != nullptr) << "SsrRenderer::Init 需要非空的 ShaderManager";
    if (initialized_) {
        Finalize();
    }
    shader_mgr_ = shader_mgr;

    // 纹理边长上限是**驱动属性**，不写死常量（见 ssr_renderer.h 的 kMaxSliceBytes 注释）。
    GLint max_tex = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_tex);
    CHECK_GT(max_tex, 0) << "GL_MAX_TEXTURE_SIZE 查询失败（GL context 未激活？）";
    max_texture_size_ = static_cast<int>(max_tex);

    // 三份 FS 都是「公共头 + 各自的 main」拼出来的（公共头只此一份，见 ssr_shader.h）。
    // resolve 有带/不带调试附件两个变体（SSR_DEBUG 宏）。
    // 拼装顺序：版本头 → 公共块 → 各自的 main。`#version` 必须在第一行，所以它单独一串。
    const std::string march_src = std::string(kSsrGlslVersion) + kSsrCommonGlsl + kSsrMarchFs;
    const std::string resolve_src = std::string(kSsrGlslVersion) + kSsrCommonGlsl + kSsrResolveFs;
    prog_march_ = shader_mgr_->GetOrCreate("ssr_march", {kSsrFullscreenVs, march_src.c_str(), {}});
    prog_resolve_ = shader_mgr_->GetOrCreate(
        "ssr_resolve", {kSsrFullscreenVs, resolve_src.c_str(), {}});
    prog_resolve_debug_ = shader_mgr_->GetOrCreate(
        "ssr_resolve_debug", {kSsrFullscreenVs, resolve_src.c_str(), {"SSR_DEBUG"}});

    initialized_ = true;
}

void SsrRenderer::Finalize() {
    DestroyTargets();
    // program 由 ShaderManager 统一释放（它按名字缓存，不归本类所有）。
    prog_march_ = 0;
    prog_resolve_ = 0;
    prog_resolve_debug_ = 0;
    shader_mgr_ = nullptr;
    initialized_ = false;
}

void SsrRenderer::DestroyTargets() {
    if (slice_fbo_ != 0) {
        glDeleteFramebuffers(1, &slice_fbo_);
        slice_fbo_ = 0;
    }
    if (slice_tex_ != 0) {
        glDeleteTextures(1, &slice_tex_);
        slice_tex_ = 0;
    }
    if (fbo_ != 0) {
        glDeleteFramebuffers(1, &fbo_);
        fbo_ = 0;
    }
    if (outputs_.radiance_tex != 0) {
        glDeleteTextures(1, &outputs_.radiance_tex);
        outputs_.radiance_tex = 0;
    }
    if (outputs_.reason_tex != 0) {
        glDeleteTextures(1, &outputs_.reason_tex);
        outputs_.reason_tex = 0;
    }
    if (outputs_.hit_s_tex != 0) {
        glDeleteTextures(1, &outputs_.hit_s_tex);
        outputs_.hit_s_tex = 0;
    }
    width_ = 0;
    height_ = 0;
    tile_ = 0;
    debug_ = false;
}

void SsrRenderer::EnsureTargets(int w, int h, int tile, bool debug) {
    if (w == width_ && h == height_ && tile == tile_ && debug == debug_ && fbo_ != 0) {
        return;
    }
    DestroyTargets();
    width_ = w;
    height_ = h;
    tile_ = tile;
    debug_ = debug;

    const int sw = w * tile;
    const int sh = h * tile;

    // ① march 目标：每 texel 一个 lane 的 s（R32F；**不能**用 R16F，s 可到几千像素，
    //    半精度在 2048 处步长已经 2 px，会跳格取错颜色）。
    MakeTexture(&slice_tex_, sw, sh, GL_R32F, GL_RED, GL_FLOAT);
    glGenFramebuffers(1, &slice_fbo_);
    GLint prev_fbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, slice_fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, slice_tex_, 0);
    const GLenum slice_buf = GL_COLOR_ATTACHMENT0;
    glDrawBuffers(1, &slice_buf);
    CHECK_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE))
        << "SSR slice FBO 不完整（" << sw << "×" << sh << "，R32F）";

    // ② resolve 目标：radiance（+ 调试附件）。
    MakeTexture(&outputs_.radiance_tex, w, h, GL_RGBA16F, GL_RGBA, GL_FLOAT);
    glGenFramebuffers(1, &fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           outputs_.radiance_tex, 0);
    std::vector<GLenum> bufs{GL_COLOR_ATTACHMENT0};
    if (debug) {
        // R8UI：一个纹素一个字节，正好装一个 SsrResult（枚举值 0..3）。
        MakeTexture(&outputs_.reason_tex, w, h, GL_R8UI, GL_RED_INTEGER, GL_UNSIGNED_BYTE);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D,
                               outputs_.reason_tex, 0);
        MakeTexture(&outputs_.hit_s_tex, w, h, GL_R32F, GL_RED, GL_FLOAT);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_TEXTURE_2D,
                               outputs_.hit_s_tex, 0);
        bufs.push_back(GL_COLOR_ATTACHMENT1);
        bufs.push_back(GL_COLOR_ATTACHMENT2);
    }
    glDrawBuffers(static_cast<GLsizei>(bufs.size()), bufs.data());
    CHECK_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE))
        << "SSR 输出 FBO 不完整（" << w << "×" << h << "，debug=" << debug << "）";
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prev_fbo));
}

const SsrOutputs& SsrRenderer::Render(const SsrInputs& in) {
    // ── Pre-condition（见头文件；不静默退化）──
    CHECK(initialized_) << "SsrRenderer::Render 前必须先 Init";
    CHECK_NE(in.color_tex, 0u) << "SsrInputs::color_tex 未设置";
    CHECK_NE(in.depth_tex, 0u) << "SsrInputs::depth_tex 未设置";
    CHECK_NE(in.normal_tex, 0u) << "SsrInputs::normal_tex 未设置";
    CHECK_GT(in.width, 0) << "SsrInputs::width 未设置";
    CHECK_GT(in.height, 0) << "SsrInputs::height 未设置";
    CHECK_GT(in.near_plane, 0.0f) << "SsrInputs::near_plane 未设置";
    CHECK_GT(in.far_plane, in.near_plane) << "SsrInputs::far_plane 必须 > near_plane";
    CHECK_GT(in.tan_half_fov_y, 0.0f) << "SsrInputs::tan_half_fov_y 未设置";
    CHECK_GT(in.aspect, 0.0f) << "SsrInputs::aspect 未设置";
    CHECK_GT(in.thickness_pixels, 0.0f) << "SsrInputs::thickness_pixels 未设置";
    CHECK_GT(in.min_thickness_m, 0.0f) << "SsrInputs::min_thickness_m 未设置";
    CHECK_GT(in.max_distance_m, 0.0f) << "SsrInputs::max_distance_m 未设置";
    CHECK_GT(in.max_ray_pixels, 0) << "SsrInputs::max_ray_pixels 未设置";
    CHECK(IsValidTile(in.tile_size))
        << "SsrInputs::tile_size 必须是 1/2/4/8（tile 须为偶数且是 2 的幂，"
           "否则 2×2 quad 会跨 tile，隐式 LOD 出错），得到 " << in.tile_size;

    // 输入尺寸必须与声明的分辨率一致（不一致会静默采到别的像素）。
    CHECK_EQ(TextureWidth(in.color_tex), in.width)
        << "color_tex 的宽度与 SsrInputs::width 不一致";
    CHECK_EQ(TextureHeight(in.color_tex), in.height)
        << "color_tex 的高度与 SsrInputs::height 不一致";
    CHECK_EQ(TextureWidth(in.depth_tex), in.width) << "depth_tex 的宽度与 width 不一致";
    CHECK_EQ(TextureHeight(in.depth_tex), in.height) << "depth_tex 的高度与 height 不一致";
    CHECK_EQ(TextureWidth(in.normal_tex), in.width) << "normal_tex 的宽度与 width 不一致";
    CHECK_EQ(TextureHeight(in.normal_tex), in.height) << "normal_tex 的高度与 height 不一致";

    // 纹理边长 / 显存预算（动态，不写死）。
    const long long slice_texels =
        static_cast<long long>(in.width) * in.tile_size * in.height * in.tile_size;
    CHECK_LE(in.width * in.tile_size, max_texture_size_)
        << "width×tile = " << in.width * in.tile_size << " 超 GL_MAX_TEXTURE_SIZE "
        << max_texture_size_ << "（调小 tile 或分辨率）";
    CHECK_LE(in.height * in.tile_size, max_texture_size_)
        << "height×tile = " << in.height * in.tile_size << " 超 GL_MAX_TEXTURE_SIZE "
        << max_texture_size_ << "（调小 tile 或分辨率）";
    CHECK_LE(slice_texels * 4, kMaxSliceBytes)
        << "slice 缓冲 " << (slice_texels * 4) / (1024 * 1024) << " MB 超上限 "
        << kMaxSliceBytes / (1024 * 1024) << " MB（W·T×H·T 个 R32F）";

    const ssr::DerivedParams derived =
        ssr::DeriveParams(in.tile_size, in.max_ray_pixels, in.thickness_pixels,
                          in.tan_half_fov_y, in.height);

    // 段长 < 4 px ⇒ 相邻 lane 重复判同一格（判据次数 ×(1+1/L)）。只是浪费，**不失败**；
    // 且**只在配置变化时打一条** —— 每帧刷屏会把真错误淹掉。
    if (ssr::SlabTooShortForEfficiency(derived)) {
        if (in.tile_size != warned_tile_ || in.max_ray_pixels != warned_max_ray_pixels_) {
            LOG(WARNING) << "SSR: 段长只有 " << derived.slab_pixels << " px（tile=" << in.tile_size
                         << ", max_ray_pixels=" << in.max_ray_pixels
                         << "）⇒ 相邻 lane 会重复判同一格，判据次数约 ×"
                         << (1.0f + 1.0f / std::max(1.0e-6f, derived.slab_pixels))
                         << "。口诀：tile² ≤ max_ray_pixels/4（T=1 任意 / 2 需 16 / 4 需 64 /"
                            " 8 需 256）";
            warned_tile_ = in.tile_size;
            warned_max_ray_pixels_ = in.max_ray_pixels;
        }
    } else {
        warned_tile_ = 0;
        warned_max_ray_pixels_ = 0;
    }

    // 皮肤检查（契约 6）：不许把本模块自己的输出再喂回来。
    CHECK(in.color_tex != outputs_.radiance_tex && in.depth_tex != outputs_.radiance_tex &&
              in.normal_tex != outputs_.radiance_tex)
        << "输入纹理是本模块自己的 radiance_tex（反馈环）";
    CHECK(in.color_tex != outputs_.hit_s_tex && in.depth_tex != outputs_.hit_s_tex &&
              in.normal_tex != outputs_.hit_s_tex)
        << "输入纹理是本模块自己的 hit_s_tex（反馈环）";

    // ── 保存调用方的 GL 状态（返回前原样恢复）──
    // ⚠️ 必须在 EnsureTargets / 尺寸查询**之前**：它们会临时绑纹理与 FBO，
    //    先保存就会把"被临时绑过的值"当成调用方状态再恢复回去。
    GLint prev_fbo = 0;
    GLint prev_viewport[4] = {0, 0, 0, 0};
    GLint prev_active = 0;
    GLint prev_blend = 0;
    GLint prev_depth = 0;
    GLint prev_cull = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    glGetIntegerv(GL_VIEWPORT, prev_viewport);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_active);
    prev_blend = glIsEnabled(GL_BLEND);
    prev_depth = glIsEnabled(GL_DEPTH_TEST);
    prev_cull = glIsEnabled(GL_CULL_FACE);
    GLint prev_bind[kTexUnitSize] = {0, 0, 0, 0};
    for (int u = 0; u < kTexUnitSize; ++u) {
        glActiveTexture(GL_TEXTURE0 + u);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_bind[u]);
    }
    GLint prev_program = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prev_program);

    EnsureTargets(in.width, in.height, in.tile_size, in.debug_reason);

    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

    // ── ① march：(W·T)×(H·T)，每 texel 一个 lane ──
    glBindFramebuffer(GL_FRAMEBUFFER, slice_fbo_);
    glViewport(0, 0, in.width * in.tile_size, in.height * in.tile_size);
    glUseProgram(prog_march_);
    auto set1i = [&](unsigned int prog, const char* name, int v) {
        const int loc = shader_mgr_->GetUniform(prog, name);
        if (loc >= 0) {
            glUniform1i(loc, v);
        }
    };
    auto set1f = [&](unsigned int prog, const char* name, float v) {
        const int loc = shader_mgr_->GetUniform(prog, name);
        if (loc >= 0) {
            glUniform1f(loc, v);
        }
    };
    auto set2f = [&](unsigned int prog, const char* name, float x, float y) {
        const int loc = shader_mgr_->GetUniform(prog, name);
        if (loc >= 0) {
            glUniform2f(loc, x, y);
        }
    };
    auto upload_common = [&](unsigned int prog) {
        set2f(prog, "uRes", static_cast<float>(in.width), static_cast<float>(in.height));
        set1f(prog, "uNear", in.near_plane);
        set1f(prog, "uFar", in.far_plane);
        set1f(prog, "uTanHalf", in.tan_half_fov_y);
        set1f(prog, "uAspect", in.aspect);
        set1f(prog, "uMaxDistance", in.max_distance_m);
        set1f(prog, "uThicknessC", derived.thickness_coefficient);
        set1f(prog, "uMinThickness", in.min_thickness_m);
        set1f(prog, "uSentinel", ssr::kNoHit);
        set1i(prog, "uTile", in.tile_size);
        set1f(prog, "uSlabPixels", derived.slab_pixels);
        set1i(prog, "uStepsPerLane", derived.steps_per_lane);
    };
    upload_common(prog_march_);
    glActiveTexture(GL_TEXTURE0 + kTexUnitMarchDepth);
    glBindTexture(GL_TEXTURE_2D, in.depth_tex);
    glActiveTexture(GL_TEXTURE0 + kTexUnitMarchNormal);
    glBindTexture(GL_TEXTURE_2D, in.normal_tex);
    set1i(prog_march_, "uDepth", kTexUnitMarchDepth);
    set1i(prog_march_, "uNormal", kTexUnitMarchNormal);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    // ── ② resolve：W×H，T² 取 min → 命中格颜色 + A ──
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glViewport(0, 0, in.width, in.height);
    const unsigned int prog_resolve = in.debug_reason ? prog_resolve_debug_ : prog_resolve_;
    glUseProgram(prog_resolve);
    upload_common(prog_resolve);
    glActiveTexture(GL_TEXTURE0 + kTexUnitResolveSlice);
    glBindTexture(GL_TEXTURE_2D, slice_tex_);
    glActiveTexture(GL_TEXTURE0 + kTexUnitResolveDepth);
    glBindTexture(GL_TEXTURE_2D, in.depth_tex);
    glActiveTexture(GL_TEXTURE0 + kTexUnitResolveNormal);
    glBindTexture(GL_TEXTURE_2D, in.normal_tex);
    glActiveTexture(GL_TEXTURE0 + kTexUnitResolveColor);
    glBindTexture(GL_TEXTURE_2D, in.color_tex);
    set1i(prog_resolve, "uSlice", kTexUnitResolveSlice);
    set1i(prog_resolve, "uDepth", kTexUnitResolveDepth);
    set1i(prog_resolve, "uNormal", kTexUnitResolveNormal);
    set1i(prog_resolve, "uColor", kTexUnitResolveColor);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    // ── 恢复调用方的状态 ──
    glUseProgram(static_cast<GLuint>(prev_program));
    for (int u = 0; u < kTexUnitSize; ++u) {
        glActiveTexture(GL_TEXTURE0 + u);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prev_bind[u]));
    }
    glActiveTexture(static_cast<GLenum>(prev_active));
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prev_fbo));
    glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);
    if (prev_blend != 0) {
        glEnable(GL_BLEND);
    }
    if (prev_depth != 0) {
        glEnable(GL_DEPTH_TEST);
    }
    if (prev_cull != 0) {
        glEnable(GL_CULL_FACE);
    }

    return outputs_;
}

}  // namespace jpov
