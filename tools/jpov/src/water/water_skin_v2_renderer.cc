// JPOV WaterSkinV2Renderer 实现（只产层，不合成 —— 见头文件顶部）
//
// GL 状态契约（与 V1 完全一致）：
//   · Draw() 只绑自己的 WBO FBO（BindForDraw/EndDraw 成对），退出时 FBO 已解绑；
//   · 纹理单元 0（场景深度）、1（天光探针）、2（flow map）会被改绑 —— 调用方后续自己重绑；
//   · 不改 blend / depth-test 的 enable 位（WBO 的 BindForDraw 自己设好并在 EndDraw 复原）。

#define GL_GLEXT_PROTOTYPES

#include "tools/jpov/src/water/water_skin_v2_renderer.h"

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
// ⚠️ MinGW 的 <GL/gl.h> 会拉进 windows.h，而它把 `near` / `far` 定义成**空宏**（本项目已知坑）。
#undef near
#undef far
#endif

#include "tools/jpov/src/water/water_medium_shader.h"   // kWaterInterfaceUniformsGlsl + kWaterMediumUniformsGlsl + kWaterMediumGlsl
#include "tools/jpov/src/water/water_skin_shader.h"       // kWaterSkinVs（全屏三角形，与 V1 同一份）
#include "tools/jpov/src/water/water_skin_v2_shader.h"
#include "tools/jpov/src/water/water_wave_math.h"      // WaterWaves / WavePhaseOriginM
#include "tools/jpov/src/water/water_wave_shader.h"    // kWaterWaveGlsl
#include "tools/jpov/src/water/water_wave_uniforms.h"  // 波浪 uniform 上传（与天球/ V1 共用）

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

WaterSkinV2Renderer::WaterSkinV2Renderer() = default;
WaterSkinV2Renderer::~WaterSkinV2Renderer() = default;

void WaterSkinV2Renderer::Init(ShaderManager* shader_mgr) {
    CHECK(shader_mgr != nullptr) << "WaterSkinV2Renderer::Init: shader_mgr 不能为空";
    CHECK(!initialized_) << "WaterSkinV2Renderer::Init 重复调用";
    shader_mgr_ = shader_mgr;
    // 4 个 program：水上/水下 × 生产/调试（拼装顺序见 water_skin_v2_shader.h 顶部）。
    //   水上：接口 uniform + 波浪 + GGX + 公共 + 水上着色；不含介质（无 MediumAt）。
    //   水下：接口 uniform + 介质 uniform + 介质函数 + 波浪 + 公共 + 水下着色；不含 GGX/太阳。
    std::string above_fs = "#version 330 core\n";
    above_fs += water::kWaterInterfaceUniformsGlsl;
    above_fs += water::kWaterWaveGlsl;
    above_fs += lighting::kGgxSpecularGlsl;
    above_fs += water::kWaterSkinV2CommonGlsl;
    above_fs += water::kWaterSkinV2AboveGlsl;

    std::string below_fs = "#version 330 core\n";
    below_fs += water::kWaterInterfaceUniformsGlsl;
    below_fs += water::kWaterMediumUniformsGlsl;
    below_fs += water::kWaterMediumGlsl;
    below_fs += water::kWaterWaveGlsl;
    below_fs += water::kWaterSkinV2CommonGlsl;
    below_fs += water::kWaterSkinV2BelowGlsl;

    prog_above_ = shader_mgr_->GetOrCreate(
        "water_skin_v2_above",
        {water::kWaterSkinVs, (above_fs + water::kWaterSkinV2AboveMainGlsl).c_str()});
    prog_below_ = shader_mgr_->GetOrCreate(
        "water_skin_v2_below",
        {water::kWaterSkinVs, (below_fs + water::kWaterSkinV2BelowMainGlsl).c_str()});
    prog_above_debug_ = shader_mgr_->GetOrCreate(
        "water_skin_v2_above_debug",
        {water::kWaterSkinVs,
         (above_fs + water::kWaterSkinV2DebugGlsl + water::kWaterSkinV2AboveDebugMainGlsl)
             .c_str()});
    prog_below_debug_ = shader_mgr_->GetOrCreate(
        "water_skin_v2_below_debug",
        {water::kWaterSkinVs,
         (below_fs + water::kWaterSkinV2DebugGlsl + water::kWaterSkinV2BelowDebugMainGlsl)
             .c_str()});
    glGenVertexArrays(1, &vao_);
    CHECK_NE(vao_, 0u) << "WaterSkinV2Renderer::Init: glGenVertexArrays 失败";
    initialized_ = true;
}

unsigned int WaterSkinV2Renderer::SelectProgram(const Inputs& in) const {
    const bool under = in.camera.position.y() < in.level_y;
    const bool debug = in.wave_debug != 0;
    if (under) {
        return debug ? prog_below_debug_ : prog_below_;
    }
    return debug ? prog_above_debug_ : prog_above_;
}

void WaterSkinV2Renderer::Finalize() {
    wbo_.Destroy();
    if (vao_ != 0) {
        glDeleteVertexArrays(1, &vao_);
        vao_ = 0;
    }
    // 4 个 program 由 ShaderManager 拥有（它在自己的析构里 glDeleteProgram），这里只抹掉句柄。
    prog_above_ = 0;
    prog_below_ = 0;
    prog_above_debug_ = 0;
    prog_below_debug_ = 0;
    has_content_ = false;
    initialized_ = false;
}

void WaterSkinV2Renderer::EnsureTarget(int w, int h) {
    // 与 V1 同一份 WBO 契约：1× 层、不含加法集、申请 depth 附件（att3）。
    wbo_.Ensure({w, h, /*with_additive=*/false, /*with_depth=*/true,
                 /*linear_filter=*/false});
}

void WaterSkinV2Renderer::Begin(int full_w, int full_h) {
    CHECK(initialized_) << "WaterSkinV2Renderer::Begin: 未 Init";
    CHECK_GT(full_w, 0) << "WaterSkinV2Renderer::Begin: full_w 必须 > 0";
    CHECK_GT(full_h, 0) << "WaterSkinV2Renderer::Begin: full_h 必须 > 0";
    EnsureTarget(full_w, full_h);
    wbo_.Clear();
    has_content_ = false;
}

void WaterSkinV2Renderer::UploadCommonUniforms(unsigned int prog, const Inputs& in,
                                               int sample_count) {
    // ⚠️ 四个 program 各只声明自己需要的 uniform（水上无介质、水下无 env/太阳），这里**统一全传**：
    //    未声明的 uniform 在 GetUniform 返回 -1，glUniform* 对 -1 是静默 no-op（GL 惯例）。
    //    与 V1 的差异共四条：S、谱不截断、相位局部原点 + 局部系相机 xz；
    //    没有 uSunFootprint（V2 无 dFdx AA）。
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
    // V2 唯一的门控量：每像素采样数（GLSL 侧还有 clamp 兜底，这里负责拦住 API 误用）。
    glUniform1i(shader_mgr_->GetUniform(prog, "uSampleCount"), sample_count);
    // 方差系两个消费点的开关（水面以上着色才消费；水下 program 没有这两个 uniform ⇒ no-op）。
    const bool env_lod_on = (in.waves == nullptr) || in.waves->v2_env_lod;
    const bool sun_lobe_on = (in.waves == nullptr) || in.waves->v2_sun_lobe;
    glUniform1i(shader_mgr_->GetUniform(prog, "uEnvLodOn"), env_lod_on ? 1 : 0);
    glUniform1i(shader_mgr_->GetUniform(prog, "uSunLobeOn"), sun_lobe_on ? 1 : 0);
    // 波浪局部系的相机 xz：**必须在 double 里减**（相机的 float 值已经是 1e5 量级时，
    // 在着色器里减已经晚了 —— 那一步之前 `cam + rd·t` 就已经丢位了）。
    const float origin_x = water::WavePhaseOriginM(cam.position.x());
    const float origin_z = water::WavePhaseOriginM(cam.position.z());
    glUniform2f(shader_mgr_->GetUniform(prog, "uCamPosXZLocal"),
                static_cast<float>(static_cast<double>(cam.position.x()) - origin_x),
                static_cast<float>(static_cast<double>(cam.position.z()) - origin_z));

    // ── 介质 uniform（水下支路的 S_P(∞) 与水平面/折射率）──
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

    // ── 太阳（解析 GGX，盘状光源）—— V2 没有 uSunFootprint（见着色片段注释）──
    glUniform1i(shader_mgr_->GetUniform(prog, "uHasSun"), in.has_sun ? 1 : 0);
    glUniform3f(shader_mgr_->GetUniform(prog, "uSunToward"),
                in.sun_toward.x(), in.sun_toward.y(), in.sun_toward.z());
    glUniform3f(shader_mgr_->GetUniform(prog, "uSunIrradiance"),
                in.has_sun ? in.sun_irradiance.x() : 0.0f,
                in.has_sun ? in.sun_irradiance.y() : 0.0f,
                in.has_sun ? in.sun_irradiance.z() : 0.0f);
    glUniform1f(shader_mgr_->GetUniform(prog, "uSunAngularRadius"), in.sun_angular_radius);

    // 波浪谱：口径由 WaterWaves::{v2_truncation_px} 决定（V1 = 3/8 px，V2 默认不截断）
    // + 相位局部原点（float 精度手段）。与天球折射共用同一份上传实现。
    const water::WaveTruncation trunc =
        in.waves != nullptr ? water::ResolveWaveTruncation(*in.waves)
                            : water::WaveTruncation{};
    water::WaveUniformParams wp;
    wp.time_s = in.wave_time_s;
    wp.depth_m = in.wave_depth_m;
    wp.debug = in.wave_debug;
    wp.displacement = (in.waves != nullptr) && in.waves->displacement;
    wp.has_flow_tex = in.wave_flow_tex != 0u;
    wp.truncate_px_main = trunc.truncate_px_main;
    wp.truncate_px_detail = trunc.truncate_px_detail;
    wp.origin_x = origin_x;
    wp.origin_z = origin_z;
    water::UploadWaterWaveUniforms(*shader_mgr_, prog, in.waves, wp);
}

void WaterSkinV2Renderer::Draw(const Inputs& in, int sample_count) {
    CHECK(initialized_) << "WaterSkinV2Renderer::Draw: 未 Init";
    CHECK_GE(sample_count, kMinSamples)
        << "WaterSkinV2Renderer::Draw: sample_count 必须 >= " << kMinSamples;
    CHECK_LE(sample_count, kMaxSamples)
        << "WaterSkinV2Renderer::Draw: sample_count 必须 <= " << kMaxSamples;
    // 本 pass 按 `WaterWaves::v2_truncation_px` 上传谱口径（默认不截断，T>0 时按像素足迹截断）。
    // 天球折射读的是同一个字段 —— 两者必须同口径，否则 Snell 窗缘会用另一套几何（漏色）。
    CHECK(in.waves == nullptr || in.waves->v2)
        << "WaterSkinV2Renderer::Draw: 传进来的波浪谱必须标了 v2=true "
           "（否则天球那边会按 V1 截断谱，Snell 窗缘会错位）";
    CHECK_NE(in.scene_depth_tex, 0u) << "WaterSkinV2Renderer::Draw: scene_depth_tex 不能为 0";
    CHECK(in.env_cube_tex != 0u || in.camera.position.y() < in.level_y)
        << "WaterSkinV2Renderer::Draw: 水上必须提供 env_cube_tex";
    CHECK_GE(in.ior, 1.0f) << "WaterSkinV2Renderer::Draw: ior 必须 >= 1（纯水 1.333）";
    CHECK_GT(in.full_w, 0);
    CHECK_GT(in.full_h, 0);
    CHECK_EQ(in.full_w, wbo_.width()) << "WaterSkinV2Renderer::Draw: 与 Begin 的宽度不一致";
    CHECK_EQ(in.full_h, wbo_.height()) << "WaterSkinV2Renderer::Draw: 与 Begin 的高度不一致";

    wbo_.BindForDraw();
    // 全屏三角形：关剔除（NDC 绕序在部分驱动上会被判定为背面）。
    glDisable(GL_CULL_FACE);
    const unsigned int prog = SelectProgram(in);
    glUseProgram(prog);
    UploadCommonUniforms(prog, in, sample_count);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, in.scene_depth_tex);
    glUniform1i(shader_mgr_->GetUniform(prog, "uSceneDepth"), 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_CUBE_MAP, in.env_cube_tex);
    glUniform1i(shader_mgr_->GetUniform(prog, "uEnvCube"), 1);
    // flow map（可选）：纹理单元 2。0 时绑空纹理（uWaveHasFlow=0 不会采样它）。
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, in.wave_flow_tex);
    glUniform1i(shader_mgr_->GetUniform(prog, "uWaveFlowMap"), 2);
    glActiveTexture(GL_TEXTURE0);

    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);

    glUseProgram(0);
    glActiveTexture(GL_TEXTURE0);
    wbo_.EndDraw();
    has_content_ = true;
}

TransparentLayer WaterSkinV2Renderer::layer() const {
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
