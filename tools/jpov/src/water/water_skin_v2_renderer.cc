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
#include "tools/jpov/src/csm_shadow_glsl.h"           // kCsmSingleTapGlsl（与 fog/strip 同一份）
#include "tools/jpov/src/water/water_skin_shader.h"       // kWaterSkinVs（全屏三角形，与 V1 同一份）
#include "tools/jpov/src/water/water_skin_v2_shader.h"
#include "tools/jpov/src/water/water_wave_math.h"      // WaterWaves / WavePhaseOriginM
#include "tools/jpov/src/water/water_wave_shader.h"    // kWaterWaveGlsl
#include "tools/jpov/src/water/water_wave_uniforms.h"  // 波浪 uniform 上传（与天球/ V1 共用）

namespace jpov {
namespace {

// CSM 级联纹理占用的纹理单元起点（0 场景深度 / 1 天光探针 / 2 flow map 已占用）。
// 5 个级联 ⇒ 8..12；与 strip 的 kShadowUnitBase 不冲突（不同 pass，用完即还）。
constexpr int kCsmTexUnitBase = 8;

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
    // CSM（阴影只作用在太阳的像上；实现是 csm_shadow_glsl.h 的唯一一份）。
    above_fs += kCsmSingleTapGlsl;
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
         (above_fs + water::kWaterSkinV2DebugGlsl + water::kWaterSkinV2SsrDebugGlsl +
          water::kWaterSkinV2AboveDebugMainGlsl)
             .c_str()});
    prog_below_debug_ = shader_mgr_->GetOrCreate(
        "water_skin_v2_below_debug",
        {water::kWaterSkinVs,
         (below_fs + water::kWaterSkinV2DebugGlsl + water::kWaterSkinV2BelowDebugMainGlsl)
             .c_str()});
    // SSR 的 query 趟（2026-11-27）：只拼接求交/法线所需的片段（无 GGX / 无探针 / 无介质），
    // 所以它与水上/水下的着色 program 是**两份互不影响的编译单元**。
    std::string query_fs = "#version 330 core\n";
    query_fs += water::kWaterInterfaceUniformsGlsl;
    query_fs += water::kWaterWaveGlsl;
    query_fs += water::kWaterSkinV2CommonGlsl;
    query_fs += water::kWaterSkinV2QueryGlsl;
    prog_query_ = shader_mgr_->GetOrCreate("water_skin_v2_ssr_query",
                                          {water::kWaterSkinVs, query_fs.c_str()});
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
    DestroyQueryTarget();
    if (vao_ != 0) {
        glDeleteVertexArrays(1, &vao_);
        vao_ = 0;
    }
    // 4 个 program 由 ShaderManager 拥有（它在自己的析构里 glDeleteProgram），这里只抹掉句柄。
    prog_query_ = 0;
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
                                               int sample_count, const WaterCsmShadow* csm,
                                               float truncate_override_px) {
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
    if (in.waves == nullptr) {
        glUniform1i(shader_mgr_->GetUniform(prog, "uEnvLodOn"), 1);
        glUniform1i(shader_mgr_->GetUniform(prog, "uM1On"), 1);
        glUniform1i(shader_mgr_->GetUniform(prog, "uM2On"), 1);
        glUniform1i(shader_mgr_->GetUniform(prog, "uM3On"), 0);
    } else {
        glUniform1i(shader_mgr_->GetUniform(prog, "uEnvLodOn"), in.waves->v2_env_lod ? 1 : 0);
        glUniform1i(shader_mgr_->GetUniform(prog, "uM1On"), in.waves->v2_m1 ? 1 : 0);
        glUniform1i(shader_mgr_->GetUniform(prog, "uM2On"), in.waves->v2_m2 ? 1 : 0);
        glUniform1i(shader_mgr_->GetUniform(prog, "uM3On"), in.waves->v2_m3 ? 1 : 0);
    }
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
    // M1 的足迹带宽开关（与 V1 同一个字段；GPU 对拍测试传 0 关掉）。
    glUniform1f(shader_mgr_->GetUniform(prog, "uSunFootprint"), in.sun_footprint);

    // ── CSM 阴影（只影响太阳的像；环境反射不吃阴影）──
    // 级联纹理绑到 8..12 号单元（0 场景深度 / 1 天光探针 / 2 flow map 已被占用）。
    const bool has_csm = (csm != nullptr) && csm->IsValid();
    glUniform1i(shader_mgr_->GetUniform(prog, "uHasCsm"), has_csm ? 1 : 0);
    if (has_csm) {
        const int n = std::min(csm->cfg.cascade_count, ShadowConfig::kMaxCascades);
        CHECK_GT(n, 0) << "WaterCsmShadow: cascade_count 必须 > 0（没有级联就别传 csm）";
        CHECK_EQ(static_cast<int>(csm->fbos.size()), n)
            << "WaterCsmShadow: fbos.size() 必须等于 cascade_count";
        glUniform1i(shader_mgr_->GetUniform(prog, "uCascadeCount"), n);
        glUniform1fv(shader_mgr_->GetUniform(prog, "uCascadeRanges"),
                     ShadowConfig::kMaxCascades, csm->cfg.cascade_ranges);
        glUniform1fv(shader_mgr_->GetUniform(prog, "uShadowTexelWorld"),
                     ShadowConfig::kMaxCascades, csm->texel_world);
        glUniform1fv(shader_mgr_->GetUniform(prog, "uShadowBiasCascade"),
                     ShadowConfig::kMaxCascades, csm->cfg.cascade_bias);
        glUniform1i(shader_mgr_->GetUniform(prog, "uShadowBiasOverride"),
                    csm->cfg.override_cascade_bias ? 1 : 0);
        glUniform1f(shader_mgr_->GetUniform(prog, "uShadowFadeStart"), csm->cfg.fade_start);
        glUniform1f(shader_mgr_->GetUniform(prog, "uShadowFadeEnd"), csm->cfg.fade_end);
        for (int c = 0; c < n; ++c) {
            const int unit = kCsmTexUnitBase + c;
            glActiveTexture(GL_TEXTURE0 + unit);
            glBindTexture(GL_TEXTURE_2D, csm->fbos[static_cast<size_t>(c)].tex);
            const std::string idx = "[" + std::to_string(c) + "]";
            glUniform1i(shader_mgr_->GetUniform(prog, ("uShadowMap" + idx).c_str()), unit);
            glUniformMatrix4fv(
                shader_mgr_->GetUniform(prog, ("uShadowVP" + idx).c_str()), 1, GL_FALSE,
                csm->vp[c]);
            glUniformMatrix4fv(
                shader_mgr_->GetUniform(prog, ("uShadowDepthVP" + idx).c_str()), 1, GL_FALSE,
                csm->depth_vp[c]);
        }
        glActiveTexture(GL_TEXTURE0);   // 复位（后续 texture unit 0 = 场景深度）
    }

    // 波浪谱：口径由 WaterWaves::{v2_truncation_px} 决定（V1 = 3/8 px，V2 默认不截断）
    // + 相位局部原点（float 精度手段）。与天球折射共用同一份上传实现。
    water::WaveTruncation trunc =
        in.waves != nullptr ? water::ResolveWaveTruncation(*in.waves)
                            : water::WaveTruncation{};
    if (truncate_override_px > 0.0f) {
        // query 趟专用：**同一个机制、另一组值**（3T~8T px）。着色趟仍用谱自己的口径。
        trunc.truncate_px_main = truncate_override_px;
        trunc.truncate_px_detail = truncate_override_px;
    }
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

void WaterSkinV2Renderer::DestroyQueryTarget() {
    query_ = WaterSsrQueryTextures{};
    if (query_fbo_ != 0) {
        glDeleteFramebuffers(1, &query_fbo_);
        query_fbo_ = 0;
    }
}

void WaterSkinV2Renderer::EnsureQueryTarget(int w, int h) {
    if (query_fbo_ != 0 && query_.width == w && query_.height == h) {
        return;
    }
    DestroyQueryTarget();

    // 三张全分辨率纹理：窗口深度（R32F，装置按 1/z 用）、视空间法线、rough。
    glGenTextures(1, &query_.depth_tex);
    glBindTexture(GL_TEXTURE_2D, query_.depth_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, w, h, 0, GL_RED, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    const auto make_half = [&](unsigned int* out) {
        glGenTextures(1, out);
        glBindTexture(GL_TEXTURE_2D, *out);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_HALF_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    };
    make_half(&query_.normal_tex);
    make_half(&query_.rough_tex);
    glBindTexture(GL_TEXTURE_2D, 0);

    glGenFramebuffers(1, &query_fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, query_fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           query_.depth_tex, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D,
                           query_.normal_tex, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_TEXTURE_2D,
                           query_.rough_tex, 0);
    const GLenum bufs[3] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1,
                            GL_COLOR_ATTACHMENT2};
    glDrawBuffers(3, bufs);
    CHECK_EQ(static_cast<int>(glCheckFramebufferStatus(GL_FRAMEBUFFER)),
             static_cast<int>(GL_FRAMEBUFFER_COMPLETE))
        << "WaterSkinV2Renderer: SSR query FBO 不完整";
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    query_.width = w;
    query_.height = h;
}

void WaterSkinV2Renderer::DrawQuery(const Inputs& in, const WaterSsrQueryTuning& tuning) {
    CHECK(initialized_) << "WaterSkinV2Renderer::DrawQuery: 未 Init";
    CHECK_GT(in.full_w, 0);
    CHECK_GT(in.full_h, 0);
    CHECK_EQ(in.full_w, wbo_.width()) << "DrawQuery: 与 Begin 的宽度不一致";
    CHECK_EQ(in.full_h, wbo_.height()) << "DrawQuery: 与 Begin 的高度不一致";
    CHECK_NE(in.scene_depth_tex, 0u) << "DrawQuery: scene_depth_tex 不能为 0";
    CHECK(in.waves == nullptr || in.waves->v2)
        << "DrawQuery: Query 的几何口径必须与着色趟一致（waves.v2 == true）";
    CHECK(tuning.resolution_scale > 0.0f);
    CHECK(tuning.roughness_to_lod > 0.0f);
    CHECK(tuning.rough_max > 0.0f && tuning.rough_max <= 1.0f);
    CHECK(tuning.normal_truncate_px >= 0.0f);

    EnsureQueryTarget(in.full_w, in.full_h);

    // 保存本趟会动到的状态（调用方后续还要接着画水皮/合成）。
    GLint prev_fbo = 0;
    GLint prev_viewport[4] = {0, 0, 0, 0};
    GLint prev_program = 0;
    const GLint prev_depth_test = glIsEnabled(GL_DEPTH_TEST);
    const GLint prev_blend = glIsEnabled(GL_BLEND);
    const GLint prev_cull = glIsEnabled(GL_CULL_FACE);
    GLint prev_depth_mask = GL_TRUE;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    glGetIntegerv(GL_VIEWPORT, prev_viewport);
    glGetIntegerv(GL_CURRENT_PROGRAM, &prev_program);
    glGetIntegerv(GL_DEPTH_WRITEMASK, &prev_depth_mask);

    glBindFramebuffer(GL_FRAMEBUFFER, query_fbo_);
    glViewport(0, 0, in.full_w, in.full_h);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDepthMask(GL_FALSE);

    glUseProgram(prog_query_);
    // 与着色趟共用同一份 uniform 上传（相机 / 波浪谱 / 介质 / 太阳 / 深度容差）。
    // 传 sample_count = 1：query 只要中心法线（S 不影响几何，也不进这一趟的成本）。
    UploadCommonUniforms(prog_query_, in, /*sample_count=*/1, /*csm=*/nullptr,
                         /*truncate_override_px=*/tuning.normal_truncate_px);
    glUniform1f(shader_mgr_->GetUniform(prog_query_, "uSsrResolutionScale"),
                tuning.resolution_scale);
    glUniform1f(shader_mgr_->GetUniform(prog_query_, "uSsrRoughnessToLod"),
                tuning.roughness_to_lod);
    glUniform1f(shader_mgr_->GetUniform(prog_query_, "uSsrRoughMax"), tuning.rough_max);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, in.scene_depth_tex);
    glUniform1i(shader_mgr_->GetUniform(prog_query_, "uSceneDepth"), 0);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, in.wave_flow_tex);
    glUniform1i(shader_mgr_->GetUniform(prog_query_, "uWaveFlowMap"), 2);
    glActiveTexture(GL_TEXTURE0);

    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);

    glUseProgram(0);

    // 恢复调用方的状态（本趟没有用 WBO 的 BindForDraw/EndDraw 对，
    // 也就不碰它的混合方程 —— 只改 GL 开关，全部在这里还原）。
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<unsigned int>(prev_fbo));
    glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);
    glUseProgram(static_cast<unsigned int>(prev_program));
    glDepthMask(prev_depth_mask);
    if (prev_depth_test != 0) {
        glEnable(GL_DEPTH_TEST);
    } else {
        glDisable(GL_DEPTH_TEST);
    }
    if (prev_blend != 0) {
        glEnable(GL_BLEND);
    } else {
        glDisable(GL_BLEND);
    }
    if (prev_cull != 0) {
        glEnable(GL_CULL_FACE);
    } else {
        glDisable(GL_CULL_FACE);
    }
    glActiveTexture(GL_TEXTURE0);
}

void WaterSkinV2Renderer::Draw(const Inputs& in, int sample_count,
                               const WaterCsmShadow* csm) {
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
    UploadCommonUniforms(prog, in, sample_count, csm);

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
    // SSR 反射图（可选）：纹理单元 3。0 = 关（uSsrOn=0 ⇒ 着色器不采样、逐字节等于旧行为）。
    // 只有水上 program 声明这两个 uniform；水下/调试 program 上 glUniform(-1) 是 no-op。
    const bool ssr_on = (in.ssr_reflection_tex != 0u);
    glUniform1i(shader_mgr_->GetUniform(prog, "uSsrOn"), ssr_on ? 1 : 0);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, in.ssr_reflection_tex);
    glUniform1i(shader_mgr_->GetUniform(prog, "uSsrReflection"), 3);
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
    return layer;
}

}  // namespace jpov
