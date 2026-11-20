// JPOV WaterSkinRenderer 实现（只产层，不合成 —— 见头文件顶部）
//
// GL 状态契约：
//   · Draw() 只绑自己的 WBO FBO（BindForDraw/EndDraw 成对），退出时 FBO 已解绑；
//   · 纹理单元 0（场景深度）、1（天光探针）会被改绑 —— 调用方后续 pass 自己重绑；
//   · 不改 blend / depth-test 的 enable 位（WBO 的 BindForDraw 自己设好并在 EndDraw 复原）。

#define GL_GLEXT_PROTOTYPES

#include "tools/jpov/src/water/water_skin_renderer.h"

#include <GL/gl.h>
#include <GL/glext.h>

#ifdef _WIN32
#ifndef GLOG_NO_ABBREVIATED_SEVERITIES
#define GLOG_NO_ABBREVIATED_SEVERITIES
#endif
#include "third_party/gl_loader-mingw/gl_loader.h"
#endif

#include <algorithm>
#include <cmath>
#include <string>

#include <glog/logging.h>

#ifdef _WIN32
// ⚠️ MinGW 的 <GL/gl.h> 会拉进 windows.h，而它把 `near` / `far` 定义成**空宏** ——
//    那会把 `cam.near` / `cam.far` 展开成 `cam.` 直接编译错误（本项目已知坑）。
#undef near
#undef far
#endif

#include "tools/jpov/src/water/water_medium_shader.h"   // kWaterMediumGlsl（MediumAt）
#include "tools/jpov/src/water/water_skin_shader.h"
#include "tools/jpov/src/water/water_wave_math.h"      // WaterWaves / kWaveGravity
#include "tools/jpov/src/water/water_wave_shader.h"    // kWaterWaveGlsl

namespace jpov {
namespace {

using water::WaterMediumSample;

Vec3f SubVec(const Vec3f& a, const Vec3f& b) {
    return Vec3f(a.x() - b.x(), a.y() - b.y(), a.z() - b.z());
}

Vec3f CrossVec(const Vec3f& a, const Vec3f& b) {
    return Vec3f(a.y() * b.z() - a.z() * b.y(),
                 a.z() * b.x() - a.x() * b.z(),
                 a.x() * b.y() - a.y() * b.x());
}

Vec3f NormalizeOr(const Vec3f& v, const Vec3f& fallback) {
    const float n = v.Norm();
    if (n < 1.0e-8f) {
        return fallback;
    }
    return Vec3f(v.x() / n, v.y() / n, v.z() / n);
}

}  // namespace

WaterSkinRenderer::WaterSkinRenderer() = default;
WaterSkinRenderer::~WaterSkinRenderer() = default;

void WaterSkinRenderer::Init(ShaderManager* shader_mgr) {
    CHECK(shader_mgr != nullptr) << "WaterSkinRenderer::Init: shader_mgr 不能为空";
    CHECK(!initialized_) << "WaterSkinRenderer::Init 重复调用";
    shader_mgr_ = shader_mgr;
    // 介质片段（MediumAt + uWater* uniform）拼在本 pass 的 FS 前面：
    // 水下支路要用它取「深海积分反射」 S_P(∞)，物理只写一份。
    // GGX 片段（平光镜面项）同理 —— 与 PBR 共用同一份 D/G，不在水皮里再拄一遍。
    std::string fs = "#version 330 core\n";
    fs += water::kWaterMediumGlsl;
    fs += water::kWaterWaveGlsl;
    fs += lighting::kGgxSpecularGlsl;
    fs += water::kWaterSkinFs;
    prog_draw_ = shader_mgr_->GetOrCreate("water_skin_draw",
                                          {water::kWaterSkinVs, fs.c_str()});
    glGenVertexArrays(1, &vao_);
    CHECK_NE(vao_, 0u) << "WaterSkinRenderer::Init: glGenVertexArrays 失败";
    initialized_ = true;
}

void WaterSkinRenderer::Finalize() {
    wbo_.Destroy();
    if (vao_ != 0) {
        glDeleteVertexArrays(1, &vao_);
        vao_ = 0;
    }
    has_content_ = false;
    initialized_ = false;
}

void WaterSkinRenderer::EnsureTarget(int w, int h) {
    // 1× 层、不含加法集；**申请 depth 附件**（att3）—— 这是「水皮是一层」的全部凭据。
    // 线性过滤关掉：1× 上插值没有意义，且 att3 是标量代表深度。
    wbo_.Ensure({w, h, /*with_additive=*/false, /*with_depth=*/true,
                 /*linear_filter=*/false});
}

void WaterSkinRenderer::Begin(int full_w, int full_h) {
    CHECK(initialized_) << "WaterSkinRenderer::Begin: 未 Init";
    CHECK_GT(full_w, 0) << "WaterSkinRenderer::Begin: full_w 必须 > 0";
    CHECK_GT(full_h, 0) << "WaterSkinRenderer::Begin: full_h 必须 > 0";
    EnsureTarget(full_w, full_h);
    wbo_.Clear();
    has_content_ = false;
}

void WaterSkinRenderer::UploadCommonUniforms(unsigned int prog, const Inputs& in) {
    const Camera& cam = in.camera;
    const Vec3f fwd = NormalizeOr(SubVec(cam.target, cam.position), Vec3f(0.0f, 0.0f, -1.0f));
    const Vec3f side = NormalizeOr(CrossVec(fwd, cam.up), Vec3f(1.0f, 0.0f, 0.0f));
    const Vec3f upv = CrossVec(side, fwd);
    const float tan_half_fov = std::tan(0.5f * cam.fov * 3.14159265358979323846f / 180.0f);
    const float aspect = static_cast<float>(in.full_w) / static_cast<float>(in.full_h);

    glUniform2f(shader_mgr_->GetUniform(prog, "uViewport"),
                static_cast<float>(in.full_w), static_cast<float>(in.full_h));
    glUniform3f(shader_mgr_->GetUniform(prog, "uCamPos"),
                cam.position.x(), cam.position.y(), cam.position.z());
    glUniform3f(shader_mgr_->GetUniform(prog, "uCamFwd"), fwd.x(), fwd.y(), fwd.z());
    glUniform3f(shader_mgr_->GetUniform(prog, "uCamRight"), side.x(), side.y(), side.z());
    glUniform3f(shader_mgr_->GetUniform(prog, "uCamUp"), upv.x(), upv.y(), upv.z());
    glUniform1f(shader_mgr_->GetUniform(prog, "uTanHalfFov"), tan_half_fov);
    glUniform1f(shader_mgr_->GetUniform(prog, "uAspect"), aspect);
    glUniform1f(shader_mgr_->GetUniform(prog, "uNear"), cam.near);
    glUniform1f(shader_mgr_->GetUniform(prog, "uFar"), cam.far);
    glUniform1f(shader_mgr_->GetUniform(prog, "uEnvScale"), in.env_scale);
    glUniform1f(shader_mgr_->GetUniform(prog, "uHorizonClampSin"), water::kHorizonClampSin);
    glUniform1f(shader_mgr_->GetUniform(prog, "uDepthBiasM"), water::kDepthBiasM);

    // ── 介质 uniform（水下支路的 S_P(∞) 与水平面/折射率）──
    // 水平面高度与 ior 总是上传（即使没有介质也要能画界面）；
    // 其余无介质时置零，避免上一帧残留。
    glUniform1f(shader_mgr_->GetUniform(prog, "uWaterLevelY"), in.level_y);
    glUniform1f(shader_mgr_->GetUniform(prog, "uWaterIor"), in.ior);
    const bool has_medium = (in.medium != nullptr);
    glUniform1i(shader_mgr_->GetUniform(prog, "uHasWater"), has_medium ? 1 : 0);
    if (has_medium) {
        const WaterMediumSample& m = *in.medium;
        glUniform3f(shader_mgr_->GetUniform(prog, "uWaterSigmaA"),
                    m.sigma_a.x(), m.sigma_a.y(), m.sigma_a.z());
        glUniform3f(shader_mgr_->GetUniform(prog, "uWaterSigmaS"),
                    m.sigma_s.x(), m.sigma_s.y(), m.sigma_s.z());
        glUniform1f(shader_mgr_->GetUniform(prog, "uWaterPhaseG"), m.phase_g);
        glUniform3f(shader_mgr_->GetUniform(prog, "uWaterAmbient"),
                    m.ambient.x(), m.ambient.y(), m.ambient.z());
        glUniform3f(shader_mgr_->GetUniform(prog, "uWaterSunColor"),
                    m.sun_color.x(), m.sun_color.y(), m.sun_color.z());
        glUniform3f(shader_mgr_->GetUniform(prog, "uWaterSunToward"),
                    m.sun_toward.x(), m.sun_toward.y(), m.sun_toward.z());
        glUniform1i(shader_mgr_->GetUniform(prog, "uWaterHasSun"), m.has_sun ? 1 : 0);
    } else {
        glUniform3f(shader_mgr_->GetUniform(prog, "uWaterSigmaA"), 0.0f, 0.0f, 0.0f);
        glUniform3f(shader_mgr_->GetUniform(prog, "uWaterSigmaS"), 0.0f, 0.0f, 0.0f);
        glUniform1f(shader_mgr_->GetUniform(prog, "uWaterPhaseG"), 0.0f);
        glUniform3f(shader_mgr_->GetUniform(prog, "uWaterAmbient"), 0.0f, 0.0f, 0.0f);
        glUniform3f(shader_mgr_->GetUniform(prog, "uWaterSunColor"), 0.0f, 0.0f, 0.0f);
        glUniform3f(shader_mgr_->GetUniform(prog, "uWaterSunToward"), 0.0f, 1.0f, 0.0f);
        glUniform1i(shader_mgr_->GetUniform(prog, "uWaterHasSun"), 0);
    }

    // ── 太阳的像（解析 GGX，盘状光源）──
    // 与介质那份 sun 分开传：介质可以关，水面依然该有日盘像。
    // 无太阳时置零，避免上一帧残留。
    glUniform1i(shader_mgr_->GetUniform(prog, "uHasSun"), in.has_sun ? 1 : 0);
    glUniform3f(shader_mgr_->GetUniform(prog, "uSunToward"),
                in.sun_toward.x(), in.sun_toward.y(), in.sun_toward.z());
    glUniform3f(shader_mgr_->GetUniform(prog, "uSunIrradiance"),
                in.has_sun ? in.sun_irradiance.x() : 0.0f,
                in.has_sun ? in.sun_irradiance.y() : 0.0f,
                in.has_sun ? in.sun_irradiance.z() : 0.0f);
    glUniform1f(shader_mgr_->GetUniform(prog, "uSunAngularRadius"), in.sun_angular_radius);
    glUniform1f(shader_mgr_->GetUniform(prog, "uSunFootprint"), in.sun_footprint);

    UploadWaveUniforms(prog, in);
}

void WaterSkinRenderer::UploadWaveUniforms(unsigned int prog, const Inputs& in) {
    const WaterWaves* w = in.waves;
    const bool on = (w != nullptr) && w->enabled && w->num_waves > 0;
    const int count = on ? std::min(w->num_waves, WaterWaves::kMaxWaves) : 0;
    glUniform1i(shader_mgr_->GetUniform(prog, "uWaveCount"), count);

    // 波表先算好 k / ω 再上传：GPU 不重算色散（与 CPU 参考同一口径）。
    // ⚠️ 逐个 glUniform4f/2f 上传，不用 glUniform4fv/2fv —— MinGW 的 gl_loader 只导出了
    //    标量版本（数组版本 undefined reference，Windows 交叉编译直接链失败）。
    //    GL 保证数组 uniform 的 location 连续 ⇒ loc+i 就是第 i 个元素。
    const int wave_l_loc = shader_mgr_->GetUniform(prog, "uWaveL[0]");
    const int wave_dir_loc = shader_mgr_->GetUniform(prog, "uWaveDir[0]");
    const float k_two_pi = 6.28318530717958647692f;
    for (int i = 0; i < count; ++i) {
        const WaterWave& wv = w->waves[i];
        CHECK_GT(wv.wavelength_m, 0.0f) << "WaterWave.wavelength_m 必须 > 0";
        CHECK_GE(wv.amplitude_m, 0.0f) << "WaterWave.amplitude_m 必须 >= 0";
        const float k = k_two_pi / wv.wavelength_m;
        const float omega = water::DispersionOmega(k, in.wave_depth_m);
        glUniform4f(wave_l_loc + i, k, omega, wv.amplitude_m, wv.phase_rad);
        const float dl = std::sqrt(wv.dir_x * wv.dir_x + wv.dir_z * wv.dir_z);
        const bool has_dir = dl > 1.0e-6f;
        glUniform2f(wave_dir_loc + i,
                    has_dir ? (wv.dir_x / dl) : 1.0f,
                    has_dir ? (wv.dir_z / dl) : 0.0f);
    }

    glUniform1f(shader_mgr_->GetUniform(prog, "uWaveTime"), on ? in.wave_time_s : 0.0f);

    // ── 域扭曲（domain warp）：4 条低频载波 → (k, dir_x, dir_z, phase) ──
    const float warp_amp = on ? w->warp_amplitude_m : 0.0f;
    glUniform1f(shader_mgr_->GetUniform(prog, "uWaveWarpAmp"), warp_amp > 0.0f ? warp_amp : 0.0f);
    const int warp_loc = shader_mgr_->GetUniform(prog, "uWaveWarp[0]");
    for (int j = 0; j < WaterWaves::kWarpCarriers; ++j) {
        if (warp_amp <= 0.0f) {
            glUniform4f(warp_loc + j, 1.0f, 1.0f, 0.0f, 0.0f);
            continue;
        }
        CHECK_GT(w->warp_wavelength_m[j], 0.0f)
            << "WaterWaves.warp_wavelength_m 必须 > 0（关掉请用 warp_amplitude_m = 0）";
        const float k = k_two_pi / w->warp_wavelength_m[j];
        float dx = w->warp_dir_x[j];
        float dz = w->warp_dir_z[j];
        const float dl = std::sqrt(dx * dx + dz * dz);
        if (dl > 1.0e-6f) {
            dx /= dl;
            dz /= dl;
        } else {
            dx = 1.0f;
            dz = 0.0f;
        }
        glUniform4f(warp_loc + j, k, dx, dz, w->warp_phase_rad[j]);
    }
    glUniform1i(shader_mgr_->GetUniform(prog, "uWaveStokes2"),
                (on && w->stokes_second_order) ? 1 : 0);

    // ── 细节短波：只进法线（不进几何/求交）──
    const int detail_count =
        on ? std::clamp(w->num_detail_waves, 0, WaterWaves::kMaxDetailWaves) : 0;
    glUniform1i(shader_mgr_->GetUniform(prog, "uWaveDetailCount"), detail_count);
    const int detail_l_loc = shader_mgr_->GetUniform(prog, "uWaveDetailL[0]");
    const int detail_dir_loc = shader_mgr_->GetUniform(prog, "uWaveDetailDir[0]");
    for (int i = 0; i < detail_count; ++i) {
        const WaterWave& dv = w->detail_waves[i];
        CHECK_GT(dv.wavelength_m, 0.0f) << "detail_waves.wavelength_m 必须 > 0";
        CHECK_GE(dv.amplitude_m, 0.0f) << "detail_waves.amplitude_m 必须 >= 0";
        const float k = k_two_pi / dv.wavelength_m;
        glUniform4f(detail_l_loc + i, k, dv.amplitude_m, dv.phase_rad, 0.0f);
        const float dl = std::sqrt(dv.dir_x * dv.dir_x + dv.dir_z * dv.dir_z);
        const bool has_dir = dl > 1.0e-6f;
        glUniform2f(detail_dir_loc + i, has_dir ? (dv.dir_x / dl) : 1.0f,
                    has_dir ? (dv.dir_z / dl) : 0.0f);
    }
    glUniform1f(shader_mgr_->GetUniform(prog, "uWaveFadeStart"), on ? w->fade_start_m : 0.0f);
    glUniform1f(shader_mgr_->GetUniform(prog, "uWaveFadeEnd"), on ? w->fade_end_m : 0.0f);
    glUniform1f(shader_mgr_->GetUniform(prog, "uWaveFlowAlign"), on ? w->flow_align : 0.0f);
    glUniform1f(shader_mgr_->GetUniform(prog, "uWaveFlowWindGain"),
                on ? w->flow_wind_gain : 0.0f);
    glUniform1f(shader_mgr_->GetUniform(prog, "uWaveFlowStrength"),
                on ? w->flow_strength : 0.0f);
    glUniform2f(shader_mgr_->GetUniform(prog, "uWaveFlowUvScale"),
                on ? w->flow_uv_scale : 0.0f, on ? w->flow_uv_scale : 0.0f);
    glUniform1i(shader_mgr_->GetUniform(prog, "uWaveDebug"), in.wave_debug);
    glUniform1i(shader_mgr_->GetUniform(prog, "uWaveDisplace"),
                (on && w->displacement) ? 1 : 0);
    glUniform1i(shader_mgr_->GetUniform(prog, "uWaveHasFlow"),
                (on && in.wave_flow_tex != 0u) ? 1 : 0);
    // 短波：只进方差（不进几何）。方向与斜率 RMS 由调用方给。
    float swx = on ? w->short_wave.dir_x : 0.0f;
    float swz = on ? w->short_wave.dir_z : 0.0f;
    float swwind = on ? w->short_wave.wind_speed_ms : 0.0f;
    glUniform2f(shader_mgr_->GetUniform(prog, "uShortWaveDir"), swx, swz);
    glUniform1f(shader_mgr_->GetUniform(prog, "uShortWaveMss"),
                water::CoxMunkMeanSquareSlope(swwind));
}

void WaterSkinRenderer::Draw(const Inputs& in) {
    CHECK(initialized_) << "WaterSkinRenderer::Draw: 未 Init";
    CHECK_NE(in.scene_depth_tex, 0u) << "WaterSkinRenderer::Draw: scene_depth_tex 不能为 0";
    // 水上支路的反射源是天光探针；水下支路只用介质（S_P(∞)），不需要探针。
    CHECK(in.env_cube_tex != 0u || in.camera.position.y() < in.level_y)
        << "WaterSkinRenderer::Draw: 水上必须提供 env_cube_tex";
    // 与 Renderer 同一口径：ior < 1（比空气还疏）没有折射解 ⇒ cosθw=0 ⇒ 增益 1/cosθw = inf
    // 与 (1−R)=0 相乘得 NaN。1.0 = 无界面/调试值，合法。
    CHECK_GE(in.ior, 1.0f) << "WaterSkinRenderer::Draw: ior 必须 >= 1（纯水 1.333）";
    CHECK_GT(in.full_w, 0);
    CHECK_GT(in.full_h, 0);
    CHECK_EQ(in.full_w, wbo_.width()) << "WaterSkinRenderer::Draw: 与 Begin 的宽度不一致";
    CHECK_EQ(in.full_h, wbo_.height()) << "WaterSkinRenderer::Draw: 与 Begin 的高度不一致";

    wbo_.BindForDraw();
    // 全屏三角形：关剔除（NDC 绕序在部分驱动上会被判定为背面）。
    glDisable(GL_CULL_FACE);
    glUseProgram(prog_draw_);
    UploadCommonUniforms(prog_draw_, in);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, in.scene_depth_tex);
    glUniform1i(shader_mgr_->GetUniform(prog_draw_, "uSceneDepth"), 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_CUBE_MAP, in.env_cube_tex);
    glUniform1i(shader_mgr_->GetUniform(prog_draw_, "uEnvCube"), 1);
    // flow map（可选）：纹理单元 2。0 时绑空纹理（uWaveHasFlow=0 不会采样它）。
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, in.wave_flow_tex);
    glUniform1i(shader_mgr_->GetUniform(prog_draw_, "uWaveFlowMap"), 2);
    glActiveTexture(GL_TEXTURE0);

    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);

    glUseProgram(0);
    glActiveTexture(GL_TEXTURE0);
    wbo_.EndDraw();
    has_content_ = true;
}

TransparentLayer WaterSkinRenderer::layer() const {
    TransparentLayer layer;
    if (!has_content_ || !wbo_.valid()) {
        return layer;   // 全 0 = invalid()，TransparentLayerSet::Add 会忽略
    }
    layer.accum_tex = wbo_.accum_tex();
    layer.reveal_tex = wbo_.reveal_tex();
    layer.depth_tex = wbo_.depth_tex();
    layer.scale = 1.0f;
    return layer;
}

}  // namespace jpov
