// JPOV SkeletonRenderer 实现
//
// 蒙皮带骨实例的 PBR 渲染（pose atlas 查表 + 逐实例 draw）+ 蒙皮阴影深度绘制。

#define GL_GLEXT_PROTOTYPES

#include "tools/jpov/src/skeleton/skeleton_renderer.h"

#include <algorithm>
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

// 4x4 矩阵乘法：out = a * b（列主序）
void Mat4Mul(const float a[16], const float b[16], float out[16]) {
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                sum += a[k * 4 + row] * b[col * 4 + k];
            }
            out[col * 4 + row] = sum;
        }
    }
}

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

// ==================== UploadAmbient ====================

void SkeletonRenderer::UploadAmbient(ShaderManager& shader_mgr,
                                     unsigned int prog,
                                     const AmbientLight& ambient) {
    glUseProgram(prog);
    glUniform3f(shader_mgr.GetUniform(prog, "uAmbientColor"),
                ambient.color.r, ambient.color.g, ambient.color.b);
    glUniform1f(shader_mgr.GetUniform(prog, "uAmbientIntensity"),
                std::max(ambient.intensity, 0.0f));  // 负值 clamp 到 0
}

// ==================== DrawSkinnedMesh ====================
// 一批带骨实例的蒙皮 PBR 渲染：绑材质纹理 + pose atlas，逐实例设
// uMVP(=proj*view*model)/uModel/uPoseRow/uPoseCol 后 draw。
// 材质纹理绑定放实例循环外（同一命令内所有实例共用材质）。
// 光照（太阳/环境光）由调用方在 Render() 主流程经 UploadSunData/UploadAmbient 预置。
void SkeletonRenderer::DrawSkinnedMesh(
    const SkinnedMeshCommand& cmd,
    const RenderCommandList& cmds,
    MeshManager& mesh_mgr,
    TextureManager& texture_mgr,
    ShaderManager& shader_mgr,
    const float mvp[16],
    unsigned int skinned_prog,
    const SkeletonManager::GpuHandles& gh) {
    const GPUMesh* mesh = mesh_mgr.GetMesh(cmd.mesh_id);
    CHECK(mesh != nullptr) << "DrawSkinnedMesh: mesh_id "
                           << cmd.mesh_id << " 未注册";
    CHECK_GT(mesh->vao, 0u);
    CHECK(MeshHasFlag(mesh->flags, MeshVertexFlags::kNormal))
        << "DrawSkinnedMesh: mesh_id=" << cmd.mesh_id
        << " 需要 kNormal 属性";
    CHECK(MeshHasFlag(mesh->flags, MeshVertexFlags::kJoints))
        << "DrawSkinnedMesh: mesh_id=" << cmd.mesh_id
        << " 需要 kJoints 属性（蒙皮网格）";
    CHECK(!cmd.instances.empty()) << "SkinnedMesh instance 数组不能为空";

    const bool any_tex =
        (cmd.material.base_color_tex != 0) ||
        (cmd.material.has_metallic_tex && cmd.material.metallic_tex != 0) ||
        (cmd.material.has_roughness_tex && cmd.material.roughness_tex != 0) ||
        (cmd.material.emissive_tex != 0) ||
        (cmd.material.ao_tex != 0) ||
        (cmd.material.normal_tex != 0);
    const bool use_normal_map = (cmd.material.normal_tex != 0);

    glPushAttrib(GL_ENABLE_BIT);
    glUseProgram(skinned_prog);
    unsigned int sp = skinned_prog;

    // 统一 baseColor / 法线等材质 uniform（与 DrawObject3D 一致）。
    glUniform3f(glGetUniformLocation(sp, "uBaseColor"),
                cmd.material.base_color.r, cmd.material.base_color.g,
                cmd.material.base_color.b);
    glUniform1f(glGetUniformLocation(sp, "uMetallic"), cmd.material.metallic);
    glUniform1f(glGetUniformLocation(sp, "uRoughness"), cmd.material.roughness);
    glUniform3f(glGetUniformLocation(sp, "uEmissive"),
                cmd.material.emissive.r, cmd.material.emissive.g,
                cmd.material.emissive.b);
    glUniform1f(glGetUniformLocation(sp, "uAO"), cmd.material.ao.r);
    glUniform1i(glGetUniformLocation(sp, "uHasBaseColorTex"), 0);
    glUniform1i(glGetUniformLocation(sp, "uHasMetallicTex"), 0);
    glUniform1i(glGetUniformLocation(sp, "uHasRoughnessTex"), 0);
    glUniform1i(glGetUniformLocation(sp, "uHasEmissiveTex"), 0);
    glUniform1i(glGetUniformLocation(sp, "uHasAoTex"), 0);
    glUniform1i(glGetUniformLocation(sp, "uHasNormalTex"), 0);
    glUniform1f(glGetUniformLocation(sp, "uNormalScale"), 1.0f);
    glUniform3f(glGetUniformLocation(sp, "uCameraPos"),
                cmds.camera.position.x(), cmds.camera.position.y(),
                cmds.camera.position.z());
    glUniform1f(glGetUniformLocation(sp, "uCameraNear"), cmds.camera.near);

    if (any_tex) {
        CHECK(MeshHasFlag(mesh->flags, MeshVertexFlags::kUV))
            << "蒙皮材质带纹理但 mesh 无 kUV，无法采样";
    }
    if (use_normal_map) {
        CHECK(MeshHasFlag(mesh->flags, MeshVertexFlags::kTangent))
            << "蒙皮 normal_tex 非 0 但 mesh 无 kTangent";
    }

    // ---- 材质纹理绑定（同 DrawObject3D：baseColor1/metallic2/roughness3/emissive4/ao5/normal6）----
    if (cmd.material.base_color_tex != 0) {
        unsigned int gl_tex = texture_mgr.GetGLTexture(cmd.material.base_color_tex);
        CHECK_NE(gl_tex, 0u) << "蒙皮 base_color_tex 未注册";
        glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, gl_tex);
        glUniform1i(glGetUniformLocation(sp, "uBaseColorTex"), 1);
        glUniform1i(glGetUniformLocation(sp, "uHasBaseColorTex"), 1);
    }
    if (cmd.material.has_metallic_tex && cmd.material.metallic_tex != 0) {
        unsigned int gl_tex = texture_mgr.GetGLTexture(cmd.material.metallic_tex);
        CHECK_NE(gl_tex, 0u); glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, gl_tex);
        glUniform1i(glGetUniformLocation(sp, "uMetallicTex"), 2);
        glUniform1i(glGetUniformLocation(sp, "uHasMetallicTex"), 1);
    }
    if (cmd.material.has_roughness_tex && cmd.material.roughness_tex != 0) {
        unsigned int gl_tex = texture_mgr.GetGLTexture(cmd.material.roughness_tex);
        CHECK_NE(gl_tex, 0u); glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, gl_tex);
        glUniform1i(glGetUniformLocation(sp, "uRoughnessTex"), 3);
        glUniform1i(glGetUniformLocation(sp, "uHasRoughnessTex"), 1);
    }
    if (cmd.material.emissive_tex != 0) {
        unsigned int gl_tex = texture_mgr.GetGLTexture(cmd.material.emissive_tex);
        CHECK_NE(gl_tex, 0u); glActiveTexture(GL_TEXTURE4);
        glBindTexture(GL_TEXTURE_2D, gl_tex);
        glUniform1i(glGetUniformLocation(sp, "uEmissiveTex"), 4);
        glUniform1i(glGetUniformLocation(sp, "uHasEmissiveTex"), 1);
    }
    if (cmd.material.ao_tex != 0) {
        unsigned int gl_tex = texture_mgr.GetGLTexture(cmd.material.ao_tex);
        CHECK_NE(gl_tex, 0u); glActiveTexture(GL_TEXTURE5);
        glBindTexture(GL_TEXTURE_2D, gl_tex);
        glUniform1i(glGetUniformLocation(sp, "uAoTex"), 5);
        glUniform1i(glGetUniformLocation(sp, "uHasAoTex"), 1);
    }
    if (cmd.material.normal_tex != 0) {
        unsigned int gl_tex = texture_mgr.GetGLTexture(cmd.material.normal_tex);
        CHECK_NE(gl_tex, 0u); glActiveTexture(GL_TEXTURE6);
        glBindTexture(GL_TEXTURE_2D, gl_tex);
        glUniform1i(glGetUniformLocation(sp, "uNormalTex"), 6);
        glUniform1i(glGetUniformLocation(sp, "uHasNormalTex"), 1);
        glUniform1f(glGetUniformLocation(sp, "uNormalScale"), cmd.material.normal_scale);
    }

    // ---- 骨纹理 pose atlas（TEXTURE12；避开 0=tile/1-6=材质/7-11=shadow 5 级联）----
    glActiveTexture(GL_TEXTURE12);
    glBindTexture(GL_TEXTURE_2D, gh.pose_atlas_tex);
    glUniform1i(glGetUniformLocation(sp, "uPoseAtlas"), 12);
    glUniform1i(glGetUniformLocation(sp, "uBoneCount"), gh.bone_count);

    // ---- 逐实例：uMVP(=proj*view*model)/uModel/uPose 后 draw ----
    for (const SkinnedInstanceState& inst : cmd.instances) {
        float model[16], mvp_final[16];
        BuildModelMatrix(inst.center, inst.up, inst.front, inst.scale, model);
        Mat4Mul(mvp, model, mvp_final);
        glUniformMatrix4fv(glGetUniformLocation(sp, "uMVP"), 1, GL_FALSE, mvp_final);
        glUniformMatrix4fv(glGetUniformLocation(sp, "uModel"), 1, GL_FALSE, model);

        // M1 单 pose 静态(pose_a==pose_b)：取 pose_a 行/列。动态(pose_a/pose_b+ratio)后续。
        const int ppi = gh.pose_per_row;
        const int pa = inst.pose_a;
        glUniform1i(glGetUniformLocation(sp, "uPoseRow"), pa / ppi);
        glUniform1i(glGetUniformLocation(sp, "uPoseCol"), pa % ppi);

        glBindVertexArray(mesh->vao);
        if (mesh->index_count > 0) {
            glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(mesh->index_count),
                           GL_UNSIGNED_INT, nullptr);
        } else {
            glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(mesh->vertex_count));
        }
        glBindVertexArray(0);
    }

    GLenum draw_err = glGetError();
    if (draw_err != GL_NO_ERROR) {
        LOG_FIRST_N(WARNING, 1) << "GL error after DrawSkinnedMesh: "
                                << draw_err;
    }
    glPopAttrib();
}

// ==================== UploadSunData ====================

void SkeletonRenderer::UploadSunData(
    ShaderManager& shader_mgr,
    unsigned int prog,
    const std::vector<CascadeFBO>& shadow_fbos,
    const float shadow_vp[][16],
    const float shadow_depth_vp[][16],
    const ShadowConfig& cfg,
    const std::optional<DirectionalLight>& sun) {
    const int cascade_count = sun.has_value() ? cfg.cascade_count : 0;

    glUseProgram(prog);
    glUniform1i(shader_mgr.GetUniform(prog, "uHasSun"), sun.has_value() ? 1 : 0);
    glUniform1i(shader_mgr.GetUniform(prog, "uCascadeCount"), cascade_count);
    if (!sun.has_value()) {
        // 无太阳：仅置 uHasSun=0 / uCascadeCount=0（防止上一帧残留直射光）。
        return;
    }
    glUniform3f(shader_mgr.GetUniform(prog, "uSunDir"),
                sun->direction.x(), sun->direction.y(), sun->direction.z());
    glUniform3f(shader_mgr.GetUniform(prog, "uSunColor"),
                sun->color.r, sun->color.g, sun->color.b);
    glUniform1f(shader_mgr.GetUniform(prog, "uSunIntensity"), sun->intensity);

    // 各级联 far 距离 + 阴影淡出范围。
    CHECK_LE(cascade_count, ShadowConfig::kMaxCascades);
    glUniform1fv(shader_mgr.GetUniform(prog, "uCascadeRanges"),
                 cascade_count, cfg.cascade_ranges);
    glUniform1f(shader_mgr.GetUniform(prog, "uShadowFadeStart"), cfg.fade_start);
    glUniform1f(shader_mgr.GetUniform(prog, "uShadowFadeEnd"), cfg.fade_end);

    // 绑各级联 shadow 深度纹理到 TEXTURE(7+i)，上传对应 ViewProj + texel。
    CHECK_EQ(shadow_fbos.size(), static_cast<size_t>(cascade_count))
        << "UploadSunData: shadow_fbos.size() 与 cascade_count 不一致";
    // 每级联独立深度偏置（米，来自 ShadowConfig::cascade_bias）。
    // 上传全部 kMaxCascades 位（未用位取 0 或默认，shader 只索引实际级联）。
    static constexpr int kMaxC = jpov::ShadowConfig::kMaxCascades;
    float bias[kMaxC] = {0.004f, 0.004f, 0.004f, 0.004f, 0.004f};
    for (int c = 0; c < kMaxC; ++c) bias[c] = cfg.cascade_bias[c];
    glUniform1fv(shader_mgr.GetUniform(prog, "uShadowBiasCascade"), kMaxC, bias);
    for (int c = 0; c < cascade_count; ++c) {
        const unsigned int unit = 7u + static_cast<unsigned int>(c);
        glActiveTexture(GL_TEXTURE0 + unit);
        glBindTexture(GL_TEXTURE_2D, shadow_fbos[c].tex);
        // 纹理单元名 uShadowMap[i]（数组 sampler uniform）。
        std::string uni = "uShadowMap[" + std::to_string(c) + "]";
        glUniform1i(shader_mgr.GetUniform(prog, uni.c_str()), static_cast<int>(unit));
        uni = "uShadowVP[" + std::to_string(c) + "]";
        glUniformMatrix4fv(shader_mgr.GetUniform(prog, uni.c_str()),
                           1, GL_FALSE, shadow_vp[c]);
        uni = "uShadowDepthVP[" + std::to_string(c) + "]";
        glUniformMatrix4fv(shader_mgr.GetUniform(prog, uni.c_str()),
                           1, GL_FALSE, shadow_depth_vp[c]);
        uni = "uShadowTexel[" + std::to_string(c) + "]";
        glUniform1f(shader_mgr.GetUniform(prog, uni.c_str()),
                    1.0f / static_cast<float>(shadow_fbos[c].size));
    }
    glActiveTexture(GL_TEXTURE0);
}

// ==================== DrawSkinnedMeshShadow ====================
// 阴影 pass：把一批带骨实例从太阳正交光空间画进阴影纹理（只写线性深度 .r）。
// 蒙皮在 mesh 局部空间做（蒙皮 VS 内），再乘光空间 VP*model。
// pose atlas 绑到 TEXTURE7（阴影 pass 不与主 pass 的 TEXTURE12 冲突）。
void SkeletonRenderer::DrawSkinnedMeshShadow(
    const SkinnedMeshCommand& cmd,
    MeshManager& mesh_mgr,
    ShaderManager& shader_mgr,
    const SkeletonManager::GpuHandles& gh,
    const float shadow_vp[16],
    const float depth_vp[16],
    unsigned int shadow_prog) {
    const GPUMesh* mesh = mesh_mgr.GetMesh(cmd.mesh_id);
    CHECK(mesh != nullptr) << "DrawSkinnedMeshShadow: mesh_id " << cmd.mesh_id
                           << " 未注册";
    CHECK_GT(mesh->vao, 0u);
    CHECK(!cmd.instances.empty()) << "SkinnedMesh instance 数组不能为空";

    const int pose_per_row = gh.pose_per_row;

    glUseProgram(shadow_prog);
    glActiveTexture(GL_TEXTURE7);
    glBindTexture(GL_TEXTURE_2D, gh.pose_atlas_tex);
    glUniform1i(glGetUniformLocation(shadow_prog, "uPoseAtlas"), 7);
    glUniform1i(glGetUniformLocation(shadow_prog, "uBoneCount"), gh.bone_count);

    for (const SkinnedInstanceState& inst : cmd.instances) {
        float model[16];
        BuildModelMatrix(inst.center, inst.up, inst.front, inst.scale, model);
        // 光空间 MVP = 光VP × model。
        float sm[16], dm[16];
        Mat4Mul(shadow_vp, model, sm);
        Mat4Mul(depth_vp, model, dm);
        glUniformMatrix4fv(glGetUniformLocation(shadow_prog, "uShadowMVP"),
                           1, GL_FALSE, sm);
        glUniformMatrix4fv(glGetUniformLocation(shadow_prog, "uShadowDepthMVP"),
                           1, GL_FALSE, dm);

        const int row = inst.pose_a / pose_per_row;
        const int col = inst.pose_a % pose_per_row;
        glUniform1i(glGetUniformLocation(shadow_prog, "uPoseRow"), row);
        glUniform1i(glGetUniformLocation(shadow_prog, "uPoseCol"), col);

        glBindVertexArray(mesh->vao);
        if (mesh->index_count > 0) {
            glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(mesh->index_count),
                           GL_UNSIGNED_INT, nullptr);
        } else {
            glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(mesh->vertex_count));
        }
        glBindVertexArray(0);
    }
}

}  // namespace jpov
