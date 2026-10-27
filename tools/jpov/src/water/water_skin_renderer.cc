// JPOV WaterSkinRenderer 实现。见 water_skin_renderer.h / water_skin_shader.h。

#define GL_GLEXT_PROTOTYPES

#include "tools/jpov/src/water/water_skin_renderer.h"

#include <GL/gl.h>
#include <GL/glext.h>

#include <algorithm>
#ifdef _WIN32
#ifndef GLOG_NO_ABBREVIATED_SEVERITIES
#define GLOG_NO_ABBREVIATED_SEVERITIES
#endif
#include "third_party/gl_loader-mingw/gl_loader.h"
// windef.h 把 near / far 定义成**空宏**，会让 `cam.near` / `cam.far` 展开成 `cam.`
// （同 strip / particle / horizon_fog 的处理）。
#undef near
#undef far
#endif

#include <glog/logging.h>

#include "tools/jpov/src/water/water_skin_shader.h"

namespace jpov {

namespace {

// 本 pass 是独立 pass（不透明 3D + 粒子/雾/条带之后），与 strip pass 同样的划分：
// 单元 0 = 场景深度。pass-local，不占 texture_units.h 的跨 pass 区间。
constexpr int kSceneDepthUnit = 0;
// 天空探针 cubemap（水下折射采样）。pass-local，与 scene depth 不冲突。
constexpr int kSkyCubeUnit = 1;

}  // namespace

void WaterSkinRenderer::Init() {
    CHECK_EQ(vao_, 0u) << "WaterSkinRenderer::Init 重复调用";

    // 局部坐标的正方形（x,z ∈ ±1，y = 0）：世界尺寸与高度由 uniform 展开，
    // 所以改水面大小/水位不需要重传顶点缓冲。
    constexpr float kVerts[] = {
        -1.0f, 0.0f, -1.0f,
         1.0f, 0.0f, -1.0f,
        -1.0f, 0.0f,  1.0f,
         1.0f, 0.0f,  1.0f,
    };
    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kVerts), kVerts, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), nullptr);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void WaterSkinRenderer::Finalize() {
    if (vbo_ != 0) {
        glDeleteBuffers(1, &vbo_);
        vbo_ = 0;
    }
    if (vao_ != 0) {
        glDeleteVertexArrays(1, &vao_);
        vao_ = 0;
    }
}

void WaterSkinRenderer::Draw(const WaterSkinParams& params, const Camera& cam,
                             const float view_proj[16], unsigned int scene_depth_tex,
                             unsigned int sky_env_cube, int width, int height,
                             ShaderManager& shader_mgr) {
    CHECK_GT(params.half_extent_m, 0.0f) << "WaterSkinRenderer: half_extent_m 必须 > 0";
    CHECK_NE(scene_depth_tex, 0u) << "WaterSkinRenderer: 缺少场景深度纹理";
    CHECK_NE(vao_, 0u) << "WaterSkinRenderer: 未 Init()";
    CHECK_GT(width, 0);
    CHECK_GT(height, 0);

    const unsigned int prog = shader_mgr.GetOrCreate("water_skin",
                                                     {kWaterSkinVs, kWaterSkinFs});
    glUseProgram(prog);

    glUniformMatrix4fv(shader_mgr.GetUniform(prog, "uVP"), 1, GL_FALSE, view_proj);
    glUniform1f(shader_mgr.GetUniform(prog, "uHalfExtent"), params.half_extent_m);
    // uLevelY 由 VS（展开 quad 高度）与 FS（判相机在水上/水下）**共用**：同一个 program
    // 里同名 uniform 只需上传一次，两个阶段都能读到。
    glUniform1f(shader_mgr.GetUniform(prog, "uLevelY"), params.level_y);
    glUniform2f(shader_mgr.GetUniform(prog, "uViewport"), static_cast<float>(width),
                static_cast<float>(height));
    glUniform1f(shader_mgr.GetUniform(prog, "uNear"), cam.near);
    glUniform1f(shader_mgr.GetUniform(prog, "uFar"), cam.far);
    glUniform3f(shader_mgr.GetUniform(prog, "uCamPos"), cam.position.x(),
                cam.position.y(), cam.position.z());
    glUniform3f(shader_mgr.GetUniform(prog, "uBodyColor"), params.body_color.r,
                params.body_color.g, params.body_color.b);
    glUniform1f(shader_mgr.GetUniform(prog, "uBodyAlpha"), params.body_alpha);
    glUniform3f(shader_mgr.GetUniform(prog, "uRimColor"), params.rim_color.r,
                params.rim_color.g, params.rim_color.b);
    glUniform1f(shader_mgr.GetUniform(prog, "uF0"), params.fresnel_f0);
    glUniform1f(shader_mgr.GetUniform(prog, "uPower"), params.fresnel_power);
    // 水下视角（Snell 窗 / 全内反射）：临界角外的
    // 全内反射看到的是水下环境，用地色代表。
    glUniform3f(shader_mgr.GetUniform(prog, "uTirColor"), params.tir_color.r,
                params.tir_color.g, params.tir_color.b);
    glUniform1f(shader_mgr.GetUniform(prog, "uIor"), params.ior);
    glUniform1f(shader_mgr.GetUniform(prog, "uMinTransmission"),
                std::max(0.0f, std::min(params.min_transmission, 1.0f)));
    constexpr float kDeg2Rad = 3.14159265358979323846f / 180.0f;
    // 波浪（只扰法线）：相位原样传，斜率/方向由度换弧度，波长夹到 >0。
    glUniform1f(shader_mgr.GetUniform(prog, "uWavePhase"), params.wave_phase);
    glUniform1f(shader_mgr.GetUniform(prog, "uWaveSlopeRad"),
                std::max(params.wave_slope_deg, 0.0f) * kDeg2Rad);
    glUniform1f(shader_mgr.GetUniform(prog, "uWaveLengthM"),
                std::max(params.wave_length_m, 0.01f));
    glUniform1f(shader_mgr.GetUniform(prog, "uWaveDirRad"),
                params.wave_dir_deg * kDeg2Rad);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, scene_depth_tex);
    glUniform1i(shader_mgr.GetUniform(prog, "uSceneDepth"), kSceneDepthUnit);

    // 天空探针（水下折射采样用）。没有探针时置 0 ⇒ FS 走退化分支（不做折射）。
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_CUBE_MAP, sky_env_cube);
    glUniform1i(shader_mgr.GetUniform(prog, "uEnvCube"), kSkyCubeUnit);
    glUniform1i(shader_mgr.GetUniform(prog, "uHasEnvCube"), sky_env_cube != 0 ? 1 : 0);
    glActiveTexture(GL_TEXTURE0);

    // 简易透射 = 固定管线 alpha 混合，dst 就是帧缓冲里已有的场景颜色。
    // 深度测试关闭：HDR 目标没有深度附件，FS 里手动比 MRT#1 场景深度。
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);   // 双面：相机沉到水下时还能看见水面
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);

    // 复原成「3D pass 结束后的状态」：调用方随后是 highlight / bloom / tone map，自设状态。
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glUseProgram(0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

}  // namespace jpov
