// JPOV SkeletonRenderer 实现
//
// 蒙皮带骨实例的 PBR 渲染（pose atlas 查表 + 逐实例 draw）+ 蒙皮阴影深度绘制。

#define GL_GLEXT_PROTOTYPES

#include "tools/jpov/src/skeleton/skeleton_renderer.h"

#include "tools/jpov/src/texture_units.h"

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

// 编译期钉住 per-instance 粗细的布局约定（三处必须同步：本文件的上传循环、
// instance_buffer.h 的 kInstanceThicknessAttrSpec、skinning_shader.h 的 loc11/12）。
// 任一处单改都不会编译报错，而是**静默错位**（读到别人的槽位 / 每实例只传一半数据）
// ⇒ 在这里做交叉校验，把“三处同步”变成编译期强制。
static_assert(kNumThicknessGroup == 8, "部位粗细系数按 2 个 vec4 上传 ⇒ kNumThicknessGroup 必须是 8");
static_assert(kInstanceThicknessAttrSpec.slot_count *
                      kInstanceThicknessAttrSpec.slot_components ==
                  kNumThicknessGroup,
              "kInstanceThicknessAttrSpec 的 (slot_count × slot_components) 必须 == "
              "kNumThicknessGroup（每实例 8 个组系数）");
static_assert(kInstanceThicknessAttrSpec.stride_floats == kNumThicknessGroup,
              "kInstanceThicknessAttrSpec::stride_floats 必须 == 每实例上传的 float 数"
              "（= kNumThicknessGroup，见 UploadSkinningInstanceAttributes）");
static_assert(kInstanceThicknessAttrSpec.base_loc ==
                  kInstancePoseAttrSpec.base_loc + kInstancePoseAttrSpec.slot_count,
              "粗细系数的 base_loc 必须紧接 pose 选择槽（10 + 1 = 11）");
static_assert(kInstanceThicknessAttrSpec.base_loc + kInstanceThicknessAttrSpec.slot_count <= 16,
              "per-instance 槽位总数不得超过 GL_MAX_VERTEX_ATTRIBS 保证的 16");
// 部位额外旋转：三处同步（instance_buffer.h 的 spec / skinning_shader.h 的 loc13/14 / 本文上传循环）。
static_assert(kInstancePartialAttrSpec.slot_count * kInstancePartialAttrSpec.slot_components ==
                  kNumPartialRotation * 4,
              "kInstancePartialAttrSpec 的 (slot_count × slot_components) 必须 == "
              "kNumPartialRotation × 4（每实例 2 个四元数 = 8 float）");
static_assert(kInstancePartialAttrSpec.stride_floats == kNumPartialRotation * 4,
              "kInstancePartialAttrSpec::stride_floats 必须 == 每实例上传的 float 数"
              "（= kNumPartialRotation × 4，见 UploadSkinningInstanceAttributes）");
static_assert(kInstancePartialAttrSpec.base_loc ==
                  kInstanceThicknessAttrSpec.base_loc + kInstanceThicknessAttrSpec.slot_count,
              "额外旋转的 base_loc 必须紧接粗细系数槽（11 + 2 = 13）");
static_assert(kInstancePartialAttrSpec.base_loc + kInstancePartialAttrSpec.slot_count <= 16,
              "per-instance 槽位总数不得超过 GL_MAX_VERTEX_ATTRIBS 保证的 16");

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

// 本批实例里是否存在「非恒等」的部位额外旋转 —— 用于 host 侧开关 uPartialEnabled。
//   全恒等 + 骨架无通道 ⇒ 整段跳过（逐字节零回归，不靠“乘单位元”）。
bool AnyNonIdentityPartialRotation(const std::vector<SkinnedInstanceState>& instances) {
    for (const SkinnedInstanceState& inst : instances) {
        for (int c = 0; c < kNumPartialRotation; ++c) {
            const geom::Quaternion<float>& q = inst.partial_rotations[static_cast<size_t>(c)];
            if (q.x != 0.0f || q.y != 0.0f || q.z != 0.0f || q.w != 1.0f) {
                return true;
            }
        }
    }
    return false;
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

// ==================== UploadSkinningInstanceAttributes ====================
// 主 pass / shadow pass 共用的逐实例数据上传（摆放矩阵 + pose 选择），写进**渲染器持有
// 的实例缓冲**（不是 mesh 资源 —— 见 instance_buffer.h 顶部）。
// 两个 pass 必须用**同一套**逐实例数据，否则影子与身体错位。
void SkeletonRenderer::UploadSkinningInstanceAttributes(
    const SkinnedMeshCommand& cmd,
    int pose_w,
    InstanceBuffer& instance_model_buf,
    InstanceBuffer& instance_pose_buf,
    InstanceBuffer& instance_thickness_buf,
    InstanceBuffer& instance_partial_buf) {
    const size_t n = cmd.instances.size();
    CHECK_GT(n, 0u) << "UploadSkinningInstanceAttributes: instances 不能为空";

    // 1) 摆放矩阵：每实例一个列主序 mat4（与 DrawObject3D 同一套 BuildModelMatrix）。
    std::vector<float> xforms;
    xforms.resize(n * 16);
    for (size_t k = 0; k < n; ++k) {
        const InstanceTransform& t = cmd.instances[k].transform;
        float model[16];
        BuildModelMatrix(t.center, t.up, t.front, t.scale, model);
        for (int e = 0; e < 16; ++e) {
            xforms[k * 16 + static_cast<size_t>(e)] = model[e];
        }
    }
    instance_model_buf.Upload(xforms);

    // 2) pose 选择：每实例 [pose_col_a, pose_col_b, ratio]。
    //    col = pose_idx * pose_width（一个 pose 在 atlas 里的**平坦** texel 宽度
    //    = bone_count * 2：每骨 2 texel 存一个对偶四元数），
    //    shader 内按 atlas 宽度回绕成 (x,y)——与 CPU 行优先平铺逐 texel 对齐。
    std::vector<float> poses;
    poses.resize(n * 3);
    for (size_t k = 0; k < n; ++k) {
        const SkinnedInstanceState& inst = cmd.instances[k];
        poses[k * 3 + 0] = static_cast<float>(inst.pose_a * pose_w);
        poses[k * 3 + 1] = static_cast<float>(inst.pose_b * pose_w);
        poses[k * 3 + 2] = inst.ratio;
    }
    instance_pose_buf.Upload(poses);

    // 3) 部位粗细系数：每实例 kNumThicknessGroup 个 float（组号序，2×vec4 = loc11/12）。
    //    与 pose 选择同样是「这次 draw」的输入；骨架没配粗细时这里传的全是默认 1.0，
    //    蒙皮 VS 因 uThicknessEnabled=0 整段跳过（不产生任何计算）。
    std::vector<float> thickness(n * static_cast<size_t>(kNumThicknessGroup));
    for (size_t k = 0; k < n; ++k) {
        for (int g = 0; g < kNumThicknessGroup; ++g) {
            const float mu = cmd.instances[k].thickness_scales[static_cast<size_t>(g)];
            // 契约（skeleton_types.h 的 Pre-condition）：每项 > 0。0/负会把截面压成零面积 /
            //   翻法线；NaN 也被这一条拦下（NaN 的任何比较都是 false）。不 clamp 成
            //   “看起来还行”的值 —— 那是把用户的非法输入静默吞掉。
            CHECK_GT(mu, 0.0f) << "thickness_scales[" << g << "] 必须 > 0（系数为 0/负会把"
                                  "截面压成零面积/翻法线）—— 实例 " << k;
            // +inf 通过上面的 >0：它会算出 inf/NaN 顶点并污染整条管线（比 0 更隐蔽），
            //   所以有限性单独判一次（一次批内每实例 8 次浮点比较，可忽略）。
            CHECK(std::isfinite(mu)) << "thickness_scales[" << g << "] 必须是有限值，got " << mu
                                     << " —— 实例 " << k;
            thickness[k * static_cast<size_t>(kNumThicknessGroup) + static_cast<size_t>(g)] = mu;
        }
    }
    instance_thickness_buf.Upload(thickness);

    // 4) 部位额外旋转：每实例 kNumPartialRotation(=2) 个模型系四元数（xyzw，2×vec4 = loc13/14）。
    //    契约：每项为单位四元数（有限、非退化）。默认全恒等；非恒等时才由 host 开开关
    //    （见 DrawSkinnedMesh 的 uPartialEnabled），shader 侧才逐骨前乘。
    std::vector<float> partial(n * static_cast<size_t>(kNumPartialRotation) * 4);
    for (size_t k = 0; k < n; ++k) {
        for (int c = 0; c < kNumPartialRotation; ++c) {
            const geom::Quaternion<float>& q =
                cmd.instances[k].partial_rotations[static_cast<size_t>(c)];
            CHECK(std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) &&
                  std::isfinite(q.w))
                << "partial_rotations[" << c << "] 含非有限分量 —— 实例 " << k;
            // 范数应 ≈ 1（单位四元数）；范数近 0 会让 shader 的 normalize 出 NaN。
            const float norm2 = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
            CHECK_GT(norm2, 0.25f)
                << "partial_rotations[" << c << "] 范数过小（应为单位四元数）—— 实例 " << k;
            const size_t base =
                (k * static_cast<size_t>(kNumPartialRotation) + static_cast<size_t>(c)) * 4;
            partial[base + 0] = q.x;
            partial[base + 1] = q.y;
            partial[base + 2] = q.z;
            partial[base + 3] = q.w;
        }
    }
    instance_partial_buf.Upload(partial);
}

// ==================== DrawSkinnedMesh ====================
// 一批带骨实例的蒙皮 PBR 渲染：绑材质纹理 + pose atlas，摆放与 pose 选择走 per-instance
// attribute，**整批一次 instanced draw**。
// 材质纹理绑定在 draw 之前（同一命令内所有实例共用材质）。
// 光照（太阳/环境光）由调用方在 Render() 主流程经 UploadSunData/UploadAmbient 预置。
void SkeletonRenderer::DrawSkinnedMesh(
    const SkinnedMeshCommand& cmd,
    const RenderCommandList& cmds,
    MeshManager& mesh_mgr,
    TextureManager& texture_mgr,
    ShaderManager& shader_mgr,
    const float mvp[16],
    unsigned int skinned_prog,
    const SkeletonManager::GpuHandles& gh,
    int pose_count,
    InstanceBuffer& instance_model_buf,
    InstanceBuffer& instance_pose_buf,
    InstanceBuffer& instance_thickness_buf,
    InstanceBuffer& instance_partial_buf) {
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
        const int u = kTexUnitMaterialBase + 0;
        glActiveTexture(GL_TEXTURE0 + u); glBindTexture(GL_TEXTURE_2D, gl_tex);
        glUniform1i(glGetUniformLocation(sp, "uBaseColorTex"), u);
        glUniform1i(glGetUniformLocation(sp, "uHasBaseColorTex"), 1);
    }
    if (cmd.material.has_metallic_tex && cmd.material.metallic_tex != 0) {
        unsigned int gl_tex = texture_mgr.GetGLTexture(cmd.material.metallic_tex);
        CHECK_NE(gl_tex, 0u); const int u = kTexUnitMaterialBase + 1;
        glActiveTexture(GL_TEXTURE0 + u);
        glBindTexture(GL_TEXTURE_2D, gl_tex);
        glUniform1i(glGetUniformLocation(sp, "uMetallicTex"), u);
        glUniform1i(glGetUniformLocation(sp, "uHasMetallicTex"), 1);
    }
    if (cmd.material.has_roughness_tex && cmd.material.roughness_tex != 0) {
        unsigned int gl_tex = texture_mgr.GetGLTexture(cmd.material.roughness_tex);
        CHECK_NE(gl_tex, 0u); const int u = kTexUnitMaterialBase + 2;
        glActiveTexture(GL_TEXTURE0 + u);
        glBindTexture(GL_TEXTURE_2D, gl_tex);
        glUniform1i(glGetUniformLocation(sp, "uRoughnessTex"), u);
        glUniform1i(glGetUniformLocation(sp, "uHasRoughnessTex"), 1);
    }
    if (cmd.material.emissive_tex != 0) {
        unsigned int gl_tex = texture_mgr.GetGLTexture(cmd.material.emissive_tex);
        CHECK_NE(gl_tex, 0u); const int u = kTexUnitMaterialBase + 3;
        glActiveTexture(GL_TEXTURE0 + u);
        glBindTexture(GL_TEXTURE_2D, gl_tex);
        glUniform1i(glGetUniformLocation(sp, "uEmissiveTex"), u);
        glUniform1i(glGetUniformLocation(sp, "uHasEmissiveTex"), 1);
    }
    if (cmd.material.ao_tex != 0) {
        unsigned int gl_tex = texture_mgr.GetGLTexture(cmd.material.ao_tex);
        CHECK_NE(gl_tex, 0u); const int u = kTexUnitMaterialBase + 4;
        glActiveTexture(GL_TEXTURE0 + u);
        glBindTexture(GL_TEXTURE_2D, gl_tex);
        glUniform1i(glGetUniformLocation(sp, "uAoTex"), u);
        glUniform1i(glGetUniformLocation(sp, "uHasAoTex"), 1);
    }
    if (cmd.material.normal_tex != 0) {
        unsigned int gl_tex = texture_mgr.GetGLTexture(cmd.material.normal_tex);
        CHECK_NE(gl_tex, 0u); const int u = kTexUnitMaterialBase + 5;
        glActiveTexture(GL_TEXTURE0 + u);
        glBindTexture(GL_TEXTURE_2D, gl_tex);
        glUniform1i(glGetUniformLocation(sp, "uNormalTex"), u);
        glUniform1i(glGetUniformLocation(sp, "uHasNormalTex"), 1);
        glUniform1f(glGetUniformLocation(sp, "uNormalScale"), cmd.material.normal_scale);
    }

    // ---- 骨纹理 pose atlas（TEXTURE12，见 texture_units.h；避开 0=tile/1-6=材质/7-11=shadow）----
    glActiveTexture(GL_TEXTURE0 + kTexUnitPoseAtlas);
    glBindTexture(GL_TEXTURE_2D, gh.pose_atlas_tex);
    glUniform1i(glGetUniformLocation(sp, "uPoseAtlas"), kTexUnitPoseAtlas);
    glUniform1i(glGetUniformLocation(sp, "uBoneCount"), gh.bone_count);
    // ⚠️ uPoseRow / uAtlasDim 是**全批共享**的 atlas 几何参数（不随实例变）——
    //   与 instancing 改造前一样必须在此上传。漏掉 uAtlasDim → 回绕的分母为 0，
    //   texelFetch 地址非法，整批几何塌成空白（改造时踩过这个坑）。
    glUniform1i(glGetUniformLocation(sp, "uPoseRow"), 0);
    glUniform2f(glGetUniformLocation(sp, "uAtlasDim"),
                static_cast<float>(SkeletonManager::kPoseAtlasDim),
                static_cast<float>(SkeletonManager::kPoseAtlasDim));

    // ---- 部位粗细：开关 + 绑定表纹理（专用槽 kTexUnitThicknessBind，与 tile/材质/shadow/
    //      pose atlas 都不重叠，见 texture_units.h 的编译期校验）----
    //   开关与纹理同源：骨架没配粗细 ⇒ gh.thickness_bind_tex == 0 ⇒ uThicknessEnabled = 0，
    //   shader 整段跳过（旧场景零回归）。纹理绑 0 也无所谓：开着开关时才有 0 以外的采样。
    //   状态机：只动了 active unit 与本槽的 2D 绑定；本 pass 末尾统一 glActiveTexture(GL_TEXTURE0)
    //   还原（与 pose atlas 同约定），槽位专属本功能、无需还原别人的绑定。
    glUniform1i(glGetUniformLocation(sp, "uThicknessEnabled"),
                gh.thickness_bind_tex != 0 ? 1 : 0);
    glActiveTexture(GL_TEXTURE0 + kTexUnitThicknessBind);
    glBindTexture(GL_TEXTURE_2D, gh.thickness_bind_tex);
    glUniform1i(glGetUniformLocation(sp, "uThicknessBind"), kTexUnitThicknessBind);

    // ---- 部位额外旋转：通道表 + 开关（骨架有通道表 **且本批有非恒等旋转** 才开）----
    //   与粗细同源思路：host 侧开关，全恒等/未配置时 shader 整段跳过（逐字节零回归）。
    const int partial_on =
        (gh.partial_rotation_bind_tex != 0 && gh.partial_rotation_channel_tex != 0 &&
         AnyNonIdentityPartialRotation(cmd.instances))
            ? 1 : 0;
    glUniform1i(glGetUniformLocation(sp, "uPartialEnabled"), partial_on);
    glActiveTexture(GL_TEXTURE0 + kTexUnitPartialRotationBind);
    glBindTexture(GL_TEXTURE_2D, gh.partial_rotation_bind_tex);
    glUniform1i(glGetUniformLocation(sp, "uPartialBind"), kTexUnitPartialRotationBind);
    glActiveTexture(GL_TEXTURE0 + kTexUnitPartialRotationChannel);
    glBindTexture(GL_TEXTURE_2D, gh.partial_rotation_channel_tex);
    glUniform1i(glGetUniformLocation(sp, "uPartialChannel"), kTexUnitPartialRotationChannel);

    // ---- uViewProj = proj * view（每帧一张，全批共享）；摆放走 per-instance attribute ----
    glUniformMatrix4fv(glGetUniformLocation(sp, "uViewProj"), 1, GL_FALSE, mvp);

    // 越界校验（SkinnedInstanceState 契约：越界 → FATAL，不静默读 atlas 里别人的骨骼）。
    //   逐实例上传前先全批验一遍：一次 draw 里没法中途报错，验完再画。
    const int pose_w = gh.bone_count * 2;  // 每骨 2 texel（对偶四元数实部 q + 对偶部 t）
    CHECK_GT(pose_count, 0) << "pose_count 必须 > 0（骨架未注册 pose？）";
    for (const SkinnedInstanceState& inst : cmd.instances) {
        CHECK_GE(inst.pose_a, 0) << "pose_a 越界: " << inst.pose_a;
        CHECK_GE(inst.pose_b, 0) << "pose_b 越界: " << inst.pose_b;
        CHECK_LT(inst.pose_a, pose_count)
            << "pose_a 越界: " << inst.pose_a << " >= " << pose_count;
        CHECK_LT(inst.pose_b, pose_count)
            << "pose_b 越界: " << inst.pose_b << " >= " << pose_count;
        CHECK_GE(inst.ratio, 0.0f) << "ratio 越界: " << inst.ratio;
        CHECK_LE(inst.ratio, 1.0f) << "ratio 越界: " << inst.ratio;
    }

    // 实例数据：传进**渲染器持有的**实例缓冲（不写 mesh 资源）。
    //   pose 起点 = pose_idx * pose_width（平坦 texel 起点，shader 内按 atlas 宽回绕）。
    UploadSkinningInstanceAttributes(cmd, pose_w, instance_model_buf, instance_pose_buf,
                                     instance_thickness_buf, instance_partial_buf);

    // ★ 整批 = 一次 instanced draw。这才是 instancing 的意义（N 实例 ≠ N draw call）。
    const GLsizei n_inst = static_cast<GLsizei>(cmd.instances.size());
    {
        // RAII：把实例缓冲挂到本 mesh VAO 的 per-instance 槽，出作用域自动摘除。
        //   用守卫而非手写 enable/disable，是为了**结构上**不可能“挂上忘摘”——
        //   残留 divisor=1 的启用态会泄漏给后续普通 draw（见 instance_buffer.h）。
        InstanceBufferBinding bind(mesh->vao,
                                   {&instance_model_buf, &instance_pose_buf,
                                    &instance_thickness_buf, &instance_partial_buf});
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
        LOG_FIRST_N(WARNING, 1) << "GL error after DrawSkinnedMesh: "
                                << draw_err;
    }
    // 恢复 active texture unit（约定：受入时设过的单元要还原，同 DrawObject3D）。
    glActiveTexture(GL_TEXTURE0);
    glPopAttrib();
}

// ==================== UploadSunData ====================

void SkeletonRenderer::UploadSunData(
    ShaderManager& shader_mgr,
    unsigned int prog,
    const std::vector<CascadeFBO>& shadow_fbos,
    const float shadow_vp[][16],
    const float shadow_depth_vp[][16],
    const float shadow_texel_world[],
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
    // 深度偏置：自动 = shader 用单纹素世界边长几何推导；override = 手工等效边长。
    // 上传全部 kMaxCascades 位（未用位取 0 或默认，shader 只索引实际级联）。
    // 见 ShadowConfig::cascade_bias 的公式与推导。
    static constexpr int kMaxC = jpov::ShadowConfig::kMaxCascades;
    float bias[kMaxC] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    for (int c = 0; c < kMaxC; ++c) bias[c] = cfg.cascade_bias[c];
    glUniform1fv(shader_mgr.GetUniform(prog, "uShadowBiasCascade"), kMaxC, bias);
    glUniform1i(shader_mgr.GetUniform(prog, "uShadowBiasOverride"),
                cfg.override_cascade_bias ? 1 : 0);
    glUniform1fv(shader_mgr.GetUniform(prog, "uShadowTexelWorld"),
                 kMaxC, shadow_texel_world);
    for (int c = 0; c < cascade_count; ++c) {
        const unsigned int unit = static_cast<unsigned int>(kTexUnitShadowMapBase) + static_cast<unsigned int>(c);
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
// 蒙皮在 mesh 局部空间做（蒙皮 VS 内），再乘光空间 VP。
// pose atlas 绑到 kTexUnitPoseAtlas（与主 pass **同号**：两 pass 不同时活跃，无需错开；
//   单元分配表见 src/texture_units.h）。
//
// ⚠️ 整批 = **一次 instanced draw**：摆放矩阵与 pose 选择都走 per-instance attribute，
//   数据由调用方传入的 InstanceBuffer 承载（instance_buffer.h），
//   用 InstanceBufferBinding 挂到 mesh VAO 并在出作用域时自动摘除。
void SkeletonRenderer::DrawSkinnedMeshShadow(
    const SkinnedMeshCommand& cmd,
    MeshManager& mesh_mgr,
    ShaderManager& shader_mgr,
    const SkeletonManager::GpuHandles& gh,
    int pose_count,
    const float shadow_vp[16],
    const float depth_vp[16],
    unsigned int shadow_prog,
    InstanceBuffer& instance_model_buf,
    InstanceBuffer& instance_pose_buf,
    InstanceBuffer& instance_thickness_buf,
    InstanceBuffer& instance_partial_buf) {
    const GPUMesh* mesh = mesh_mgr.GetMesh(cmd.mesh_id);
    CHECK(mesh != nullptr) << "DrawSkinnedMeshShadow: mesh_id " << cmd.mesh_id
                           << " 未注册";
    CHECK_GT(mesh->vao, 0u);
    CHECK(!cmd.instances.empty()) << "SkinnedMesh instance 数组不能为空";

    // 越界校验（SkinnedInstanceState 契约：越界 → FATAL，不静默读 atlas 里别人的骨骼）。
    //   逐实例上传前先全批验一遍：一次 draw 里没法中途报错，验完再画。
    const int pose_w = gh.bone_count * 2;  // 同主 pass：每骨 2 texel（实部 q + 对偶部 t）
    CHECK_GT(pose_count, 0) << "pose_count 必须 > 0（骨架未注册 pose？）";
    for (const SkinnedInstanceState& inst : cmd.instances) {
        CHECK_GE(inst.pose_a, 0) << "pose_a 越界: " << inst.pose_a;
        CHECK_GE(inst.pose_b, 0) << "pose_b 越界: " << inst.pose_b;
        CHECK_LT(inst.pose_a, pose_count)
            << "pose_a 越界: " << inst.pose_a << " >= " << pose_count;
        CHECK_LT(inst.pose_b, pose_count)
            << "pose_b 越界: " << inst.pose_b << " >= " << pose_count;
        CHECK_GE(inst.ratio, 0.0f) << "ratio 越界: " << inst.ratio;
        CHECK_LE(inst.ratio, 1.0f) << "ratio 越界: " << inst.ratio;
    }

    // 实例数据：同一套逐实例缓冲（与主 pass 同源，否则影子与身体错位）。
    //   光空间 VP 走 uniform（全批共享）。
    UploadSkinningInstanceAttributes(cmd, pose_w, instance_model_buf, instance_pose_buf,
                                     instance_thickness_buf, instance_partial_buf);

    glUseProgram(shadow_prog);
    glActiveTexture(GL_TEXTURE0 + kTexUnitPoseAtlas);
    glBindTexture(GL_TEXTURE_2D, gh.pose_atlas_tex);
    glUniform1i(glGetUniformLocation(shadow_prog, "uPoseAtlas"), kTexUnitPoseAtlas);
    glUniform1i(glGetUniformLocation(shadow_prog, "uBoneCount"), gh.bone_count);
    glUniform1i(glGetUniformLocation(shadow_prog, "uPoseRow"), 0);
    glUniform2f(glGetUniformLocation(shadow_prog, "uAtlasDim"),
                static_cast<float>(SkeletonManager::kPoseAtlasDim),
                static_cast<float>(SkeletonManager::kPoseAtlasDim));
    // 部位粗细：与主 pass **同一开关 + 同一份表 + 同一份 per-instance 系数**（异则影子错位）。
    glUniform1i(glGetUniformLocation(shadow_prog, "uThicknessEnabled"),
                gh.thickness_bind_tex != 0 ? 1 : 0);
    glActiveTexture(GL_TEXTURE0 + kTexUnitThicknessBind);
    glBindTexture(GL_TEXTURE_2D, gh.thickness_bind_tex);
    glUniform1i(glGetUniformLocation(shadow_prog, "uThicknessBind"), kTexUnitThicknessBind);
    // 部位额外旋转：与主 pass **同一开关 + 同一份表 + 同一份 per-instance 旋转**（否则影子错位）。
    const int partial_on =
        (gh.partial_rotation_bind_tex != 0 && gh.partial_rotation_channel_tex != 0 &&
         AnyNonIdentityPartialRotation(cmd.instances))
            ? 1 : 0;
    glUniform1i(glGetUniformLocation(shadow_prog, "uPartialEnabled"), partial_on);
    glActiveTexture(GL_TEXTURE0 + kTexUnitPartialRotationBind);
    glBindTexture(GL_TEXTURE_2D, gh.partial_rotation_bind_tex);
    glUniform1i(glGetUniformLocation(shadow_prog, "uPartialBind"), kTexUnitPartialRotationBind);
    glActiveTexture(GL_TEXTURE0 + kTexUnitPartialRotationChannel);
    glBindTexture(GL_TEXTURE_2D, gh.partial_rotation_channel_tex);
    glUniform1i(glGetUniformLocation(shadow_prog, "uPartialChannel"),
                kTexUnitPartialRotationChannel);
    // 光空间 VP 走 uniform（不含 model）。
    glUniformMatrix4fv(glGetUniformLocation(shadow_prog, "uShadowViewProj"),
                       1, GL_FALSE, shadow_vp);
    glUniformMatrix4fv(glGetUniformLocation(shadow_prog, "uShadowDepthViewProj"),
                       1, GL_FALSE, depth_vp);

    const GLsizei n_inst = static_cast<GLsizei>(cmd.instances.size());
    {
        // RAII 配对挂载（同主 pass）。
        InstanceBufferBinding bind(mesh->vao,
                                   {&instance_model_buf, &instance_pose_buf,
                                    &instance_thickness_buf, &instance_partial_buf});
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
    // 恢复 active texture unit（与 DrawSkinnedMesh 同约定）。
    glActiveTexture(GL_TEXTURE0);
}

}  // namespace jpov
