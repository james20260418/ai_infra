// JPOV FogCompositeRenderer 实现。见 fog_composite_renderer.h。

#define GL_GLEXT_PROTOTYPES

#include "tools/jpov/src/fog/fog_composite_renderer.h"

#include <GL/gl.h>
#include <GL/glext.h>

#ifdef _WIN32
#ifndef GLOG_NO_ABBREVIATED_SEVERITIES
#define GLOG_NO_ABBREVIATED_SEVERITIES
#endif
#include "third_party/gl_loader-mingw/gl_loader.h"
#endif

#include <glog/logging.h>

#include <vector>

#include "tools/jpov/src/fog/fog_shader.h"
#include "tools/jpov/src/matrix_util.h"
#include "tools/jpov/src/water/water_medium_shader.h"   // kDefaultMediumGlsl

namespace jpov {

namespace {

// composite 的纹理单元分配（固定顺序，避免各调用点各记一套）。
constexpr GLint kScatterUnit = 0;
constexpr GLint kSceneDepthUnit = 1;
constexpr GLint kLayerUnitBase = 2;   // 每个层占 3 个连续单元（accum/reveal/depth）
// 本趟触碰的单元总数：雾散射 + 场景深度 + 4 个层槽（空槽也绑哑元，
// 否则采样器默认指向 unit 0，会把雾散射纹理当成层累积读）。
constexpr int kUnitsTouched = kLayerUnitBase + kMaxTransparentLayers * 3;

// 每个层槽的 3 个采样器 uniform 名（GLSL 330 不能动态索引采样器，故按槽展开）。
constexpr const char* kLayerSlotUniforms[kMaxTransparentLayers][3] = {
    {"uLayer0Accum", "uLayer0Reveal", "uLayer0Depth"},
    {"uLayer1Accum", "uLayer1Reveal", "uLayer1Depth"},
    {"uLayer2Accum", "uLayer2Reveal", "uLayer2Depth"},
    {"uLayer3Accum", "uLayer3Reveal", "uLayer3Depth"},
};

// 组装一遍 FS 源码（#version 330 core + 公共前置 + 介质提供者 + 趟体）。
//
// 介质提供者（medium_glsl）必须定义 `MediumAt(ro, rd, z, out tau, out S)`；语义是
// 「相机 → z（沿视线欧氏距离）」的**前缀累积** (τ, S)。水体实现在
// src/water/water_medium_shader.h（物理与 water_medium_math.h 同源）。
std::string AssembleCompositeShader(const char* medium_glsl, const char* body) {
    std::string s = "#version 330 core\n";
    s += kFogCommonGlsl;
    // 水面接口 + 介质 uniform：MediumAt 依赖它们（2026-11-25 重构后 kWaterMediumGlsl 只含函数）。
    s += water::kWaterInterfaceUniformsGlsl;
    s += water::kWaterMediumUniformsGlsl;
    s += medium_glsl;
    s += body;
    return s;
}

}  // namespace

FogCompositeRenderer::~FogCompositeRenderer() {
    // GL 资源由 Finalize 释放（析构不碰 GL：context 可能已先死）。
}

void FogCompositeRenderer::Init(ShaderManager* shader_mgr, const char* medium_glsl) {
    CHECK(shader_mgr != nullptr);
    if (medium_glsl == nullptr) {
        medium_glsl = water::kDefaultMediumGlsl;
    }
    shader_mgr_ = shader_mgr;
    fs_composite_ = AssembleCompositeShader(medium_glsl, kFsComposite);
    prog_composite_ = shader_mgr_->GetOrCreate(
        "fog_composite", {kFogVs, fs_composite_.c_str()});

    // 1×1 深度哑元（值 = 1.0 ⇒ 屏幕深度取远平面）：无场景深度时的占位。
    const float one = 1.0f;
    glGenTextures(1, &dummy_depth_tex_);
    glBindTexture(GL_TEXTURE_2D, dummy_depth_tex_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, 1, 1, 0, GL_RED, GL_FLOAT, &one);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void FogCompositeRenderer::Finalize() {
    if (dummy_depth_tex_ != 0) {
        glDeleteTextures(1, &dummy_depth_tex_);
        dummy_depth_tex_ = 0;
    }
    shader_mgr_ = nullptr;
    prog_composite_ = 0;
    fs_composite_.clear();
}

void FogCompositeRenderer::Draw(const FogField& field,
                                unsigned int scene_depth_tex,
                                const Camera& cam,
                                const float view_proj[16],
                                const FogParams& params,
                                const TransparentLayerSet& layers,
                                const water::WaterMediumSample* medium,
                                int water_layer_index) {
    CHECK(shader_mgr_ != nullptr) << "FogCompositeRenderer::Init 未调用";
    if (!field.valid() && layers.count == 0 && medium == nullptr) {
        return;   // 无雾场、无层、无介质：零开销
    }
    CHECK_LE(layers.count, kMaxTransparentLayers)
        << "layers.count 超过 kMaxTransparentLayers";
    CHECK_GT(params.z_near, 0.0f) << "FogParams.z_near 必须 > 0";
    CHECK_GT(params.z_far, params.z_near) << "FogParams.z_far 必须 > z_near";

    float inv_vp[16];
    CHECK(Mat4Invert(view_proj, inv_vp))
        << "FogCompositeRenderer: 相机矩阵不可逆（near/far 非法或退化）";

    // ── 保存调用方状态（见头文件「GL 状态机契约」）──
    //    ⚠️ 只用 glBlendFunc / GL_BLEND_SRC_ALPHA（**不用** glBlendFuncSeparate）：
    //    Windows 的 MinGW GL 扩展加载器没声明它，交叉编译会 undefined reference。
    //    alpha 不变靠**片元输出的 alpha** 保证（见 kFsComposite：乘 T 的趟输出 a=1、
    //    加 L 的趟输出 a=0），而不是靠分开的混合因子。
    GLint prev_blend_src = 0;
    GLint prev_blend_dst = 0;
    GLint prev_vp[4] = {0, 0, 0, 0};
    GLint prev_active_tex = 0;
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &prev_blend_src);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &prev_blend_dst);
    glGetIntegerv(GL_VIEWPORT, prev_vp);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_active_tex);
    std::vector<GLint> prev_tex_binding(static_cast<size_t>(kUnitsTouched), 0);
    for (int u = 0; u < kUnitsTouched; ++u) {
        glActiveTexture(GL_TEXTURE0 + u);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex_binding[static_cast<size_t>(u)]);
    }

    // viewport 必须与雾场构建时的 3D 视口一致：SampleFogBilinear 用 gl_FragCoord 对齐 tile。
    if (field.viewport_w > 0 && field.viewport_h > 0) {
        glViewport(0, 0, field.viewport_w, field.viewport_h);
    }

    glUseProgram(prog_composite_);
    // 无雾场时绑哑元 + uHasFog=0（着色器整条跳过 froxel 采样，恒等）。
    glActiveTexture(GL_TEXTURE0 + kScatterUnit);
    glBindTexture(GL_TEXTURE_2D, field.valid() ? field.scatter_tex : dummy_depth_tex_);
    glUniform1i(shader_mgr_->GetUniform(prog_composite_, "uScatter"), kScatterUnit);
    glUniform1i(shader_mgr_->GetUniform(prog_composite_, "uHasFog"),
                field.valid() ? 1 : 0);
    // 【调试】背景像素的介质积分上限；0 = 关（原行为）
    glUniform1f(shader_mgr_->GetUniform(prog_composite_, "uDepthInfinityM"),
                jpov::kDepthInfinityM);
    // 水凝胶的"无穷远"是**真无穷**（解析解在 len→∞ 时收敛）。
    glUniform1f(shader_mgr_->GetUniform(prog_composite_, "uWaterInfiniteZ"),
                jpov::water::kWaterInfiniteZ);
    glUniform1i(shader_mgr_->GetUniform(prog_composite_, "uWaterLayerIndex"), water_layer_index);

    const unsigned int depth_tex =
        scene_depth_tex != 0 ? scene_depth_tex : dummy_depth_tex_;
    glActiveTexture(GL_TEXTURE0 + kSceneDepthUnit);
    glBindTexture(GL_TEXTURE_2D, depth_tex);
    glUniform1i(shader_mgr_->GetUniform(prog_composite_, "uSceneDepthTex"),
                kSceneDepthUnit);

    glActiveTexture(GL_TEXTURE0);
    glUniformMatrix4fv(shader_mgr_->GetUniform(prog_composite_, "uInvVP"), 1, GL_FALSE,
                       inv_vp);
    glUniform3f(shader_mgr_->GetUniform(prog_composite_, "uCamPos"), cam.position.x(),
                cam.position.y(), cam.position.z());
    glUniform1f(shader_mgr_->GetUniform(prog_composite_, "uZNear"), params.z_near);
    glUniform1f(shader_mgr_->GetUniform(prog_composite_, "uZFar"), params.z_far);
    glUniform1f(shader_mgr_->GetUniform(prog_composite_, "uZScale"),
                static_cast<float>(params.nz) / std::log(params.z_far / params.z_near));
    glUniform1i(shader_mgr_->GetUniform(prog_composite_, "uGridCols"), field.grid_cols);
    glUniform1i(shader_mgr_->GetUniform(prog_composite_, "uGridRows"), field.grid_rows);
    glUniform1i(shader_mgr_->GetUniform(prog_composite_, "uNz"), params.nz);
    glUniform1i(shader_mgr_->GetUniform(prog_composite_, "uBlockSide"), field.block_side);
    glUniform1i(shader_mgr_->GetUniform(prog_composite_, "uTilePx"), params.tile_px);

    // ── 透明层：4 槽全绑（空槽绑哑元，避免未定义采样）；uLayerCount 决定采样几个 ──
    for (int slot = 0; slot < kMaxTransparentLayers; ++slot) {
        const bool used = slot < layers.count && layers.layers[slot].valid();
        const unsigned int accum =
            used ? layers.layers[slot].accum_tex : dummy_depth_tex_;
        const unsigned int reveal =
            used ? layers.layers[slot].reveal_tex : dummy_depth_tex_;
        const unsigned int depth =
            used ? layers.layers[slot].depth_tex : dummy_depth_tex_;
        const GLint unit = kLayerUnitBase + slot * 3;
        glActiveTexture(GL_TEXTURE0 + unit + 0);
        glBindTexture(GL_TEXTURE_2D, accum);
        glUniform1i(shader_mgr_->GetUniform(prog_composite_,
                                            kLayerSlotUniforms[slot][0]),
                    unit + 0);
        glActiveTexture(GL_TEXTURE0 + unit + 1);
        glBindTexture(GL_TEXTURE_2D, reveal);
        glUniform1i(shader_mgr_->GetUniform(prog_composite_,
                                            kLayerSlotUniforms[slot][1]),
                    unit + 1);
        glActiveTexture(GL_TEXTURE0 + unit + 2);
        glBindTexture(GL_TEXTURE_2D, depth);
        glUniform1i(shader_mgr_->GetUniform(prog_composite_,
                                            kLayerSlotUniforms[slot][2]),
                    unit + 2);
    }
    glUniform1i(shader_mgr_->GetUniform(prog_composite_, "uLayerCount"), layers.count);
    glActiveTexture(GL_TEXTURE0);

    // ── 全局均质介质（水体）：uniform 始终上传（无介质时置 0/零，防上帧残留）──
    //    物理与 src/water/water_medium_math.h 的 WaterAt 同源，片段由 Init 拼入。
    const bool has_medium = (medium != nullptr);
    glUniform1i(shader_mgr_->GetUniform(prog_composite_, "uHasWater"),
                has_medium ? 1 : 0);
    if (has_medium) {
        glUniform1f(shader_mgr_->GetUniform(prog_composite_, "uWaterLevelY"),
                    medium->level_y);
        glUniform1f(shader_mgr_->GetUniform(prog_composite_, "uWaterIor"), medium->ior);
        glUniform3f(shader_mgr_->GetUniform(prog_composite_, "uWaterSigmaA"),
                    medium->sigma_a.x(), medium->sigma_a.y(), medium->sigma_a.z());
        glUniform3f(shader_mgr_->GetUniform(prog_composite_, "uWaterSigmaS"),
                    medium->sigma_s.x(), medium->sigma_s.y(), medium->sigma_s.z());
        glUniform1f(shader_mgr_->GetUniform(prog_composite_, "uWaterPhaseG"),
                    medium->phase_g);
        glUniform3f(shader_mgr_->GetUniform(prog_composite_, "uWaterAmbient"),
                    medium->ambient.x(), medium->ambient.y(), medium->ambient.z());
        glUniform3f(shader_mgr_->GetUniform(prog_composite_, "uWaterSunColor"),
                    medium->sun_color.x(), medium->sun_color.y(), medium->sun_color.z());
        glUniform3f(shader_mgr_->GetUniform(prog_composite_, "uWaterSunToward"),
                    medium->sun_toward.x(), medium->sun_toward.y(),
                    medium->sun_toward.z());
        glUniform1i(shader_mgr_->GetUniform(prog_composite_, "uWaterHasSun"),
                    medium->has_sun ? 1 : 0);
    } else {
        glUniform1f(shader_mgr_->GetUniform(prog_composite_, "uWaterLevelY"), 0.0f);
        glUniform1f(shader_mgr_->GetUniform(prog_composite_, "uWaterIor"), 1.333f);
        glUniform3f(shader_mgr_->GetUniform(prog_composite_, "uWaterSigmaA"), 0.0f, 0.0f,
                    0.0f);
        glUniform3f(shader_mgr_->GetUniform(prog_composite_, "uWaterSigmaS"), 0.0f, 0.0f,
                    0.0f);
        glUniform1f(shader_mgr_->GetUniform(prog_composite_, "uWaterPhaseG"), 0.0f);
        glUniform3f(shader_mgr_->GetUniform(prog_composite_, "uWaterAmbient"), 0.0f, 0.0f,
                    0.0f);
        glUniform3f(shader_mgr_->GetUniform(prog_composite_, "uWaterSunColor"), 0.0f, 0.0f,
                    0.0f);
        glUniform3f(shader_mgr_->GetUniform(prog_composite_, "uWaterSunToward"), 0.0f, 1.0f,
                    0.0f);
        glUniform1i(shader_mgr_->GetUniform(prog_composite_, "uWaterHasSun"), 0);
    }

    // 就地合成：dst = L + dst·T
    glEnable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    if (!has_medium) {
        // 旧路径：一次 draw、标量 a（无水时三通道相等 ⇒ 雾 gold 逐字节不变）
        glUniform1i(shader_mgr_->GetUniform(prog_composite_, "uSpectralOutput"), 0);
        glBlendFunc(GL_ONE, GL_SRC_ALPHA);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    } else {
        // 逐通道透射：固定管线无法一次做 `dst·T(rgb) + L` ⇒ 两次 draw。
        // 顺序必须是「先乘后加」（反了会得到 (dst+L)·T）；alpha 因子取 ZERO/ONE ⇒ alpha 不变。
        glUniform1i(shader_mgr_->GetUniform(prog_composite_, "uSpectralOutput"), 1);
        glBlendFunc(GL_ZERO, GL_SRC_COLOR);   // rgb *= T（片元 a=1 ⇒ dst 的 a 不变）
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glUniform1i(shader_mgr_->GetUniform(prog_composite_, "uSpectralOutput"), 2);
        glBlendFunc(GL_ONE, GL_ONE);          // rgb += L（片元 a=0 ⇒ dst 的 a 不变）
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }

    // ── 复原状态（调用方随后做 highlight / bloom / tone map / 2D，均自设状态）──
    // enable 位由调用方 glPushAttrib(GL_ENABLE_BIT) 兜底；此处仍显式关掉。
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    // ⭐ 回写混合函数：GL_BLEND_SRC/DST_* 属 GL_COLOR_BUFFER_BIT，**不在** enable 位里，
    //   必须显式复原成本趟进入前的值（否则混合模式会沿用到 2D/字体）。
    glBlendFunc(static_cast<GLenum>(prev_blend_src), static_cast<GLenum>(prev_blend_dst));
    glUseProgram(0);
    // 回写 viewport 与纹理单元绑定，再复原活动单元。
    glViewport(prev_vp[0], prev_vp[1], prev_vp[2], prev_vp[3]);
    for (int u = 0; u < kUnitsTouched; ++u) {
        glActiveTexture(GL_TEXTURE0 + u);
        glBindTexture(GL_TEXTURE_2D,
                      static_cast<GLuint>(prev_tex_binding[static_cast<size_t>(u)]));
    }
    glActiveTexture(static_cast<GLenum>(prev_active_tex));
}

}  // namespace jpov
