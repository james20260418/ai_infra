// JPOV SSR — 输入准备（降采样）实现。设计与选点规则见 ssr_downsample.h 顶部。

#define GL_GLEXT_PROTOTYPES

#include "tools/jpov/src/ssr/ssr_downsample.h"

#include <GL/gl.h>
#include <GL/glext.h>

#ifdef _WIN32
#ifndef GLOG_NO_ABBREVIATED_SEVERITIES
#define GLOG_NO_ABBREVIATED_SEVERITIES
#endif
#include "third_party/gl_loader-mingw/gl_loader.h"
#endif

#include <glog/logging.h>

#include "tools/jpov/src/shader_manager.h"
#include "tools/jpov/src/ssr/ssr_math.h"
#include "tools/jpov/src/ssr/ssr_shader.h"

namespace jpov {
namespace {

// 全分辨率 (color, depth, attr, slab) → SSR 分辨率 (color, slab, normal, roughness)。
//
// 颜色/法线/材质/"起点深度"四样都取自**同一个 texel**（窗口深度最小的那个）：
// 跨表面的材质混进这条求交链，后果是"某块本该跳过的像素发了射线"，比颜色取错更难查。
//
// **柱体（slab）恰好相反，必须跨子像素求并集**：
//   每个子像素的柱体只描述它自己那个像素方框内的面；而 march 的格子覆盖 N×N 个像素，
//   只在 1/N² 的面积上有资格说话 ⇒ 直接取最近那个子像素的柱体会漏掉 3/4 的 z 跨度
//   （掠射面尤其致命：实测同一帧地面 dz/texel 0.011 m → 0.87 m）。子像素方框**严丝合缝
//   地铺满**格子，所以并集就是整格的包络 —— 这也是降采样倍率不必泄漏进几何 pass 的全部理由。
//
// 并集是**连通**的（见 kSsrUnionGlsl）：轮廓处"近物 + 远墙"不能拼成一根贯通几十米的柱子。
constexpr const char* kSsrDownsampleFs = R"glsl(
#version 330 core
uniform sampler2D uColor;
uniform sampler2D uDepth;
uniform sampler2D uAttr;
uniform sampler2D uAttrMat;   // 属性纹理附件1：rgb = F0，a = roughness
uniform sampler2D uSlabHi;    // 属性纹理附件2（全分辨率 RG32F）：[front, back]，0/0 = 空格
uniform ivec2 uFullRes;
uniform int   uScale;      // 1 = 直通；2 = 2×2 选一
uniform float uNear;
uniform float uFar;
uniform float uMergeEps;   // 连通判据的相对容差（ssr_math.h::kSlabMergeEpsilon）

layout(location = 0) out vec4  FragColor;
layout(location = 1) out vec2  FragSlab;     // **体素柱 [front, back]**（w = 1/z_view；front ≤ 0 = 空）
layout(location = 2) out vec4  FragNormal;   // rgb = 三维视空间法线；a = 掩码编码
layout(location = 3) out float FragRough;    // 反射层 roughness（发射线前的截止用）

// 窗口深度 → 视距（米）。与 ssr_math.h::DepthFromWindowDepth 逐字同式（含 NaN 守卫）。
float SsrZViewFromWindow(float depth, float zn, float zf) {
    float ndc = 2.0 * depth - 1.0;
    float denom = (zf + zn) - ndc * (zf - zn);
    if (!(abs(denom) > 1.0e-6)) {
        return max(zn, 1.0e-4);
    }
    return (2.0 * zn * zf) / denom;
}

// 两根柱体是否相接（闭区间相交 + 相对 ε）。ssr_math.h::SlabsTouch 逐字同一形式。
bool SsrSlabsTouch(vec2 a, vec2 b) {
    if (!(a.x > 0.0) || !(b.x > 0.0)) {
        return false;
    }
    float eps = uMergeEps * max(a.x, b.x);
    return max(a.y, b.y) <= min(a.x, b.x) + eps;
}

const int kMaxSub = 16;   // uScale ≤ 4 ⇒ 最多 4×4

void main() {
    ivec2 q = ivec2(gl_FragCoord.xy);
    ivec2 base = q * uScale;
    int count = uScale * uScale;

    // ── ① 每个子像素的柱体 + "最近"的选择 ──
    vec2 sub[kMaxSub];
    float best_z = 2.0;
    ivec2 best = clamp(base, ivec2(0), uFullRes - 1);
    for (int j = 0; j < uScale; ++j) {
        for (int i = 0; i < uScale; ++i) {
            ivec2 p = clamp(base + ivec2(i, j), ivec2(0), uFullRes - 1);
            float z = texelFetch(uDepth, p, 0).r;
            if (z < best_z) {
                best_z = z;
                best = p;
            }
            // w = 1/z_view（0 = 天空/远平面）。
            float w_depth = (z < 1.0) ? (1.0 / SsrZViewFromWindow(z, uNear, uFar)) : 0.0;
            vec2 s = texelFetch(uSlabHi, p, 0).rg;
            if (s.x > 0.0 && w_depth > 0.0) {
                // 深度值本身必定落在这张面上 ⇒ 用它做**单向加宽**（只会更粗，不会更薄）：
                // 属性 pass 若漏了某类几何，这里就退回"零厚度"（前一版的基线），不会把遮挡丢掉。
                s = vec2(max(s.x, w_depth), min(s.y, w_depth));
            } else if (!(s.x > 0.0)) {
                s = vec2(w_depth, w_depth);   // 属性 pass 没数据 ⇒ 零厚度兵底（w_depth = 0 ⇒ 空）
            }
            sub[j * uScale + i] = s;
        }
    }

    // ── ② 连通并集（从最近的子像素出发，只并入与当前区间相接的）──
    int seed = 0;
    for (int k = 1; k < count; ++k) {
        if (sub[k].x > sub[seed].x) {
            seed = k;
        }
    }
    vec2 acc = sub[seed];
    if (!(acc.x > 0.0)) {
        acc = vec2(0.0, 0.0);   // 整格无几何
    } else {
        // 迭代到不变：A↔B、B↔C 但 A↛C 的链要能合上。count ≤ 16 ⇒ 上限 16 轮必收敛。
        for (int pass = 0; pass < kMaxSub; ++pass) {
            bool changed = false;
            for (int k = 0; k < kMaxSub; ++k) {
                if (k >= count || k == seed) {
                    continue;
                }
                if (!SsrSlabsTouch(acc, sub[k])) {
                    continue;
                }
                vec2 merged = vec2(max(acc.x, sub[k].x), min(acc.y, sub[k].y));
                if (merged != acc) {
                    changed = true;
                }
                acc = merged;
            }
            if (!changed) {
                break;
            }
        }
    }
    FragSlab = acc;

    // ── ③ 颜色/法线/材质：**同源**（那个最近的 texel）──
    FragColor  = texelFetch(uColor, best, 0);
    vec4 attr  = texelFetch(uAttr, best, 0);
    FragNormal = attr;   // rgb = 三维视空间法线，a = 掩码编码
    // 粗糙度必须与前三张**同源**：跨表面的材质混进这条求交链，
    // 后果是"某块本该跳过的像素发了射线"，比颜色取错更难查。
    FragRough = texelFetch(uAttrMat, best, 0).a;
}
)glsl";

constexpr int kTexUnitColor = 0;
constexpr int kTexUnitDepth = 1;
constexpr int kTexUnitAttr = 2;
constexpr int kTexUnitAttrMat = 3;
constexpr int kTexUnitSlabHi = 4;
constexpr int kTexUnitCount = 5;

}  // namespace

SsrDownsample::SsrDownsample() = default;
SsrDownsample::~SsrDownsample() { Finalize(); }

void SsrDownsample::Init(ShaderManager* shader_mgr) {
    CHECK(shader_mgr != nullptr);
    shader_mgr_ = shader_mgr;
    prog_ = shader_mgr_->GetOrCreate("ssr_downsample", {kSsrFullscreenVs, kSsrDownsampleFs, {}});
    CHECK_NE(prog_, 0u);
}

void SsrDownsample::Finalize() {
    DestroyTargets();
    prog_ = 0;
    shader_mgr_ = nullptr;
}

void SsrDownsample::DestroyTargets() {
    if (fbo_ != 0) {
        glDeleteFramebuffers(1, &fbo_);
        fbo_ = 0;
    }
    if (color_tex_ != 0) {
        glDeleteTextures(1, &color_tex_);
        color_tex_ = 0;
    }
    if (slab_tex_ != 0) {
        glDeleteTextures(1, &slab_tex_);
        slab_tex_ = 0;
    }
    if (normal_tex_ != 0) {
        glDeleteTextures(1, &normal_tex_);
        normal_tex_ = 0;
    }
    if (rough_tex_ != 0) {
        glDeleteTextures(1, &rough_tex_);
        rough_tex_ = 0;
    }
    width_ = 0;
    height_ = 0;
}

void SsrDownsample::EnsureTargets(int w, int h) {
    if (w == width_ && h == height_ && fbo_ != 0) {
        return;
    }
    DestroyTargets();
    width_ = w;
    height_ = h;

    auto make_tex = [](unsigned int* tex, GLint internal, GLenum format, int tw, int th) {
        glGenTextures(1, tex);
        glBindTexture(GL_TEXTURE_2D, *tex);
        glTexImage2D(GL_TEXTURE_2D, 0, internal, tw, th, 0, format, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
    };
    make_tex(&color_tex_, GL_RGBA16F, GL_RGBA, w, h);
    make_tex(&slab_tex_, GL_RG32F, GL_RG, w, h);
    make_tex(&normal_tex_, GL_RGBA16F, GL_RGBA, w, h);
    make_tex(&rough_tex_, GL_R16F, GL_RED, w, h);
    // ⚠️ 命中取色走**双线性**（resolve 里 texture(uColor, pos/uRes)）⇒ 必须开 LINEAR。
    //    其它三张被 texelFetch 精确读取，保持 NEAREST（避免任何意外的插值）。
    glBindTexture(GL_TEXTURE_2D, color_tex_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, 0);

    glGenFramebuffers(1, &fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color_tex_, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, slab_tex_, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_TEXTURE_2D, normal_tex_, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT3, GL_TEXTURE_2D, rough_tex_, 0);
    const GLenum bufs[4] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2,
                            GL_COLOR_ATTACHMENT3};
    glDrawBuffers(4, bufs);
    CHECK_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE))
        << "SSR 降采样 FBO 不完整（" << w << "×" << h << "）";
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void SsrDownsample::Run(unsigned int full_color_tex, unsigned int full_depth_tex,
                        unsigned int full_attr_tex, unsigned int full_attr_mat_tex,
                        unsigned int full_slab_tex, int full_w, int full_h, int scale,
                        float near_plane, float far_plane) {
    CHECK(shader_mgr_ != nullptr) << "SsrDownsample::Run 前必须先 Init";
    CHECK_NE(full_color_tex, 0u);
    CHECK_NE(full_depth_tex, 0u);
    CHECK_NE(full_attr_tex, 0u);
    CHECK_NE(full_attr_mat_tex, 0u) << "缺属性纹理的材质附件（F0/roughness）";
    CHECK_NE(full_slab_tex, 0u) << "缺属性纹理的体素柱附件（[front, back]）";
    CHECK_GT(full_w, 0);
    CHECK_GT(full_h, 0);
    CHECK_GE(scale, 1);
    CHECK_LE(scale, 4) << "降采样倍率只支持 1..4（resolution_scale = 1/N）";
    CHECK_GT(near_plane, 0.0f);
    CHECK_GT(far_plane, near_plane) << "窗口深度 → w 的换算需要合法相机";

    int w = 0;
    int h = 0;
    DownsampleTargetSize(full_w, full_h, scale, &w, &h);

    GLint prev_fbo = 0;
    GLint prev_viewport[4] = {0, 0, 0, 0};
    GLint prev_active = 0;
    GLint prev_prog = 0;
    GLint prev_blend = 0;
    GLint prev_depth = 0;
    GLint prev_cull = 0;
    GLint prev_scissor = 0;
    GLint prev_bind[kTexUnitCount] = {0, 0, 0, 0};
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    glGetIntegerv(GL_VIEWPORT, prev_viewport);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_active);
    glGetIntegerv(GL_CURRENT_PROGRAM, &prev_prog);
    prev_blend = glIsEnabled(GL_BLEND);
    prev_depth = glIsEnabled(GL_DEPTH_TEST);
    prev_cull = glIsEnabled(GL_CULL_FACE);
    prev_scissor = glIsEnabled(GL_SCISSOR_TEST);
    for (int u = 0; u < kTexUnitCount; ++u) {
        glActiveTexture(GL_TEXTURE0 + u);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_bind[u]);
    }

    EnsureTargets(w, h);

    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glViewport(0, 0, w, h);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);     // 全屏三角形：不能被剔除
    glDisable(GL_SCISSOR_TEST);  // 调用方开着 scissor 会把全屏 pass 裁成一小块
    glUseProgram(prog_);

    glActiveTexture(GL_TEXTURE0 + kTexUnitColor);
    glBindTexture(GL_TEXTURE_2D, full_color_tex);
    glActiveTexture(GL_TEXTURE0 + kTexUnitDepth);
    glBindTexture(GL_TEXTURE_2D, full_depth_tex);
    glActiveTexture(GL_TEXTURE0 + kTexUnitAttr);
    glBindTexture(GL_TEXTURE_2D, full_attr_tex);
    glActiveTexture(GL_TEXTURE0 + kTexUnitAttrMat);
    glBindTexture(GL_TEXTURE_2D, full_attr_mat_tex);
    glActiveTexture(GL_TEXTURE0 + kTexUnitSlabHi);
    glBindTexture(GL_TEXTURE_2D, full_slab_tex);
    glUniform1i(shader_mgr_->GetUniform(prog_, "uColor"), kTexUnitColor);
    glUniform1i(shader_mgr_->GetUniform(prog_, "uDepth"), kTexUnitDepth);
    glUniform1i(shader_mgr_->GetUniform(prog_, "uAttr"), kTexUnitAttr);
    glUniform1i(shader_mgr_->GetUniform(prog_, "uAttrMat"), kTexUnitAttrMat);
    glUniform1i(shader_mgr_->GetUniform(prog_, "uSlabHi"), kTexUnitSlabHi);
    glUniform1f(shader_mgr_->GetUniform(prog_, "uMergeEps"), ssr::kSlabMergeEpsilon);
    glUniform2i(shader_mgr_->GetUniform(prog_, "uFullRes"), full_w, full_h);
    glUniform1i(shader_mgr_->GetUniform(prog_, "uScale"), scale);
    glUniform1f(shader_mgr_->GetUniform(prog_, "uNear"), near_plane);
    glUniform1f(shader_mgr_->GetUniform(prog_, "uFar"), far_plane);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    // 恢复调用方状态
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prev_fbo));
    glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);
    glUseProgram(static_cast<GLuint>(prev_prog));
    if (prev_blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (prev_depth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (prev_cull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    if (prev_scissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    for (int u = 0; u < kTexUnitCount; ++u) {
        glActiveTexture(GL_TEXTURE0 + u);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prev_bind[u]));
    }
    glActiveTexture(static_cast<GLenum>(prev_active));
}

}  // namespace jpov
