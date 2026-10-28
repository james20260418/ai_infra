// JPOV WaterSkinRenderer 实现。见 water_skin_renderer.h / water_skin_shader.h。

#define GL_GLEXT_PROTOTYPES

#include "tools/jpov/src/water/water_skin_renderer.h"

#include <GL/gl.h>
#include <GL/glext.h>

#include <algorithm>
#include <cmath>
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
// 天空探针（镜面天光）。pass-local，与 scene depth 不冲突。
constexpr int kSkyCubeUnit = 1;
// 波浪法线 flipbook 图集（模式 4）。pass-local。
constexpr int kWaveTileUnit = 2;
// 平面反射的倒影颜色（实验）。pass-local。
constexpr int kReflTexUnit = 3;

}  // namespace

void WaterSkinRenderer::Init() {
    CHECK_EQ(vao_, 0u) << "WaterSkinRenderer::Init 重复调用";

    // 全屏三角形：顶点坐标由 gl_VertexID 在 VS 里生成，**不需要顶点缓冲**，
    // 只要一个空 VAO（core profile 不允许 client-side 数组，也不允许无 VAO 绘制）。
    // 位置由"逐像素视线 ∩ 水平平面"算出来 ⇒ 水面**没有几何边界**（覆盖到地平线）。
    glGenVertexArrays(1, &vao_);
    glBindVertexArray(0);

    // 1×1 黑纹理（见 refl_dummy_tex_ 的说明）。
    const unsigned char kDummy[4] = {0, 0, 0, 255};
    glGenTextures(1, &refl_dummy_tex_);
    glBindTexture(GL_TEXTURE_2D, refl_dummy_tex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, kDummy);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);

    // 波浪法线 flipbook（256² × 8 帧 × 16 m tile）**不在这里烘**：
    // 256²/16 m = 6.25 cm/texel 够表现厘米级涟漪，但烘焙 ~60-90 ms + 4.2 MB ——
    // 不用水面的 app 一分钱都不该花（Builder 在首次 Draw 里做，见下）。
    //
    // ⚠️ 另一个更硬的理由：这个烘焙**会改变其它 pass 的逐像素结果**（实测同一份
    //    fillrect/roundrect 2D gold 出图差 ±1/255、5.5k 像素）——在 Init 里烘
    //    等于让"从来没画过水面"的场景也吃到这个副作用。懒烘之后 2D gold 逐字节复原。
}

void WaterSkinRenderer::Finalize() {
    ReleaseWaterNormalTile(&wave_tile_);
    if (refl_dummy_tex_ != 0) {
        glDeleteTextures(1, &refl_dummy_tex_);
        refl_dummy_tex_ = 0;
    }
    if (vao_ != 0) {
        glDeleteVertexArrays(1, &vao_);
        vao_ = 0;
    }
}

void WaterSkinRenderer::Draw(const WaterSkinParams& params, const Camera& cam,
                             unsigned int scene_depth_tex, unsigned int sky_env_cube,
                             const DirectionalLight* sun, int width, int height,
                             ShaderManager& shader_mgr, const WaterSkinReflection& refl) {
    CHECK_NE(scene_depth_tex, 0u) << "WaterSkinRenderer: 缺少场景深度纹理";
    CHECK_NE(vao_, 0u) << "WaterSkinRenderer: 未 Init()";
    CHECK_GT(width, 0);
    CHECK_GT(height, 0);

    // 波浪法线 flipbook：首次真正画水面时才烘（见 Init 的说明）。
    if (wave_tile_.tex == 0) {
        wave_tile_ = BuildWaterNormalTile(/*size=*/256, /*frames=*/8, /*tile_m=*/16.0f);
    }

    const unsigned int prog = shader_mgr.GetOrCreate("water_skin",
                                                     {kWaterSkinVs, kWaterSkinFs});
    glUseProgram(prog);

    // 无限平面不需要 VP 矩阵：射线由相机基 + NDC 直接算（见 FS）。
    glUniform1f(shader_mgr.GetUniform(prog, "uLevelY"), params.level_y);
    glUniform2f(shader_mgr.GetUniform(prog, "uViewport"), static_cast<float>(width),
                static_cast<float>(height));
    glUniform1f(shader_mgr.GetUniform(prog, "uNear"), cam.near);
    glUniform1f(shader_mgr.GetUniform(prog, "uFar"), cam.far);
    glUniform3f(shader_mgr.GetUniform(prog, "uCamPos"), cam.position.x(),
                cam.position.y(), cam.position.z());
    {
        // 相机基（与 Primitives3DRenderer::BuildMVP 的 lookAt 同一套约定）：
        //   fwd = normalize(target − position)，right = normalize(cross(fwd, up))，
        //   up' = cross(right, fwd)。
        // 逐像素方向 = fwd + (ndc.x·tanHalf·aspect)·right + (ndc.y·tanHalf)·up'。
        const Vec3f fwd_raw = Vec3f{cam.target.x() - cam.position.x(),
                                    cam.target.y() - cam.position.y(),
                                    cam.target.z() - cam.position.z()};
        const float fwd_len = fwd_raw.Norm();
        CHECK_GT(fwd_len, 1.0e-6f) << "WaterSkinRenderer: 相机 target 与 position 重合";
        const Vec3f fwd{fwd_raw.x() / fwd_len, fwd_raw.y() / fwd_len, fwd_raw.z() / fwd_len};
        Vec3f right{fwd.y() * cam.up.z() - fwd.z() * cam.up.y(),
                    fwd.z() * cam.up.x() - fwd.x() * cam.up.z(),
                    fwd.x() * cam.up.y() - fwd.y() * cam.up.x()};
        const float right_len = right.Norm();
        CHECK_GT(right_len, 1.0e-6f) << "WaterSkinRenderer: 相机 up 与视线平行";
        right = Vec3f{right.x() / right_len, right.y() / right_len, right.z() / right_len};
        // up' = right × fwd（**不是** fwd × right —— 那个顺序得到的是 −up，
        // 会让整张水面的上下方向翻转：屏幕上半（应朝天）被当成朝下求交）。
        const Vec3f up{right.y() * fwd.z() - right.z() * fwd.y(),
                       right.z() * fwd.x() - right.x() * fwd.z(),
                       right.x() * fwd.y() - right.y() * fwd.x()};
        glUniform3f(shader_mgr.GetUniform(prog, "uCamFwd"), fwd.x(), fwd.y(), fwd.z());
        glUniform3f(shader_mgr.GetUniform(prog, "uCamRight"), right.x(), right.y(), right.z());
        glUniform3f(shader_mgr.GetUniform(prog, "uCamUp"), up.x(), up.y(), up.z());
        constexpr float kDeg2RadCam = 3.14159265358979323846f / 180.0f;
        glUniform1f(shader_mgr.GetUniform(prog, "uTanHalfFov"),
                    std::tan(0.5f * cam.fov * kDeg2RadCam));
        glUniform1f(shader_mgr.GetUniform(prog, "uAspect"),
                    static_cast<float>(width) / static_cast<float>(height));
    }
    glUniform3f(shader_mgr.GetUniform(prog, "uBodyColor"), params.body_color.r,
                params.body_color.g, params.body_color.b);
    glUniform1f(shader_mgr.GetUniform(prog, "uBodyAlpha"), params.body_alpha);
    glUniform3f(shader_mgr.GetUniform(prog, "uRimColor"), params.rim_color.r,
                params.rim_color.g, params.rim_color.b);
    // 水下视角（Snell 窗 / 全内反射）：临界角外的
    // 全内反射看到的是水下环境，用地色代表。
    glUniform3f(shader_mgr.GetUniform(prog, "uTirColor"), params.tir_color.r,
                params.tir_color.g, params.tir_color.b);
    glUniform1f(shader_mgr.GetUniform(prog, "uIor"), params.ior);
    glUniform1f(shader_mgr.GetUniform(prog, "uMinTransmission"),
                std::max(0.0f, std::min(params.min_transmission, 1.0f)));

    // ── 水上三项 ──
    glUniform1i(shader_mgr.GetUniform(prog, "uUseSkyRefl"),
                params.use_sky_reflection ? 1 : 0);
    glUniform1i(shader_mgr.GetUniform(prog, "uUseTransmit"),
                params.use_transmission ? 1 : 0);
    glUniform1i(shader_mgr.GetUniform(prog, "uUseSun"),
                (params.use_sun_glint && sun != nullptr) ? 1 : 0);
    glUniform1f(shader_mgr.GetUniform(prog, "uSkyScale"), params.sky_scale);
    glUniform3f(shader_mgr.GetUniform(prog, "uSunGlowColor"), params.sun_glow_color.r,
                params.sun_glow_color.g, params.sun_glow_color.b);
    glUniform1f(shader_mgr.GetUniform(prog, "uSunGlowSigma"),
                std::max(params.sun_glow_sigma_rad, 1.0e-5f));
    glUniform1f(shader_mgr.GetUniform(prog, "uSunDiscRadius"),
                std::max(params.sun_disc_radius_rad, 0.0f));
    glUniform1f(shader_mgr.GetUniform(prog, "uHaloSize"),
                std::max(params.sun_halo_size, 0.05f));
    glUniform1f(shader_mgr.GetUniform(prog, "uHaloAspect"),
                std::max(params.sun_halo_aspect, 0.05f));
    glUniform3f(shader_mgr.GetUniform(prog, "uSunHaloDir"), params.sun_halo_dir.x(),
                params.sun_halo_dir.y(), params.sun_halo_dir.z());
    glUniform1f(shader_mgr.GetUniform(prog, "uLeanGain"),
                params.use_lean_glint ? std::max(params.lean_gain, 0.0f) : 0.0f);
    if (sun != nullptr) {
        // DirectionalLight::direction 是**光传播方向**（从太阳射向场景）⇒ 指向太阳要取反。
        const Vec3f to_sun = Vec3f{-sun->direction.x(), -sun->direction.y(),
                                   -sun->direction.z()};
        const float len = to_sun.Norm();
        const Vec3f dir = (len > 1e-6f) ? Vec3f{to_sun.x() / len, to_sun.y() / len,
                                                 to_sun.z() / len}
                                        : Vec3f{0.0f, 1.0f, 0.0f};
        glUniform3f(shader_mgr.GetUniform(prog, "uSunDir"), dir.x(), dir.y(), dir.z());
        glUniform3f(shader_mgr.GetUniform(prog, "uSunColor"),
                    sun->color.r * sun->intensity, sun->color.g * sun->intensity,
                    sun->color.b * sun->intensity);
    } else {
        glUniform3f(shader_mgr.GetUniform(prog, "uSunDir"), 0.0f, 1.0f, 0.0f);
        glUniform3f(shader_mgr.GetUniform(prog, "uSunColor"), 0.0f, 0.0f, 0.0f);
    }
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_CUBE_MAP, sky_env_cube);
    glUniform1i(shader_mgr.GetUniform(prog, "uEnvCube"), kSkyCubeUnit);
    glUniform1i(shader_mgr.GetUniform(prog, "uHasEnvCube"), sky_env_cube != 0 ? 1 : 0);
    glActiveTexture(GL_TEXTURE0);
    constexpr float kDeg2Rad = 3.14159265358979323846f / 180.0f;
    // 波浪（只扰法线）：相位原样传，斜率/方向由度换弧度，波长夹到 >0。
    glUniform1f(shader_mgr.GetUniform(prog, "uWavePhase"), params.wave_phase);
    glUniform1f(shader_mgr.GetUniform(prog, "uWaveSlopeRad"),
                std::max(params.wave_slope_deg, 0.0f) * kDeg2Rad);
    glUniform1f(shader_mgr.GetUniform(prog, "uWaveLengthM"),
                std::max(params.wave_length_m, 0.01f));
    glUniform1f(shader_mgr.GetUniform(prog, "uWaveDirRad"),
                params.wave_dir_deg * kDeg2Rad);
    glUniform1i(shader_mgr.GetUniform(prog, "uWaveFilterMode"),
                std::max(0, std::min(params.wave_filter_mode, 4)));
    // 模式 4：把烘好的法线图集挂上去（单元 2，pass-local）。
    glUniform1f(shader_mgr.GetUniform(prog, "uWaveTileM"),
                std::max(params.wave_tile_m, 0.05f));
    glUniform1i(shader_mgr.GetUniform(prog, "uWaveTileFrames"), wave_tile_.frames);
    glUniform1i(shader_mgr.GetUniform(prog, "uHasWaveTile"), wave_tile_.tex != 0 ? 1 : 0);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D_ARRAY, wave_tile_.tex);
    glUniform1i(shader_mgr.GetUniform(prog, "uWaveTile"), kWaveTileUnit);
    glActiveTexture(GL_TEXTURE0);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, scene_depth_tex);
    glUniform1i(shader_mgr.GetUniform(prog, "uSceneDepth"), kSceneDepthUnit);

    // ── 平面反射（实验）：倒影贴图 + 镜像相机 VP ──
    glUniform1i(shader_mgr.GetUniform(prog, "uUseReflTex"),
                (refl.texture != 0) ? 1 : 0);
    glUniformMatrix4fv(shader_mgr.GetUniform(prog, "uReflVP"), 1, GL_FALSE, refl.vp);
    glUniform1f(shader_mgr.GetUniform(prog, "uReflDistort"),
                std::max(params.refl_distort, 0.0f));
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, (refl.texture != 0) ? refl.texture : refl_dummy_tex_);
    glUniform1i(shader_mgr.GetUniform(prog, "uReflTex"), kReflTexUnit);


    // 简易透射 = 固定管线 alpha 混合，dst 就是帧缓冲里已有的场景颜色。
    // 深度测试关闭：HDR 目标没有深度附件，FS 里手动比 MRT#1 场景深度。
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);   // 双面：相机沉到水下时还能看见水面
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);   // 全屏三角形（顶点由 gl_VertexID 生成）
    glBindVertexArray(0);

    // 复原成「3D pass 结束后的状态」：调用方随后是 highlight / bloom / tone map，自设状态。
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glUseProgram(0);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

}  // namespace jpov
