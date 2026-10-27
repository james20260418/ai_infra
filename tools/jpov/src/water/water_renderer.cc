// JPOV WaterRenderer 实现
//
// 两个 pass 的 GL 细节见头文件顶部说明；这里只强调三条硬约束：
//   1. 采样反馈环：水面/介质的输入必须是**抓帧副本**（blit 出来的 color + MRT 深度），
//      不能直接采样正在写的 FBO 附件（GL 未定义）。
//   2. MRT 契约：任何写 HDR FBO 的 shader 都必须写 attachment1（场景深度），
//      否则该像素的"场景深度"未定义 —— 雾 / 粒子软深度 / 条带裁剪都会读到垃圾。
//   3. 水面写硬件深度：它之后画的粒子/火/条带靠"手动作深度测试"判断遮挡，
//      依据正是 MRT#1 —— 水面写进去，水下那些效果才会被正确挡住。

#define GL_GLEXT_PROTOTYPES

#include "tools/jpov/src/water/water_renderer.h"

#include <algorithm>
#include <cmath>
#include <vector>

// GL 头文件必须最先 include（在 MinGW #define 宏替换之前）。
// ⚠️ **两个平台都要** <GL/glext.h>：MinGW 的 GL/gl.h 只到 1.1，FBO / 附件枚举
//    （GL_COLOR_ATTACHMENT1 等）不在里面 —— 只在 Linux 分支里 include 会过 Linux、
//    挂 Windows 交叉编译（与 volume_manager.cc 同一坑）。
#include <GL/gl.h>
#include <GL/glext.h>

#ifdef _WIN32
// MinGW: windows.h 定义 ERROR 宏与 glog 冲突，必须在 glog 之前 suppress
#ifndef GLOG_NO_ABBREVIATED_SEVERITIES
#define GLOG_NO_ABBREVIATED_SEVERITIES
#endif
#include "third_party/gl_loader-mingw/gl_loader.h"
#endif

// Windows/MinGW: windef.h 定义 near/far 宏，与 Camera::near/far 字段冲突。
#ifdef _WIN32
#undef near
#undef far
#endif

#include <glog/logging.h>

#include "tools/jpov/src/water/water_shader.h"

namespace {

// 极坐标 LOD 网格参数。
//
// 结构 = 「近场幂律环」+「远场几何环」，中心**没有洞**：
//   · 第 0 圈半径 = 0（96 个顶点重合在相机正下方）⇒ 与第 1 圈之间自动退化成
//     "扇形"（每个四边形里两个三角形退化为零面积）。**曾经写成内半径 0.35 m，
//     相机贴近水面（水位 ≈ 相机高度）时那个洞会在屏幕上张成一个巨大的缺口，
//     直接从水里看到海床**（2026-10-27 Danis 报的"漏出大量 box"）。
//   · 近场 0 → kNearRadius 用幂律（r = R·(i/N)^1.6）：中心密（间距 ~0.5 m）、
//     外沿疏（~5.7 m）—— 这个间距决定了**能表示多短的浪**（≥ 4~6 个采样/波长）。
//   · 远场 kNearRadius → kOuterRadius 用几何级数：这段只负责铺到地平线，浪幅已被
//     VS 里的距离淡出压到 0（否则 30 m 间距的四边形会把浪画成折角多边形）。
//   · 最外圈 900 m：相机在水面 2 m 高时，外圈相对真地平线下沉 ≈ 0.13°（≈ 2 px，
//     交给远景雾吃掉）；相机 far 比 900 小时会被远平面裁掉（无副作用）。
constexpr int kNearRings = 56;
constexpr int kFarRings = 12;
constexpr int kRings = kNearRings + kFarRings;
constexpr int kSegments = 96;
constexpr float kNearRadius = 200.0f;
constexpr float kOuterRadius = 900.0f;

// 4x4 列主序求逆（Gauss-Jordan + 部分主元）。只用于把 NDC 反投影回世界算距离，
// 所以不追求性能（每帧一次，在 CPU）。
void InvertMat4(const float m[16], float out[16]) {
    float a[4][8];
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            a[r][c] = m[c * 4 + r];       // 列主序 → 行主序
            a[r][c + 4] = (r == c) ? 1.0f : 0.0f;
        }
    }
    for (int col = 0; col < 4; ++col) {
        int pivot = col;
        for (int r = col + 1; r < 4; ++r) {
            if (std::fabs(a[r][col]) > std::fabs(a[pivot][col])) {
                pivot = r;
            }
        }
        if (std::fabs(a[pivot][col]) < 1.0e-12f) {
            // 理论上不会发生（投影矩阵可逆）；发生了也不要静默写出 NaN 矩阵。
            LOG(FATAL) << "WaterRenderer: view_proj 奇异，无法求逆";
        }
        if (pivot != col) {
            for (int c = 0; c < 8; ++c) {
                const float t = a[col][c];
                a[col][c] = a[pivot][c];
                a[pivot][c] = t;
            }
        }
        const float inv = 1.0f / a[col][col];
        for (int c = 0; c < 8; ++c) {
            a[col][c] *= inv;
        }
        for (int r = 0; r < 4; ++r) {
            if (r == col) {
                continue;
            }
            const float f = a[r][col];
            if (f == 0.0f) {
                continue;
            }
            for (int c = 0; c < 8; ++c) {
                a[r][c] -= f * a[col][c];
            }
        }
    }
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            out[c * 4 + r] = a[r][c + 4];     // 行主序 → 列主序
        }
    }
}

}  // namespace

namespace jpov {

void WaterRenderer::Init(ShaderManager* shader_mgr) {
    CHECK(shader_mgr != nullptr);
    shader_mgr_ = shader_mgr;
    prog_surface_ = shader_mgr_->GetOrCreate("water_surface", {kWaterVs, kWaterSurfaceFs});
    prog_medium_ = shader_mgr_->GetOrCreate("water_medium", {kWaterMediumVs, kWaterMediumFs});
    EnsureMesh();

    // 全屏三角形用的空 VAO（core profile 下没有绑 VAO 的 draw 非法）。
    if (fs_vao_ == 0) {
        glGenVertexArrays(1, &fs_vao_);
    }
}

void WaterRenderer::Finalize() {
    DestroyCapture();
    if (ebo_) {
        glDeleteBuffers(1, &ebo_);
        ebo_ = 0;
    }
    if (vbo_) {
        glDeleteBuffers(1, &vbo_);
        vbo_ = 0;
    }
    if (vao_) {
        glDeleteVertexArrays(1, &vao_);
        vao_ = 0;
    }
    if (fs_vao_) {
        glDeleteVertexArrays(1, &fs_vao_);
        fs_vao_ = 0;
    }
    index_count_ = 0;
    prog_surface_ = 0;
    prog_medium_ = 0;
    shader_mgr_ = nullptr;
}

bool WaterRenderer::IsCameraUnderwater(const WaterSurface& water, const Camera& cam) {
    // 留 1 mm 余量：相机贴在水面时不要来回抖成"水上/水下"两种画面。
    return cam.position.y() < water.level - 1.0e-3f;
}

void WaterRenderer::EnsureMesh() {
    if (vao_ != 0) {
        return;
    }
    const int vert_count = (kRings + 1) * kSegments;
    std::vector<float> verts;
    verts.reserve(static_cast<size_t>(vert_count) * 2);
    for (int i = 0; i <= kRings; ++i) {
        float r = 0.0f;
        if (i <= kNearRings) {
            const float t = static_cast<float>(i) / static_cast<float>(kNearRings);
            r = kNearRadius * std::pow(t, 1.6f);          // 幂律：中心密、外沿疏
        } else {
            const float t = static_cast<float>(i - kNearRings)
                          / static_cast<float>(kFarRings);
            r = kNearRadius * std::pow(kOuterRadius / kNearRadius, t);
        }
        for (int j = 0; j < kSegments; ++j) {
            const float ang = static_cast<float>(j) * 6.28318530718f
                            / static_cast<float>(kSegments);
            verts.push_back(r * std::cos(ang));
            verts.push_back(r * std::sin(ang));
        }
    }

    std::vector<unsigned int> idx;
    idx.reserve(static_cast<size_t>(kRings) * kSegments * 6);
    for (int i = 0; i < kRings; ++i) {
        for (int j = 0; j < kSegments; ++j) {
            const int jn = (j + 1) % kSegments;
            const unsigned int a = static_cast<unsigned int>(i * kSegments + j);
            const unsigned int b = static_cast<unsigned int>(i * kSegments + jn);
            const unsigned int c = static_cast<unsigned int>((i + 1) * kSegments + j);
            const unsigned int d = static_cast<unsigned int>((i + 1) * kSegments + jn);
            // (a,b,d) 与 (a,d,c)：从 +Y 俯视为逆时针（与 glFrontFace(GL_CCW) 一致）。
            idx.push_back(a);
            idx.push_back(b);
            idx.push_back(d);
            idx.push_back(a);
            idx.push_back(d);
            idx.push_back(c);
        }
    }
    index_count_ = static_cast<int>(idx.size());

    glGenVertexArrays(1, &vao_);
    glBindVertexArray(vao_);
    glGenBuffers(1, &vbo_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(verts.size() * sizeof(float)),
                 verts.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float),
                          reinterpret_cast<void*>(0));
    glGenBuffers(1, &ebo_);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo_);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(idx.size() * sizeof(unsigned int)),
                 idx.data(), GL_STATIC_DRAW);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

void WaterRenderer::EnsureCapture(int w, int h) {
    if (cap_fbo_ != 0 && cap_w_ == w && cap_h_ == h) {
        return;
    }
    DestroyCapture();

    glGenTextures(1, &cap_color_tex_);
    glBindTexture(GL_TEXTURE_2D, cap_color_tex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenTextures(1, &cap_depth_tex_);
    glBindTexture(GL_TEXTURE_2D, cap_depth_tex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, w, h, 0, GL_RED, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    glGenFramebuffers(1, &cap_fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, cap_fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, cap_color_tex_, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1,
                           GL_TEXTURE_2D, cap_depth_tex_, 0);
    const GLenum bufs[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
    glDrawBuffers(2, bufs);
    CHECK_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), GL_FRAMEBUFFER_COMPLETE)
        << "WaterRenderer: 抓帧 FBO 不完整";
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    cap_w_ = w;
    cap_h_ = h;
}

void WaterRenderer::DestroyCapture() {
    if (cap_fbo_) {
        glDeleteFramebuffers(1, &cap_fbo_);
        cap_fbo_ = 0;
    }
    if (cap_color_tex_) {
        glDeleteTextures(1, &cap_color_tex_);
        cap_color_tex_ = 0;
    }
    if (cap_depth_tex_) {
        glDeleteTextures(1, &cap_depth_tex_);
        cap_depth_tex_ = 0;
    }
    cap_w_ = 0;
    cap_h_ = 0;
}

void WaterRenderer::Capture(int w, int h, unsigned int fbo_hdr) {
    EnsureCapture(w, h);
    CHECK_NE(fbo_hdr, 0u);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo_hdr);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, cap_fbo_);
    // 两个附件分开 blit：显式指定 read/draw buffer，避免依赖"两边 draw buffer 布局一致"。
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glDrawBuffer(GL_COLOR_ATTACHMENT0);
    glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glReadBuffer(GL_COLOR_ATTACHMENT1);
    glDrawBuffer(GL_COLOR_ATTACHMENT1);
    glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    // 先把 read buffer 还原到 fbo_hdr 自己的状态，**再**解绑。
    // ⚠️ 若在绑回默认 FBO(0) 之后再调 glReadBuffer(GL_COLOR_ATTACHMENT0)，
    //    那是非法枚举（默认帧缓冲没有颜色附件）⇒ GL_INVALID_OPERATION(1282)，
    //    而且错误会被下一个 glGetError 的 pass 背锅（实测报在 object3d draw 上）。
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void WaterRenderer::DrawSurface(const WaterSurface& water, const Camera& cam,
                                int fbo_w, int fbo_h,
                                unsigned int fbo_hdr,
                                unsigned int color_tex, unsigned int scene_depth_tex,
                                unsigned int env_cube, float env_max_lod,
                                const float view_proj[16]) {
    CHECK(shader_mgr_ != nullptr) << "WaterRenderer::Init 未调用";
    CHECK_GT(fbo_w, 0);
    CHECK_GT(fbo_h, 0);
    CHECK_NE(fbo_hdr, 0u);
    CHECK_NE(color_tex, 0u);
    CHECK_NE(scene_depth_tex, 0u);
    CHECK_NE(env_cube, 0u) << "水面反射需要天光 cubemap（Renderer 应先建探针）";

    Capture(fbo_w, fbo_h, fbo_hdr);

    float inv_vp[16];
    InvertMat4(view_proj, inv_vp);

    glPushAttrib(GL_ENABLE_BIT | GL_DEPTH_BUFFER_BIT | GL_VIEWPORT_BIT);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_hdr);
    const GLenum bufs[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
    glDrawBuffers(2, bufs);
    glViewport(0, 0, fbo_w, fbo_h);

    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LESS);
    glDisable(GL_CULL_FACE);      // 从水下往上看时看到的是背面
    glDisable(GL_BLEND);          // 颜色已在 shader 里算好（含背景吸收）

    glUseProgram(prog_surface_);
    glUniformMatrix4fv(shader_mgr_->GetUniform(prog_surface_, "uViewProj"),
                       1, GL_FALSE, view_proj);
    glUniformMatrix4fv(shader_mgr_->GetUniform(prog_surface_, "uInvVP"),
                       1, GL_FALSE, inv_vp);
    glUniform2f(shader_mgr_->GetUniform(prog_surface_, "uResolution"),
                static_cast<float>(fbo_w), static_cast<float>(fbo_h));
    glUniform3f(shader_mgr_->GetUniform(prog_surface_, "uCameraPos"),
                cam.position.x(), cam.position.y(), cam.position.z());
    glUniform1f(shader_mgr_->GetUniform(prog_surface_, "uLevel"), water.level);

    // 波浪：方向归一化后传给 VS（非零即可，内部按 0/±40° 旋开）。
    const float wl = std::sqrt(water.wave_dir.x() * water.wave_dir.x()
                             + water.wave_dir.z() * water.wave_dir.z());
    const float wnx = (wl > 1.0e-6f) ? water.wave_dir.x() / wl : 1.0f;
    const float wnz = (wl > 1.0e-6f) ? water.wave_dir.z() / wl : 0.0f;
    glUniform1f(shader_mgr_->GetUniform(prog_surface_, "uWaveAmp"),
                std::max(0.0f, water.wave_amplitude));
    glUniform1f(shader_mgr_->GetUniform(prog_surface_, "uWaveLength"),
                std::max(0.001f, water.wave_length));
    glUniform2f(shader_mgr_->GetUniform(prog_surface_, "uWaveDir"), wnx, wnz);
    glUniform1f(shader_mgr_->GetUniform(prog_surface_, "uWaveTime"), water.wave_time);
    // 波浪距离淡出：远场环间距 ~5.7 m 起、迅速变粗，位移在那里只会变成折角多边形。
    // 取 3λ ~ 7λ（并夹到 [40,150] / [90,210]）：保证淡出结束时仍有 ≥4 个采样/波长。
    {
        const float lam = std::max(0.001f, water.wave_length);
        const float fade_start = std::clamp(3.0f * lam, 40.0f, 150.0f);
        const float fade_end = std::max(fade_start + 1.0f,
                                        std::clamp(7.0f * lam, 90.0f, 210.0f));
        glUniform1f(shader_mgr_->GetUniform(prog_surface_, "uWaveFadeStart"), fade_start);
        glUniform1f(shader_mgr_->GetUniform(prog_surface_, "uWaveFadeEnd"), fade_end);
    }

    glUniform1f(shader_mgr_->GetUniform(prog_surface_, "uEnvMaxLod"), env_max_lod);
    glUniform1f(shader_mgr_->GetUniform(prog_surface_, "uSkyScale"),
                std::max(0.0f, water.sky_scale));
    glUniform1f(shader_mgr_->GetUniform(prog_surface_, "uF0"), water.f0);
    glUniform1f(shader_mgr_->GetUniform(prog_surface_, "uRoughness"), water.roughness);
    glUniform3f(shader_mgr_->GetUniform(prog_surface_, "uAbsorption"),
                std::max(0.0f, water.absorption.x()),
                std::max(0.0f, water.absorption.y()),
                std::max(0.0f, water.absorption.z()));
    glUniform3f(shader_mgr_->GetUniform(prog_surface_, "uScatter"),
                std::max(0.0f, water.scatter.x()),
                std::max(0.0f, water.scatter.y()),
                std::max(0.0f, water.scatter.z()));
    glUniform1f(shader_mgr_->GetUniform(prog_surface_, "uScatterScale"),
                std::max(0.0f, water.scatter_scale));
    glUniform1i(shader_mgr_->GetUniform(prog_surface_, "uCamUnderwater"),
                IsCameraUnderwater(water, cam) ? 1 : 0);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, cap_color_tex_);
    glUniform1i(shader_mgr_->GetUniform(prog_surface_, "uSceneColor"), 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, cap_depth_tex_);
    glUniform1i(shader_mgr_->GetUniform(prog_surface_, "uSceneDepth"), 1);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_CUBE_MAP, env_cube);
    glUniform1i(shader_mgr_->GetUniform(prog_surface_, "uEnvCube"), 2);

    glBindVertexArray(vao_);
    glDrawElements(GL_TRIANGLES, index_count_, GL_UNSIGNED_INT, nullptr);
    glBindVertexArray(0);

    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);

    // 复原 draw buffer（后面还有 3D pass / 雾会写 MRT 双附件）。
    glDrawBuffers(2, bufs);
    glPopAttrib();
}

void WaterRenderer::DrawUnderwaterMedium(const WaterSurface& water, const Camera& cam,
                                         int fbo_w, int fbo_h,
                                         unsigned int fbo_hdr,
                                         unsigned int color_tex, unsigned int scene_depth_tex,
                                         const float view_proj[16]) {
    CHECK(shader_mgr_ != nullptr) << "WaterRenderer::Init 未调用";
    CHECK_GT(fbo_w, 0);
    CHECK_GT(fbo_h, 0);
    CHECK_NE(fbo_hdr, 0u);
    CHECK_NE(color_tex, 0u);
    CHECK_NE(scene_depth_tex, 0u);

    Capture(fbo_w, fbo_h, fbo_hdr);

    float inv_vp[16];
    InvertMat4(view_proj, inv_vp);

    glPushAttrib(GL_ENABLE_BIT | GL_DEPTH_BUFFER_BIT | GL_VIEWPORT_BIT);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_hdr);
    // 只写颜色附件 0：MRT#1（场景深度）本 pass 不动，写它内容会未定义。
    const GLenum one_buf[1] = {GL_COLOR_ATTACHMENT0};
    glDrawBuffers(1, one_buf);
    glViewport(0, 0, fbo_w, fbo_h);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);

    glUseProgram(prog_medium_);
    glUniform2f(shader_mgr_->GetUniform(prog_medium_, "uResolution"),
                static_cast<float>(fbo_w), static_cast<float>(fbo_h));
    glUniformMatrix4fv(shader_mgr_->GetUniform(prog_medium_, "uInvVP"),
                       1, GL_FALSE, inv_vp);
    glUniform3f(shader_mgr_->GetUniform(prog_medium_, "uCameraPos"),
                cam.position.x(), cam.position.y(), cam.position.z());
    glUniform3f(shader_mgr_->GetUniform(prog_medium_, "uAbsorption"),
                std::max(0.0f, water.absorption.x()),
                std::max(0.0f, water.absorption.y()),
                std::max(0.0f, water.absorption.z()));
    glUniform3f(shader_mgr_->GetUniform(prog_medium_, "uScatter"),
                std::max(0.0f, water.scatter.x()),
                std::max(0.0f, water.scatter.y()),
                std::max(0.0f, water.scatter.z()));
    glUniform1f(shader_mgr_->GetUniform(prog_medium_, "uScatterScale"),
                std::max(0.0f, water.scatter_scale));

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, cap_color_tex_);
    glUniform1i(shader_mgr_->GetUniform(prog_medium_, "uSrcColor"), 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, cap_depth_tex_);
    glUniform1i(shader_mgr_->GetUniform(prog_medium_, "uSceneDepth"), 1);

    glBindVertexArray(fs_vao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);

    const GLenum bufs[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
    glDrawBuffers(2, bufs);
    glPopAttrib();
}

}  // namespace jpov
