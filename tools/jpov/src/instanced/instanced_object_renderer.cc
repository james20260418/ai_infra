// JPOV InstancedObjectRenderer 实现
//
// 静态模型批量实例（DrawInstancedObject）+ 实例阴影（DrawInstancedObjectShadow）。
// 光照/太阳/环境光 uniform 上传与 Object3DRenderer 同源（见头文件顶部的过渡期同步约定）。

#define GL_GLEXT_PROTOTYPES

#include "tools/jpov/src/instanced/instanced_object_renderer.h"

#include "tools/jpov/src/texture_units.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

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
// MinGW 的 windows.h 定义了 near/far 宏，与 C++ 变量名冲突（cmds.camera.near 等）
#undef near
#undef far
#include "third_party/gl_loader-mingw/gl_loader.h"

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#endif

#include <glog/logging.h>

namespace jpov {

namespace {

// 构建局部→世界 model 矩阵（列主序，纯 CPU，不碰 GL 矩阵栈；顶点右乘：model*pos 先缩放后旋转再平移）。
// scale 为整体缩放（作用于局部坐标，先放大/缩小再走 up/front 旋转平移）。
// ⚠️ 主序：本仓库模型矩阵用“列行主序书写、列主序存储”的转置布局
//   （第 4 列装 center 平移），与业界列主序 MVF 一致（clip = proj*view*model*pos）；
//   但 BuildModelMatrix 内部以“行主序书写、列主序存储”写出（与 BuildLookAt 同款），
//   调用方用同一套 Mat4Mul 相乘即可，勿与教科书 column-major 混写。
void BuildModelMatrix(const Vec3f& center,
                      const Vec3f& up,
                      const Vec3f& front,
                      float scale,
                      float model[16]) {
    float u_len = std::sqrt(up.x()*up.x() + up.y()*up.y() + up.z()*up.z());
    float f_len = std::sqrt(front.x()*front.x() + front.y()*front.y() + front.z()*front.z());
    Vec3f upn = {up.x()/u_len, up.y()/u_len, up.z()/u_len};
    Vec3f frn = {front.x()/f_len, front.y()/f_len, front.z()/f_len};

    Vec3f right = {upn.y()*frn.z() - upn.z()*frn.y(),
                   upn.z()*frn.x() - upn.x()*frn.z(),
                   upn.x()*frn.y() - upn.y()*frn.x()};
    float r_len = std::sqrt(right.x()*right.x() + right.y()*right.y() + right.z()*right.z());
    right = {right.x()/r_len, right.y()/r_len, right.z()/r_len};

    CHECK_GT(scale, 0.0f) << "BuildModelMatrix: scale 必须 > 0，当前=" << scale;
    if (scale != 1.0f) {
        right = {right.x()*scale, right.y()*scale, right.z()*scale};
        upn   = {upn.x()*scale,   upn.y()*scale,   upn.z()*scale};
        frn   = {frn.x()*scale,   frn.y()*scale,   frn.z()*scale};
    }

    model[0] = right.x(); model[4] = upn.x(); model[8]  = frn.x(); model[12] = center.x();
    model[1] = right.y(); model[5] = upn.y(); model[9]  = frn.y(); model[13] = center.y();
    model[2] = right.z(); model[6] = upn.z(); model[10] = frn.z(); model[14] = center.z();
    model[3] = 0.0f;      model[7] = 0.0f;    model[11] = 0.0f;    model[15] = 1.0f;
}

}  // namespace

// ==================== UploadLightData ====================

void InstancedObjectRenderer::UploadLightData(const RenderCommandList& cmds,
                                               ShaderManager& shader_mgr,
                                               unsigned int prog,
                                               unsigned int prog_full) {
    const unsigned int programs[] = {prog, prog_full};

    const int total = static_cast<int>(cmds.point_lights.size());
    const int clamped = std::min(total, kMaxTotalLights);

    char buf[64];
    for (unsigned int p : programs) {
        glUseProgram(p);
        for (int i = 0; i < clamped; ++i) {
            const PointLight& l = cmds.point_lights[i];
            snprintf(buf, sizeof(buf), "uLights[%d].position", i);
            glUniform3f(shader_mgr.GetUniform(p, buf),
                        l.position.x(), l.position.y(), l.position.z());
            snprintf(buf, sizeof(buf), "uLights[%d].color", i);
            glUniform3f(shader_mgr.GetUniform(p, buf),
                        l.color.r, l.color.g, l.color.b);
            snprintf(buf, sizeof(buf), "uLights[%d].radius", i);
            glUniform1f(shader_mgr.GetUniform(p, buf), l.linear_radius);
            snprintf(buf, sizeof(buf), "uLights[%d].physicalRadius", i);
            glUniform1f(shader_mgr.GetUniform(p, buf), l.physical_radius);
            snprintf(buf, sizeof(buf), "uLights[%d].intensity", i);
            glUniform1f(shader_mgr.GetUniform(p, buf), l.intensity);
        }
        glUniform1i(shader_mgr.GetUniform(p, "uTotalLights"), clamped);
        // uTileCulling=1（默认）：片元走 tile 索引纹理；=0：遍历全部光源。
        glUniform1i(shader_mgr.GetUniform(p, "uTileCulling"),
                    cmds.tile_culling ? 1 : 0);
    }
}

// ==================== UploadAmbient ====================

void InstancedObjectRenderer::UploadAmbient(ShaderManager& shader_mgr,
                                             unsigned int prog,
                                             unsigned int prog_full,
                                             const AmbientLight& ambient) {
    const unsigned int programs[] = {prog, prog_full};
    for (unsigned int p : programs) {
        glUseProgram(p);
        glUniform3f(shader_mgr.GetUniform(p, "uAmbientColor"),
                    ambient.color.r, ambient.color.g, ambient.color.b);
        glUniform1f(shader_mgr.GetUniform(p, "uAmbientIntensity"),
                    std::max(ambient.intensity, 0.0f));  // 负值 clamp 到 0
        // 三色环境光（可选）：存在则开开关 + 上传 [天, 天际线, 地] 三色（每色 vec3）。
        //   按数组名取 location = 元素 [0] 的 location（GL 惯例），一次 glUniform3fv 传 3 个。
        //   location 为 -1（shader 未声明）时 glUniform 是 no-op，安全。
        glUniform1i(shader_mgr.GetUniform(p, "uAmbientTricolorEnabled"),
                    ambient.tricolor.has_value() ? 1 : 0);
        if (ambient.tricolor.has_value()) {
            // ⚠️ 用三次 glUniform3f（逐元素名）而非 glUniform3fv：Windows 的 MinGW GL
            //   扩展加载器（third_party/gl_loader-mingw）未声明 glUniform3fv。
            const std::array<Color, 3>& tc = *ambient.tricolor;
            static const char* const kTricolorUniforms[3] = {
                "uAmbientTricolor[0]", "uAmbientTricolor[1]", "uAmbientTricolor[2]"};
            for (int i = 0; i < 3; ++i) {
                glUniform3f(shader_mgr.GetUniform(p, kTricolorUniforms[i]),
                            tc[i].r, tc[i].g, tc[i].b);
            }
        }
    }
}

// ==================== UploadSunData ====================

void InstancedObjectRenderer::UploadSunData(
    ShaderManager& shader_mgr,
    unsigned int prog,
    unsigned int prog_full,
    const std::vector<CascadeFBO>& shadow_fbos,
    const float shadow_vp[][16],
    const float shadow_depth_vp[][16],
    const float shadow_texel_world[],
    const ShadowConfig& cfg,
    const ShadowPcfConfig& pcf,
    const std::optional<DirectionalLight>& sun) {
    const int cascade_count = sun.has_value() ? cfg.cascade_count : 0;

    // PCF 采样核参数校验 + 黄金角螺旋采点偏移表（CPU 预算，见
    // ShadowPcfConfig::GoldenSpiralOffsets —— 公式真值在那边）。只算一次、两张 shader
    // 共用；shader 里仅剩「查表 + 乘加」，无 per-tap sqrt/除法/cos/sin。
    CHECK_GE(pcf.tap_count, 1) << "ShadowPcfConfig::tap_count 必须 ≥1";
    CHECK_LE(pcf.tap_count, ShadowPcfConfig::kMaxPcfTaps)
        << "ShadowPcfConfig::tap_count 至多 " << ShadowPcfConfig::kMaxPcfTaps;
    CHECK_GT(pcf.radius_texels, 0.0f) << "ShadowPcfConfig::radius_texels 必须 >0";
    float pcf_offsets[2 * ShadowPcfConfig::kMaxPcfTaps];
    pcf.GoldenSpiralOffsets(pcf_offsets);
    const float pcf_inv_tap_count = 1.0f / static_cast<float>(pcf.tap_count);

    const unsigned int progs[2] = {prog, prog_full};
    for (int pidx = 0; pidx < 2; ++pidx) {
        unsigned int p = progs[pidx];
        glUseProgram(p);
        glUniform1i(shader_mgr.GetUniform(p, "uHasSun"), sun.has_value() ? 1 : 0);
        glUniform1i(shader_mgr.GetUniform(p, "uCascadeCount"), cascade_count);
        if (!sun.has_value()) {
            // 无太阳：仅置 uHasSun=0 / uCascadeCount=0（防止上一帧残留直射光）。
            continue;
        }
        glUniform3f(shader_mgr.GetUniform(p, "uSunDir"),
                    sun->direction.x(), sun->direction.y(), sun->direction.z());
        glUniform3f(shader_mgr.GetUniform(p, "uSunColor"),
                    sun->color.r, sun->color.g, sun->color.b);
        glUniform1f(shader_mgr.GetUniform(p, "uSunIntensity"), sun->intensity);

        // 各级联 far 距离 + 阴影淡出范围。
        CHECK_LE(cascade_count, ShadowConfig::kMaxCascades);
        glUniform1fv(shader_mgr.GetUniform(p, "uCascadeRanges"),
                     cascade_count, cfg.cascade_ranges);
        glUniform1f(shader_mgr.GetUniform(p, "uCascadeBlendFraction"),
                    cfg.cascade_blend_fraction);
        glUniform1f(shader_mgr.GetUniform(p, "uShadowFadeStart"), cfg.fade_start);
        glUniform1f(shader_mgr.GetUniform(p, "uShadowFadeEnd"), cfg.fade_end);

        // PCF 采样核（每帧可切；见 ShadowPcfConfig）：模式/点数/半径 + CPU 预算的采点表。
        glUniform1i(shader_mgr.GetUniform(p, "uPcfMode"),
                    static_cast<int>(pcf.mode));
        glUniform1i(shader_mgr.GetUniform(p, "uPcfTapCount"), pcf.tap_count);
        glUniform1f(shader_mgr.GetUniform(p, "uPcfRadiusTexels"), pcf.radius_texels);
        glUniform1f(shader_mgr.GetUniform(p, "uPcfInvTapCount"), pcf_inv_tap_count);
        // ⚠️ 逐元素 glUniform2f 而非 glUniform2fv：Windows 的 MinGW 扩展加载器
        //   （third_party/gl_loader-mingw）未声明 glUniform2fv（同 uAmbientTricolor 的处理）。
        for (int i = 0; i < pcf.tap_count; ++i) {
            const std::string uni = "uPcfOffsets[" + std::to_string(i) + "]";
            glUniform2f(shader_mgr.GetUniform(p, uni.c_str()),
                        pcf_offsets[2 * i], pcf_offsets[2 * i + 1]);
        }

        // 绑各级联 shadow 深度纹理到 TEXTURE(7+i)，上传对应 ViewProj + texel。
        CHECK_EQ(shadow_fbos.size(), static_cast<size_t>(cascade_count))
            << "UploadSunData: shadow_fbos.size() 与 cascade_count 不一致";
        // 深度偏置：自动 = shader 用单纹素世界边长几何推导；override = 手工等效边长。
        // 上传全部 kMaxCascades 位（未用位取 0 或默认，shader 只索引实际级联）。
        // 见 ShadowConfig::cascade_bias 的公式与推导。
        static constexpr int kMaxC = jpov::ShadowConfig::kMaxCascades;
        float bias[kMaxC] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
        for (int c = 0; c < kMaxC; ++c) bias[c] = cfg.cascade_bias[c];
        glUniform1fv(shader_mgr.GetUniform(p, "uShadowBiasCascade"),
                     kMaxC, bias);
        glUniform1i(shader_mgr.GetUniform(p, "uShadowBiasOverride"),
                    cfg.override_cascade_bias ? 1 : 0);
        glUniform1fv(shader_mgr.GetUniform(p, "uShadowTexelWorld"),
                     kMaxC, shadow_texel_world);
        for (int c = 0; c < cascade_count; ++c) {
            const unsigned int unit =
                static_cast<unsigned int>(kTexUnitShadowMapBase) + static_cast<unsigned int>(c);
            glActiveTexture(GL_TEXTURE0 + unit);
            glBindTexture(GL_TEXTURE_2D, shadow_fbos[c].tex);
            // 纹理单元名 uShadowMap[i]（数组 sampler uniform）。
            std::string uni = "uShadowMap[" + std::to_string(c) + "]";
            glUniform1i(shader_mgr.GetUniform(p, uni.c_str()), static_cast<int>(unit));
            uni = "uShadowVP[" + std::to_string(c) + "]";
            glUniformMatrix4fv(shader_mgr.GetUniform(p, uni.c_str()),
                               1, GL_FALSE, shadow_vp[c]);
            uni = "uShadowDepthVP[" + std::to_string(c) + "]";
            glUniformMatrix4fv(shader_mgr.GetUniform(p, uni.c_str()),
                               1, GL_FALSE, shadow_depth_vp[c]);
            uni = "uShadowTexel[" + std::to_string(c) + "]";
            glUniform1f(shader_mgr.GetUniform(p, uni.c_str()),
                        1.0f / static_cast<float>(shadow_fbos[c].size));
        }
        glActiveTexture(GL_TEXTURE0);
    }
}

// ==================== DrawInstancedObject ====================

void InstancedObjectRenderer::DrawInstancedObject(const InstancedObjectCommand& cmd,
                                                   const RenderCommandList& cmds,
                                                   MeshManager& mesh_mgr,
                                                   TextureManager& texture_mgr,
                                                   ShaderManager& shader_mgr,
                                                   const float view_proj[16],
                                                   unsigned int prog,
                                                   unsigned int prog_full,
                                                   unsigned int prog_cutout,
                                                   unsigned int prog_full_cutout,
                                                   unsigned int tile_index_tex,
                                                   InstanceBuffer& instance_model_buf) {
    const GPUMesh* mesh = mesh_mgr.GetMesh(cmd.mesh_id);
    CHECK(mesh != nullptr) << "DrawInstancedObject: mesh_id " << cmd.mesh_id
                           << " 未注册（DrawInstancedObject 前需先 RegisterMesh）";
    CHECK_GT(mesh->vao, 0u);
    CHECK(!cmd.instances.empty()) << "DrawInstancedObject: instances 不能为空";

    const bool any_tex =
        (cmd.material.base_color_tex != 0) ||
        (cmd.material.has_metallic_tex && cmd.material.metallic_tex != 0) ||
        (cmd.material.has_roughness_tex && cmd.material.roughness_tex != 0) ||
        (cmd.material.emissive_tex != 0) ||
        (cmd.material.ao_tex != 0) ||
        (cmd.material.normal_tex != 0);
    const bool use_normal_map = (cmd.material.normal_tex != 0);

    CHECK(MeshHasFlag(mesh->flags, MeshVertexFlags::kNormal))
        << "DrawInstancedObject: mesh_id=" << cmd.mesh_id << " 需要 kNormal 属性";

    glPushAttrib(GL_ENABLE_BIT);

    // 双面渲染（同 DrawObject3D）：材质声明 double_sided 时关背面剔除 + FS 翻法线。
    if (cmd.material.double_sided) {
        glDisable(GL_CULL_FACE);
    }

    // 选 program：有纹理走 full 变体，alpha_mode==kMask 走 cutout 变体（同 DrawObject3D）。
    const bool cutout = (cmd.material.alpha_mode == AlphaMode::kMask);
    unsigned int selected_prog;
    if (cutout) {
        selected_prog = any_tex ? prog_full_cutout : prog_cutout;
    } else {
        selected_prog = any_tex ? prog_full : prog;
    }
    glUseProgram(selected_prog);

    if (cutout) {
        glUniform1f(glGetUniformLocation(selected_prog, "uAlphaCutoff"),
                    cmd.material.alpha_cutoff);
    }

    // 摆放走 per-instance attribute（loc6..9），故只传 VP（= proj*view，不含 model）。
    glUniformMatrix4fv(glGetUniformLocation(selected_prog, "uViewProj"),
                       1, GL_FALSE, view_proj);
    glUniform3f(glGetUniformLocation(selected_prog, "uBaseColor"),
                cmd.material.base_color.r, cmd.material.base_color.g,
                cmd.material.base_color.b);
    glUniform1f(glGetUniformLocation(selected_prog, "uMetallic"), cmd.material.metallic);
    glUniform1f(glGetUniformLocation(selected_prog, "uRoughness"), cmd.material.roughness);
    glUniform3f(glGetUniformLocation(selected_prog, "uEmissive"),
                cmd.material.emissive.r, cmd.material.emissive.g,
                cmd.material.emissive.b);
    glUniform1f(glGetUniformLocation(selected_prog, "uAO"), cmd.material.ao.r);
    glUniform3f(glGetUniformLocation(selected_prog, "uCameraPos"),
                cmds.camera.position.x(), cmds.camera.position.y(),
                cmds.camera.position.z());
    glUniform1f(glGetUniformLocation(selected_prog, "uCameraNear"),
                cmds.camera.near);

    if (any_tex) {
        CHECK(MeshHasFlag(mesh->flags, MeshVertexFlags::kUV))
            << "DrawInstancedObject: 材质通道带纹理但 mesh 无 kUV 属性，无法纹理采样";
    }
    if (use_normal_map) {
        CHECK(MeshHasFlag(mesh->flags, MeshVertexFlags::kTangent))
            << "DrawInstancedObject: normal_tex 非 0 但 mesh 无 kTangent 属性，"
            << "无法构建 TBN";
    }

    // ---- 材质纹理绑定（同 DrawObject3D：baseColor1/metallic2/roughness3/emissive4/ao5/normal6）----
    if (cmd.material.base_color_tex != 0) {
        unsigned int gl_tex = texture_mgr.GetGLTexture(cmd.material.base_color_tex);
        CHECK_NE(gl_tex, 0u) << "DrawInstancedObject: base_color_tex "
                             << cmd.material.base_color_tex << " 未注册";
        glActiveTexture(GL_TEXTURE0 + kTexUnitMaterialBase + 0);
        glBindTexture(GL_TEXTURE_2D, gl_tex);
        glUniform1i(glGetUniformLocation(selected_prog, "uBaseColorTex"), 1);
        glUniform1i(glGetUniformLocation(selected_prog, "uHasBaseColorTex"), 1);
    } else {
        glUniform1i(glGetUniformLocation(selected_prog, "uHasBaseColorTex"), 0);
    }
    if (cmd.material.has_metallic_tex && cmd.material.metallic_tex != 0) {
        unsigned int gl_tex = texture_mgr.GetGLTexture(cmd.material.metallic_tex);
        CHECK_NE(gl_tex, 0u) << "DrawInstancedObject: metallic_tex 未注册";
        glActiveTexture(GL_TEXTURE0 + kTexUnitMaterialBase + 1);
        glBindTexture(GL_TEXTURE_2D, gl_tex);
        glUniform1i(glGetUniformLocation(selected_prog, "uMetallicTex"), 2);
        glUniform1i(glGetUniformLocation(selected_prog, "uHasMetallicTex"), 1);
    } else {
        glUniform1i(glGetUniformLocation(selected_prog, "uHasMetallicTex"), 0);
    }
    if (cmd.material.has_roughness_tex && cmd.material.roughness_tex != 0) {
        unsigned int gl_tex = texture_mgr.GetGLTexture(cmd.material.roughness_tex);
        CHECK_NE(gl_tex, 0u) << "DrawInstancedObject: roughness_tex 未注册";
        glActiveTexture(GL_TEXTURE0 + kTexUnitMaterialBase + 2);
        glBindTexture(GL_TEXTURE_2D, gl_tex);
        glUniform1i(glGetUniformLocation(selected_prog, "uRoughnessTex"), 3);
        glUniform1i(glGetUniformLocation(selected_prog, "uHasRoughnessTex"), 1);
    } else {
        glUniform1i(glGetUniformLocation(selected_prog, "uHasRoughnessTex"), 0);
    }
    if (cmd.material.emissive_tex != 0) {
        unsigned int gl_tex = texture_mgr.GetGLTexture(cmd.material.emissive_tex);
        CHECK_NE(gl_tex, 0u) << "DrawInstancedObject: emissive_tex 未注册";
        glActiveTexture(GL_TEXTURE0 + kTexUnitMaterialBase + 3);
        glBindTexture(GL_TEXTURE_2D, gl_tex);
        glUniform1i(glGetUniformLocation(selected_prog, "uEmissiveTex"), 4);
        glUniform1i(glGetUniformLocation(selected_prog, "uHasEmissiveTex"), 1);
    } else {
        glUniform1i(glGetUniformLocation(selected_prog, "uHasEmissiveTex"), 0);
    }
    if (cmd.material.ao_tex != 0) {
        unsigned int gl_tex = texture_mgr.GetGLTexture(cmd.material.ao_tex);
        CHECK_NE(gl_tex, 0u) << "DrawInstancedObject: ao_tex 未注册";
        glActiveTexture(GL_TEXTURE0 + kTexUnitMaterialBase + 4);
        glBindTexture(GL_TEXTURE_2D, gl_tex);
        glUniform1i(glGetUniformLocation(selected_prog, "uAoTex"), 5);
        glUniform1i(glGetUniformLocation(selected_prog, "uHasAoTex"), 1);
    } else {
        glUniform1i(glGetUniformLocation(selected_prog, "uHasAoTex"), 0);
    }
    if (cmd.material.normal_tex != 0) {
        unsigned int gl_tex = texture_mgr.GetGLTexture(cmd.material.normal_tex);
        CHECK_NE(gl_tex, 0u) << "DrawInstancedObject: normal_tex 未注册";
        glActiveTexture(GL_TEXTURE0 + kTexUnitMaterialBase + 5);
        glBindTexture(GL_TEXTURE_2D, gl_tex);
        glUniform1i(glGetUniformLocation(selected_prog, "uNormalTex"), 6);
        glUniform1i(glGetUniformLocation(selected_prog, "uHasNormalTex"), 1);
        glUniform1f(glGetUniformLocation(selected_prog, "uNormalScale"),
                    cmd.material.normal_scale);
    } else {
        glUniform1i(glGetUniformLocation(selected_prog, "uHasNormalTex"), 0);
        glUniform1f(glGetUniformLocation(selected_prog, "uNormalScale"), 1.0f);
    }

    glActiveTexture(GL_TEXTURE0 + kTexUnitTileLightIndex);
    glBindTexture(GL_TEXTURE_2D, tile_index_tex);
    glUniform1i(glGetUniformLocation(selected_prog, "uTileLightIndices"), kTexUnitTileLightIndex);

    // ---- 每实例摆放矩阵（与 DrawObject3D 同一套 BuildModelMatrix）----
    const size_t n = cmd.instances.size();
    std::vector<float> xforms(n * 16);
    for (size_t k = 0; k < n; ++k) {
        float model[16];
        const InstanceTransform& t = cmd.instances[k].transform;
        BuildModelMatrix(t.center, t.up, t.front, t.scale, model);
        for (int e = 0; e < 16; ++e) {
            xforms[k * 16 + static_cast<size_t>(e)] = model[e];
        }
    }
    instance_model_buf.Upload(xforms);

    const GLsizei n_inst = static_cast<GLsizei>(n);
    {
        // RAII 配对挂载 per-instance 矩阵（loc6..9，divisor=1）。
        InstanceBufferBinding bind(mesh->vao, {&instance_model_buf});
        glBindVertexArray(mesh->vao);
        if (mesh->index_count > 0) {
            glDrawElementsInstanced(GL_TRIANGLES,
                                    static_cast<GLsizei>(mesh->index_count),
                                    GL_UNSIGNED_INT, nullptr, n_inst);
        } else {
            glDrawArraysInstanced(GL_TRIANGLES, 0,
                                  static_cast<GLsizei>(mesh->vertex_count), n_inst);
        }
        glBindVertexArray(0);
    }

    GLenum draw_err = glGetError();
    if (draw_err != GL_NO_ERROR) {
        LOG_FIRST_N(WARNING, 1) << "GL error after DrawInstancedObject: " << draw_err;
    }

    glPopAttrib();
}

// ==================== DrawInstancedObjectShadow ====================

void InstancedObjectRenderer::DrawInstancedObjectShadow(const InstancedObjectCommand& cmd,
                                                         MeshManager& mesh_mgr,
                                                         TextureManager& texture_mgr,
                                                         ShaderManager& shader_mgr,
                                                         const float shadow_vp[16],
                                                         const float depth_vp[16],
                                                         unsigned int shadow_prog,
                                                         unsigned int shadow_prog_cutout,
                                                         InstanceBuffer& instance_model_buf) {
    const GPUMesh* mesh = mesh_mgr.GetMesh(cmd.mesh_id);
    CHECK(mesh != nullptr) << "DrawInstancedObjectShadow: mesh_id " << cmd.mesh_id
                           << " 未注册";
    CHECK_GT(mesh->vao, 0u);
    CHECK(!cmd.instances.empty()) << "DrawInstancedObjectShadow: instances 不能为空";

    // 双面（同 DrawObject3DShadow）：材质声明 double_sided 时关背面剔除，使薄片两面都投影。
    if (cmd.material.double_sided) {
        glDisable(GL_CULL_FACE);
    } else {
        glEnable(GL_CULL_FACE);
    }

    const bool cutout = (cmd.material.alpha_mode == AlphaMode::kMask);
    const unsigned int sp = cutout ? shadow_prog_cutout : shadow_prog;
    glUseProgram(sp);

    if (cutout) {
        glUniform1f(glGetUniformLocation(sp, "uAlphaCutoff"),
                    cmd.material.alpha_cutoff);
        if (cmd.material.base_color_tex != 0) {
            CHECK(MeshHasFlag(mesh->flags, MeshVertexFlags::kUV))
                << "DrawInstancedObjectShadow: cutout 且 base_color_tex 非 0 但 mesh 无 kUV"
                << "，无法采样 alpha（mesh_id=" << cmd.mesh_id << "）";
            unsigned int gl_tex = texture_mgr.GetGLTexture(cmd.material.base_color_tex);
            CHECK_NE(gl_tex, 0u) << "DrawInstancedObjectShadow: base_color_tex 未注册";
            const int u = kTexUnitMaterialBase + 0;
            glActiveTexture(GL_TEXTURE0 + u);
            glBindTexture(GL_TEXTURE_2D, gl_tex);
            glUniform1i(glGetUniformLocation(sp, "uBaseColorTex"), u);
            glUniform1i(glGetUniformLocation(sp, "uHasBaseColorTex"), 1);
        } else {
            glUniform1i(glGetUniformLocation(sp, "uHasBaseColorTex"), 0);
        }
    }

    // 摆放走 per-instance attribute，故只传光空间 VP（不含 model）。
    glUniformMatrix4fv(glGetUniformLocation(sp, "uShadowViewProj"),
                       1, GL_FALSE, shadow_vp);
    glUniformMatrix4fv(glGetUniformLocation(sp, "uShadowDepthViewProj"),
                       1, GL_FALSE, depth_vp);

    const size_t n = cmd.instances.size();
    std::vector<float> xforms(n * 16);
    for (size_t k = 0; k < n; ++k) {
        float model[16];
        const InstanceTransform& t = cmd.instances[k].transform;
        BuildModelMatrix(t.center, t.up, t.front, t.scale, model);
        for (int e = 0; e < 16; ++e) {
            xforms[k * 16 + static_cast<size_t>(e)] = model[e];
        }
    }
    instance_model_buf.Upload(xforms);

    const GLsizei n_inst = static_cast<GLsizei>(n);
    {
        InstanceBufferBinding bind(mesh->vao, {&instance_model_buf});
        glBindVertexArray(mesh->vao);
        if (mesh->index_count > 0) {
            glDrawElementsInstanced(GL_TRIANGLES,
                                    static_cast<GLsizei>(mesh->index_count),
                                    GL_UNSIGNED_INT, nullptr, n_inst);
        } else {
            glDrawArraysInstanced(GL_TRIANGLES, 0,
                                  static_cast<GLsizei>(mesh->vertex_count), n_inst);
        }
        glBindVertexArray(0);
    }
}

// ==================== DrawInstancedObjectForPick ====================

void InstancedObjectRenderer::DrawInstancedObjectForPick(const InstancedObjectCommand& cmd,
                                                         MeshManager& mesh_mgr,
                                                         TextureManager& texture_mgr,
                                                         const float view_proj[16],
                                                         unsigned int prog,
                                                         unsigned int prog_cutout,
                                                         std::vector<uint32_t>* pick_id_map,
                                                         InstanceBuffer& instance_model_buf) {
    const GPUMesh* mesh = mesh_mgr.GetMesh(cmd.mesh_id);
    CHECK(mesh != nullptr) << "DrawInstancedObjectForPick: mesh_id " << cmd.mesh_id << " 未注册";
    CHECK_GT(mesh->vao, 0u);
    CHECK(!cmd.instances.empty()) << "DrawInstancedObjectForPick: instances 不能为空";

    const bool cutout = (cmd.material.alpha_mode == AlphaMode::kMask);
    const unsigned int sp = cutout ? prog_cutout : prog;
    glUseProgram(sp);

    glPushAttrib(GL_ENABLE_BIT);
    if (cmd.material.double_sided) {
        glDisable(GL_CULL_FACE);
    }

    glUniformMatrix4fv(glGetUniformLocation(sp, "uViewProj"), 1, GL_FALSE, view_proj);
    // pick 模式：本命令占用的 internal id：base .. base+n-1（逐实例 = base + gl_InstanceID）。
    // highlight 模式（map==nullptr）：不需要 id，跳过。
    const size_t n = cmd.instances.size();
    if (pick_id_map != nullptr) {
        const uint32_t base = kPickIdBaseInstanced + static_cast<uint32_t>(pick_id_map->size());
        CHECK_LE(static_cast<uint64_t>(base) + n, static_cast<uint64_t>(kPickIdMax))
            << "DrawInstancedObjectForPick: instanced 段 id 溢出（>= " << kPickIdSegmentSize << "）";
        glUniform1i(glGetUniformLocation(sp, "uPickIdBase"), static_cast<int>(base));
    }

    if (cutout) {
        glUniform1f(glGetUniformLocation(sp, "uAlphaCutoff"), cmd.material.alpha_cutoff);
        if (cmd.material.base_color_tex != 0) {
            CHECK(MeshHasFlag(mesh->flags, MeshVertexFlags::kUV))
                << "DrawInstancedObjectForPick: cutout 且 base_color_tex 非 0 但 mesh 无 kUV";
            unsigned int gl_tex = texture_mgr.GetGLTexture(cmd.material.base_color_tex);
            CHECK_NE(gl_tex, 0u) << "DrawInstancedObjectForPick: base_color_tex 未注册";
            const int u = kTexUnitMaterialBase + 0;
            glActiveTexture(GL_TEXTURE0 + u);
            glBindTexture(GL_TEXTURE_2D, gl_tex);
            glUniform1i(glGetUniformLocation(sp, "uBaseColorTex"), u);
            glUniform1i(glGetUniformLocation(sp, "uHasBaseColorTex"), 1);
        } else {
            glUniform1i(glGetUniformLocation(sp, "uHasBaseColorTex"), 0);
        }
    }

    // pick 模式：CPU 侧 id 映射，逐实例追加（含 picking_id==0，保证 base+i 连续）。
    if (pick_id_map != nullptr) {
        for (const InstanceState& inst : cmd.instances) {
            pick_id_map->push_back(inst.picking_id);
        }
    }

    std::vector<float> xforms(n * 16);
    for (size_t k = 0; k < n; ++k) {
        float model[16];
        const InstanceTransform& t = cmd.instances[k].transform;
        BuildModelMatrix(t.center, t.up, t.front, t.scale, model);
        for (int e = 0; e < 16; ++e) {
            xforms[k * 16 + static_cast<size_t>(e)] = model[e];
        }
    }
    instance_model_buf.Upload(xforms);

    const GLsizei n_inst = static_cast<GLsizei>(n);
    {
        InstanceBufferBinding bind(mesh->vao, {&instance_model_buf});
        glBindVertexArray(mesh->vao);
        if (mesh->index_count > 0) {
            glDrawElementsInstanced(GL_TRIANGLES,
                                    static_cast<GLsizei>(mesh->index_count),
                                    GL_UNSIGNED_INT, nullptr, n_inst);
        } else {
            glDrawArraysInstanced(GL_TRIANGLES, 0,
                                  static_cast<GLsizei>(mesh->vertex_count), n_inst);
        }
        glBindVertexArray(0);
    }
    glActiveTexture(GL_TEXTURE0);
    glPopAttrib();
}

}  // namespace jpov
