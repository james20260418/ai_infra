// JPOV HorizonFogRenderer 实现
//
// 全屏三角形 + 解析式雾 FS，用 alpha 混合就地合成到当前绑定的 FBO（不持有自己的 FBO）。
// 详见 horizon_fog_shader.h 的公式说明。

#define GL_GLEXT_PROTOTYPES

#include "tools/jpov/src/horizon_fog/horizon_fog_renderer.h"

#include <cmath>

// GL 头文件必须最先 include（在 MinGW #define 宏替换之前）
#ifdef _WIN32
#include <GL/gl.h>
#else
#include <GL/gl.h>
#include <GL/glext.h>
#endif

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

#include "tools/jpov/src/horizon_fog/horizon_fog_shader.h"

namespace {

// 4x4 列主序求逆（Gauss-Jordan + 部分主元）。把 view_proj 转成逆 VP
//（仰角雾需由屏幕坐标反推世界方向）。不可逆返回 false。
bool Mat4Invert(const float m[16], float out[16]) {
    float a[16];
    for (int i = 0; i < 16; ++i) {
        a[i] = m[i];
    }
    float inv[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    for (int col = 0; col < 4; ++col) {
        int piv = col;
        float best = std::fabs(a[col * 4 + col]);
        for (int r = col + 1; r < 4; ++r) {
            const float v = std::fabs(a[col * 4 + r]);
            if (v > best) {
                best = v;
                piv = r;
            }
        }
        if (best < 1e-12f) {
            return false;
        }
        if (piv != col) {
            for (int c = 0; c < 4; ++c) {
                float t = a[c * 4 + col];
                a[c * 4 + col] = a[c * 4 + piv];
                a[c * 4 + piv] = t;
                t = inv[c * 4 + col];
                inv[c * 4 + col] = inv[c * 4 + piv];
                inv[c * 4 + piv] = t;
            }
        }
        const float d = a[col * 4 + col];
        for (int c = 0; c < 4; ++c) {
            a[c * 4 + col] /= d;
            inv[c * 4 + col] /= d;
        }
        for (int r = 0; r < 4; ++r) {
            if (r == col) {
                continue;
            }
            const float f = a[col * 4 + r];
            for (int c = 0; c < 4; ++c) {
                a[c * 4 + r] -= f * a[c * 4 + col];
                inv[c * 4 + r] -= f * inv[c * 4 + col];
            }
        }
    }
    for (int i = 0; i < 16; ++i) {
        out[i] = inv[i];
    }
    return true;
}

}  // anonymous namespace

namespace jpov {

void HorizonFogRenderer::Draw(const ElevationFogConfig& cfg,
                              unsigned int scene_depth_tex,
                              unsigned int sky_color_tex,
                              const float view_proj[16],
                              const Camera& cam,
                              ShaderManager& shader_mgr) {
    CHECK_NE(scene_depth_tex, 0u) << "HorizonFogRenderer: 缺少场景深度纹理";

    const unsigned int prog = shader_mgr.GetOrCreate(
        "horizon_fog", {kHorizonFogVs, kHorizonFogFs});
    glUseProgram(prog);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, scene_depth_tex);
    glUniform1i(shader_mgr.GetUniform(prog, "uSceneDepthTex"), 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, sky_color_tex);
    glUniform1i(shader_mgr.GetUniform(prog, "uSkyTex"), 1);
    glActiveTexture(GL_TEXTURE0);

    // 收敛色：有天空纹理 且 cfg.use_sky_color ⇒ 取该方向天空色（推荐）；否则用 cfg.color。
    const int use_sky = (cfg.use_sky_color && sky_color_tex != 0) ? 1 : 0;
    glUniform1i(shader_mgr.GetUniform(prog, "uUseSkyColor"), use_sky);

    float inv_vp[16];
    CHECK(Mat4Invert(view_proj, inv_vp))
        << "HorizonFogRenderer: 相机矩阵不可逆（near/far 非法或退化）";
    glUniformMatrix4fv(shader_mgr.GetUniform(prog, "uInvVP"), 1, GL_FALSE, inv_vp);
    glUniform3f(shader_mgr.GetUniform(prog, "uCamPos"), cam.position.x(),
                cam.position.y(), cam.position.z());

    // 参数夹断（避免退化：full>start、outer>inner），供交互滑条安全调节。
    const float start = std::max(cfg.start_distance, 0.0f);
    const float full = std::max(cfg.full_distance, start + 1.0f);
    const float inner = std::max(cfg.elev_inner_deg, 0.0f);
    const float outer = std::max(cfg.elev_outer_deg, inner + 0.01f);
    constexpr float kDeg2Rad = 3.14159265358979323846f / 180.0f;
    glUniform1f(shader_mgr.GetUniform(prog, "uStart"), start);
    glUniform1f(shader_mgr.GetUniform(prog, "uFull"), full);
    glUniform1f(shader_mgr.GetUniform(prog, "uElevSinInner"), std::sin(inner * kDeg2Rad));
    glUniform1f(shader_mgr.GetUniform(prog, "uElevSinOuter"), std::sin(outer * kDeg2Rad));
    glUniform1f(shader_mgr.GetUniform(prog, "uDensity"), std::max(cfg.density, 0.0f));
    glUniform3f(shader_mgr.GetUniform(prog, "uFogColor"), cfg.color.r, cfg.color.g,
                cfg.color.b);

    // 就地 alpha 混合：out = fog_color·α + dst·(1−α)，dst = 当前 FBO 里的场景颜色。
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glDrawArrays(GL_TRIANGLES, 0, 3);   // 全屏三角形（无 VAO/VBO，用 gl_VertexID）

    // 复原：调用方（Renderer）随后要做 resolve / highlight / bloom，均自设状态；
    // 这里统一关掉本 pass 打开的混合/深度/剔除，并解绑纹理。
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glUseProgram(0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

}  // namespace jpov
