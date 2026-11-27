// JPOV SSR — 不透明场景的消费端（合成）。设计与公式见 ssr_composite.h 顶部。

#define GL_GLEXT_PROTOTYPES

#include "tools/jpov/src/ssr/ssr_composite.h"

#include <string>

#include <GL/gl.h>
#include <GL/glext.h>

#ifdef _WIN32
#ifndef GLOG_NO_ABBREVIATED_SEVERITIES
#define GLOG_NO_ABBREVIATED_SEVERITIES
#endif
#include "third_party/gl_loader-mingw/gl_loader.h"
#endif

#include <glog/logging.h>

#include "tools/jpov/src/env_specular.h"
#include "tools/jpov/src/shader_manager.h"
#include "tools/jpov/src/ssr/ssr_shader.h"

namespace jpov {
namespace {

constexpr int kTexUnitHdrCopy = 0;
constexpr int kTexUnitAttr = 1;
constexpr int kTexUnitAttrMat = 2;
constexpr int kTexUnitSceneDepth = 3;
constexpr int kTexUnitReflection = 4;
constexpr int kTexUnitBackface = 5;
constexpr int kTexUnitRaw = 6;
constexpr int kTexUnitCount = 7;

constexpr const char* kSsrCompositeFs = R"glsl(
uniform sampler2D uHdrCopy;      // 全分辨率 HDR 副本
uniform sampler2D uAttr;         // 全分辨率（rgb = 三维视空间法线，a = 掩码编码）
uniform sampler2D uAttrMat;      // 全分辨率附件1（rgb = F0，a = roughness）
uniform sampler2D uSceneDepth;   // 全分辨率 R32F 窗口深度
uniform sampler2D uReflection;   // 全分辨率 RGBA32F（rgb = SSR 辐亮度 L，a = 覆盖率 A）
uniform usampler2D uBackface;    // 全分辨率 R8UI（bit0 = 背面命中）
uniform sampler2D uRawSsr;       // SSR 分辨率 RGBA16F（**仅调试档 4**）
uniform ivec2 uFullRes;
uniform vec2  uSsrRes;
uniform int   uScale;
uniform float uNear;
uniform float uFar;
uniform float uTanHalf;
uniform float uAspect;
uniform float uAo;               // 全局（本管线没有 SSAO；逐物体 ao 常量对场景无意义）
uniform int   uCoverageDebug;    // 1=A 灰度；2=直通；3=SSR 辐亮度；4=SSR 档的 A；5=背面掩码
uniform int   uBackfaceUseSsr;
uniform float uBackfaceDim;      // [0,1]，1 = 不压暗
uniform mat3  uWorldToView;      // 世界 → 视空间（列主序，旋转）；env 项用它的转置

layout(location = 0) out vec4 FragColor;

// 窗口深度 → 视距（米）。与 ssr_math.h::DepthFromWindowDepth 同一份公式（含 NaN 守卫）。
float ZView(float depth) {
    float ndc = 2.0 * depth - 1.0;
    float denom = (uFar + uNear) - ndc * (uFar - uNear);
    if (!(abs(denom) > 1.0e-6)) {
        return max(uNear, 1.0e-4);
    }
    return (2.0 * uNear * uFar) / denom;
}

// 属性纹理语义（2026-10-31 起）：rgb = **完整三维视空间法线**，a = 1 反射体 / 0.5 有法线 /
// 0 无数据。不再用 sqrt(1−x²−y²) 重建 z —— 该式在极点（法线≈视轴）附近病态。
vec3 NormalFromAttr(vec4 attr) {
    return normalize(attr.xyz);
}
float MaskFromAttr(vec4 attr) {
    return (attr.a > 0.75) ? 1.0 : 0.0;
}

void main() {
    ivec2 ip = ivec2(gl_FragCoord.xy);
    vec3 hdr = texelFetch(uHdrCopy, ip, 0).rgb;
    vec4 attr = texelFetch(uAttr, ip, 0);
    float mask = MaskFromAttr(attr);

    // 装置给的（全分辨率）结果：rgb = 平均辐亮度，a = 覆盖率；背面 flag 另一张。
    vec4 refl = texelFetch(uReflection, ip, 0);
    float A = refl.a;
    vec3 ssr_L = refl.rgb;
    bool is_backface = (texelFetch(uBackface, ip, 0).r & 1u) != 0u;

    if (uCoverageDebug == 1) {
        FragColor = vec4(vec3(A), 1.0);
        return;
    }
    // 3 = 直出 SSR 辐亮度（**不经材质/环境项**）：用来判断一个可疑颜色是"SSR 采到的内容"
    // 还是"合成公式里的材质/环境项"。调试用，不进面板。
    if (uCoverageDebug == 3) {
        FragColor = vec4(ssr_L * A, 1.0);
        return;
    }
    // 调试档 5：背面命中掩码（白 = 该像素的 SSR 结果是背面命中）。
    // 配合两个 flag 用：先把两个 flag 都关掉看掩码范围，再逐个别开看画面变化。
    if (uCoverageDebug == 5) {
        FragColor = vec4(vec3(is_backface ? 1.0 : 0.0), 1.0);
        return;
    }
    // 调试档 4：**A（置信度，含淡出）** 的灰度图，读 SSR 分辨率那一份。
    // 与档 1 的差别：档 1 是升采样+软权之后的全分辨率 A，档 4 是装置内部的原始值。
    if (uCoverageDebug == 4) {
        // 全分辨率像素 → 它所属的那个 SSR texel（= floor((ip+0.5)/N − 0.5)，整数等价于 ip/N）。
        ivec2 q = clamp(ip / max(uScale, 1), ivec2(0), ivec2(uSsrRes) - 1);
        FragColor = vec4(vec3(texelFetch(uRawSsr, q, 0).a), 1.0);
        return;
    }

    if (mask < 0.5 || uCoverageDebug == 2) {
        FragColor = vec4(hdr, 1.0);
        return;
    }

    // ── 材质 / 环境镜面项（与 3D pass 同一份实现：kEnvSpecularGlsl）──
    vec4 mat = texelFetch(uAttrMat, ip, 0);
    vec3 F0 = mat.rgb;
    float rough = clamp(mat.a, 0.0, 1.0);
    float z_hi = ZView(texelFetch(uSceneDepth, ip, 0).r);
    vec3 n_hi = NormalFromAttr(attr);
    vec2 ndc = (vec2(ip) + 0.5) / vec2(uFullRes) * 2.0 - 1.0;
    vec3 p_view = vec3(ndc.x * uAspect * uTanHalf, ndc.y * uTanHalf, -1.0) * z_hi;
    vec3 V = normalize(-p_view);
    float r = clamp(rough, 0.02, 1.0);
    vec2 AB = EnvBrdfApprox(r, clamp(dot(n_hi, V), 1.0e-4, 1.0));
    vec3 weight = F0 * AB.x + AB.y;
    // env 项用**同一个逐像素 F0/roughness** 重算 ⇒ 被替换掉的那一份与被加上的那一份
    // 是同一套材质参数（这是"单调改进"的前提）。
    //
    // ⚠️ 坐标系是不变量：属性纹理的法线是**视空间**的，而 uEnvCube 是按**世界方向**
    //    渲染的天球探针。直接用视空间 (N, V) 去采样 = 在天球上查了另一个方位
    //    ⇒ 减掉的不是 3D pass 加进去的那一份，差额（通常是大正值）留在画面上：
    //    镜面地板里"暗物体的倒影"整块发亮/发白（2026-11-24 根因）。
    //    旋转矩阵正交 ⇒ transpose 就是逆变换；Fresnel 的 dot(N,V) 与坐标系无关。
    vec3 n_world = transpose(uWorldToView) * n_hi;
    vec3 V_world = transpose(uWorldToView) * V;
    vec3 env_term = EnvSpecularTermF0(n_world, V_world, rough, F0);

    // 本式**不做**逐通道钳位（钳到 hdr 以下会改变能量、掩盖问题）。
    // 逐像素材质让"hdr − env_term"的两侧用了同一套参数，负通道已不再是常态；残留的一种
    // 可能是**属性 pass 与主 pass 的材质来源不同**：主 pass 的 base_color 可以被纹理覆盖，
    // 而属性 pass 的非镂空变体只有材质常量（镂空变体才有 UV 去采样）。带纹理的反射体
    // 因此在 env 项上有一份残差 —— 如实记录，不在合成层打补丁。
    //
    // ⚠️ **逐通道对 hdr 截断被减项**（2026-11-02 修"剪影红边"）：数学上
    //   hdr − env_term + ssr_L·weight ≥ 0 只要 hdr 里那份额度与 env_term 是同一套参数；
    //   但在**蒙皮反射体的剪影**上，属性 pass 的覆盖率/法线与主 pass 会有 1~3 px 的不一致
    //   （实测：镜面人轮廓上一圈 57 个像素），此时 env_term 远大于 hdr 里实际存在的那一份
    //   ⇒ 出现负辐亮度 ⇒ tone map 把负通道压成 0、正通道顶到 1 ⇒ **纯红描边**。
    //   辐亮度不可能为负，所以"减掉的不超过现有的"是物理硬约束，不是调参：
    //   正常情况下 removed ≤ hdr，本式与原来的 `hdr + (add − removed)` **逐位相同**。
    //
    // ── 背面命中的处置（2026-11-13）：两个正交 flag ──
    // A 不动（背面命中同样受压缩比/预算/贴边淡出约束）。只有"被减掉的 env 权重"和
    // "加上的 ssr 权重"这两个系数按 flag 改：
    //   use_ssr=false, dim=1 ⇒ A_rm = 0, A_ad = 0 ⇒ out = hdr（**逐字节等于旧行为**）
    //   use_ssr=false, dim<1 ⇒ 只保留 dim 那份 env（旧版 backface_env_scale 的语义）
    //   use_ssr=true        ⇒ 用 SSR 颜色，整体乘 dim
    float bf_dim = clamp(uBackfaceDim, 0.0, 1.0);
    float A_rm = A;
    float A_ad = A;
    if (is_backface) {
        if (uBackfaceUseSsr != 0) {
            A_rm = A;
            A_ad = A * bf_dim;
        } else {
            A_rm = A * (1.0 - bf_dim);   // 只保住 bf_dim 那份 env
            A_ad = 0.0;
        }
    }
    vec3 removed = mask * A_rm * uAo * env_term;
    removed = min(removed, max(hdr, vec3(0.0)));
    vec3 out_rgb = hdr - removed + mask * A_ad * uAo * ssr_L * weight;
    FragColor = vec4(out_rgb, 1.0);
}
)glsl";

}  // namespace

SsrComposite::SsrComposite() = default;
SsrComposite::~SsrComposite() { Finalize(); }

void SsrComposite::Init(ShaderManager* shader_mgr) {
    CHECK(shader_mgr != nullptr);
    if (prog_ != 0) {
        Finalize();
    }
    shader_mgr_ = shader_mgr;
    // 合成的 FS = 环境镜面（GLSL 只有一份，见 env_specular.h）+ 本模块主体。
    const std::string src = std::string("#version 330 core\n") + kEnvSpecularGlsl + kSsrCompositeFs;
    prog_ = shader_mgr_->GetOrCreate("ssr_composite", {kSsrFullscreenVs, src.c_str(), {}});
    CHECK_NE(prog_, 0u);
}

void SsrComposite::Finalize() {
    if (src_fbo_ != 0) {
        // 删掉 FBO 本身就会放掉对 hdr_tex 的引用（每次 Run 末尾也主动脱钩，双保险）。
        glDeleteFramebuffers(1, &src_fbo_);
        src_fbo_ = 0;
    }
    if (copy_fbo_ != 0) {
        glDeleteFramebuffers(1, &copy_fbo_);
        copy_fbo_ = 0;
    }
    if (copy_tex_ != 0) {
        glDeleteTextures(1, &copy_tex_);
        copy_tex_ = 0;
    }
    width_ = 0;
    height_ = 0;
    prog_ = 0;
    shader_mgr_ = nullptr;
}

void SsrComposite::EnsureCopyTarget(int w, int h) {
    if (w == width_ && h == height_ && copy_fbo_ != 0) {
        return;
    }
    if (copy_fbo_ != 0) {
        glDeleteFramebuffers(1, &copy_fbo_);
        copy_fbo_ = 0;
    }
    if (copy_tex_ != 0) {
        glDeleteTextures(1, &copy_tex_);
        copy_tex_ = 0;
    }
    width_ = w;
    height_ = h;

    GLint prev_fbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);

    glGenTextures(1, &copy_tex_);
    glBindTexture(GL_TEXTURE_2D, copy_tex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    glGenFramebuffers(1, &copy_fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, copy_fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, copy_tex_, 0);
    CHECK_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE))
        << "SSR copy FBO 不完整";
    // 读端 FBO：每帧把调用方的 hdr_tex 临时挂上去做 blit 源。刻意**不在**这里挂，
    // 因为 hdr_tex 可能换（换场景/换分辨率），而挂着的引用会让调用方删不掉那张纹理。
    glGenFramebuffers(1, &src_fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prev_fbo));
}

void SsrComposite::Run(const SsrCompositeInputs& in) {
    CHECK(shader_mgr_ != nullptr) << "SsrComposite::Run 前必须先 Init";
    CHECK_NE(in.hdr_tex, 0u) << "SsrCompositeInputs::hdr_tex 未设置";
    CHECK_NE(in.attr_tex, 0u) << "SsrCompositeInputs::attr_tex 未设置";
    CHECK_NE(in.attr_mat_tex, 0u) << "SsrCompositeInputs::attr_mat_tex 未设置（F0/roughness）";
    CHECK_NE(in.scene_depth_tex, 0u) << "SsrCompositeInputs::scene_depth_tex 未设置";
    CHECK_NE(in.reflection_tex, 0u) << "SsrCompositeInputs::reflection_tex 未设置";
    CHECK_NE(in.backface_tex, 0u) << "SsrCompositeInputs::backface_tex 未设置";
    CHECK_NE(in.raw_ssr_tex, 0u) << "SsrCompositeInputs::raw_ssr_tex 未设置";
    CHECK_NE(in.dst_fbo, 0u) << "SsrCompositeInputs::dst_fbo 未设置";
    CHECK_NE(in.env_cube_tex, 0u) << "合成需要环境 cubemap（与 3D pass 同一份）";
    CHECK_GT(in.full_w, 0);
    CHECK_GT(in.full_h, 0);
    CHECK_GT(in.ssr_w, 0);
    CHECK_GT(in.ssr_h, 0);
    CHECK_GE(in.scale, 1);
    CHECK_LE(in.scale, 4);
    CHECK_GT(in.near_plane, 0.0f);
    CHECK_GT(in.far_plane, in.near_plane);
    CHECK_GT(in.tan_half_fov_y, 0.0f);
    CHECK_GT(in.aspect, 0.0f);
    // 世界→视空间：全 0 = 没填。合成层靠它把法线转到世界空间再采探针，
    // 填错/没填的表现是"env 项被减错"（画面整块发白），不是崩溃 ⇒ 必须拦住。
    float w2v_norm = 0.0f;
    for (float v : in.world_to_view) {
        w2v_norm += v * v;
    }
    CHECK(w2v_norm > 1.0e-6f) << "SsrCompositeInputs::world_to_view 未设置";

    GLint prev_fbo = 0;
    GLint prev_read = 0;
    GLint prev_viewport[4] = {0, 0, 0, 0};
    GLint prev_active = 0;
    GLint prev_prog = 0;
    GLint prev_draw_buffer = GL_COLOR_ATTACHMENT0;
    GLint prev_blend = 0;
    GLint prev_depth = 0;
    GLint prev_cull = 0;
    GLint prev_scissor = 0;
    GLint prev_bind[kTexUnitCount] = {0, 0, 0, 0, 0, 0, 0};
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read);
    glGetIntegerv(GL_VIEWPORT, prev_viewport);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_active);
    glGetIntegerv(GL_CURRENT_PROGRAM, &prev_prog);
    glGetIntegerv(GL_DRAW_BUFFER, &prev_draw_buffer);
    prev_blend = glIsEnabled(GL_BLEND);
    prev_depth = glIsEnabled(GL_DEPTH_TEST);
    prev_cull = glIsEnabled(GL_CULL_FACE);
    prev_scissor = glIsEnabled(GL_SCISSOR_TEST);
    for (int u = 0; u < kTexUnitCount; ++u) {
        glActiveTexture(GL_TEXTURE0 + u);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_bind[u]);
    }

    EnsureCopyTarget(in.full_w, in.full_h);

    // ── HDR 副本：blit（不采样、不重建，纯拷贝）──
    // 合成要**读** hdr 又**写** hdr（同一张纹理 = 反馈环）⇒ 先拷一份。
    glBindFramebuffer(GL_READ_FRAMEBUFFER, src_fbo_);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, in.hdr_tex, 0);
    CHECK_EQ(glCheckFramebufferStatus(GL_READ_FRAMEBUFFER),
             static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE))
        << "SSR copy 的读端 FBO 不完整（hdr_tex 不能作为 blit 源，检查格式/尺寸）";
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, copy_fbo_);
    glDrawBuffer(GL_COLOR_ATTACHMENT0);
    glBlitFramebuffer(0, 0, in.full_w, in.full_h, 0, 0, in.full_w, in.full_h,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
    // 摘掉引用：否则调用方以后删 hdr_tex 时，本类还攥着它（GL 会让纹理名字存活到脱钩）。
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);

    glBindFramebuffer(GL_FRAMEBUFFER, in.dst_fbo);
    glViewport(0, 0, in.full_w, in.full_h);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);     // 全屏三角形
    glDisable(GL_SCISSOR_TEST);  // 调用方开着 scissor 会把全屏 pass 裁成一小块
    // ⚠️ 目标 FBO 是 MRT 的（附件0 = HDR 颜色，附件1 = 场景深度）。本 pass 的 FS 只有
    //    一个输出（location 0）—— 若把两个附件都当 draw buffer，**附件1 的内容会变成
    //    未定义**（GL 规范：没有对应输出的 draw buffer 值是 undefined）。而场景深度 MRT
    //    后面还在被远景雾/其它后处理读 ⇒ 开 SSR 时雾会读到垃圾深度。
    //    所以这里只画附件0，并在返回前恢复调用方的 draw buffer 设置。
    glDrawBuffer(GL_COLOR_ATTACHMENT0);
    glUseProgram(prog_);

    glActiveTexture(GL_TEXTURE0 + kTexUnitHdrCopy);
    glBindTexture(GL_TEXTURE_2D, copy_tex_);
    glActiveTexture(GL_TEXTURE0 + kTexUnitAttr);
    glBindTexture(GL_TEXTURE_2D, in.attr_tex);
    glActiveTexture(GL_TEXTURE0 + kTexUnitAttrMat);
    glBindTexture(GL_TEXTURE_2D, in.attr_mat_tex);
    glActiveTexture(GL_TEXTURE0 + kTexUnitSceneDepth);
    glBindTexture(GL_TEXTURE_2D, in.scene_depth_tex);
    glActiveTexture(GL_TEXTURE0 + kTexUnitReflection);
    glBindTexture(GL_TEXTURE_2D, in.reflection_tex);
    glActiveTexture(GL_TEXTURE0 + kTexUnitBackface);
    glBindTexture(GL_TEXTURE_2D, in.backface_tex);
    glActiveTexture(GL_TEXTURE0 + kTexUnitRaw);
    glBindTexture(GL_TEXTURE_2D, in.raw_ssr_tex);

    glUniform1i(shader_mgr_->GetUniform(prog_, "uHdrCopy"), kTexUnitHdrCopy);
    glUniform1i(shader_mgr_->GetUniform(prog_, "uAttr"), kTexUnitAttr);
    glUniform1i(shader_mgr_->GetUniform(prog_, "uAttrMat"), kTexUnitAttrMat);
    glUniform1i(shader_mgr_->GetUniform(prog_, "uSceneDepth"), kTexUnitSceneDepth);
    glUniform1i(shader_mgr_->GetUniform(prog_, "uReflection"), kTexUnitReflection);
    glUniform1i(shader_mgr_->GetUniform(prog_, "uBackface"), kTexUnitBackface);
    glUniform1i(shader_mgr_->GetUniform(prog_, "uRawSsr"), kTexUnitRaw);
    glUniform2i(shader_mgr_->GetUniform(prog_, "uFullRes"), in.full_w, in.full_h);
    glUniform2f(shader_mgr_->GetUniform(prog_, "uSsrRes"), static_cast<float>(in.ssr_w),
                static_cast<float>(in.ssr_h));
    glUniform1i(shader_mgr_->GetUniform(prog_, "uScale"), in.scale);
    glUniform1f(shader_mgr_->GetUniform(prog_, "uNear"), in.near_plane);
    glUniform1f(shader_mgr_->GetUniform(prog_, "uFar"), in.far_plane);
    glUniform1f(shader_mgr_->GetUniform(prog_, "uTanHalf"), in.tan_half_fov_y);
    glUniform1f(shader_mgr_->GetUniform(prog_, "uAspect"), in.aspect);
    glUniform1f(shader_mgr_->GetUniform(prog_, "uAo"), in.ao);
    glUniform1i(shader_mgr_->GetUniform(prog_, "uCoverageDebug"), in.debug_view);
    glUniform1i(shader_mgr_->GetUniform(prog_, "uBackfaceUseSsr"),
                in.backface_use_ssr ? 1 : 0);
    glUniform1f(shader_mgr_->GetUniform(prog_, "uBackfaceDim"), in.backface_dim);
    glUniformMatrix3fv(shader_mgr_->GetUniform(prog_, "uWorldToView"), 1, GL_FALSE,
                       in.world_to_view.data());
    // 环境镜面：**每帧必须重传**（uniform 是 program 状态，上一帧开着本帧不传就还用旧的）。
    UploadEnvSpecular(*shader_mgr_, prog_, in.env_cube_tex, in.env_max_lod, in.env_sky_scale,
                      in.env_enabled);

    glDrawArrays(GL_TRIANGLES, 0, 3);

    glDrawBuffer(static_cast<GLenum>(prev_draw_buffer));
    glUseProgram(static_cast<GLuint>(prev_prog));
    for (int u = 0; u < kTexUnitCount; ++u) {
        glActiveTexture(GL_TEXTURE0 + u);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prev_bind[u]));
    }
    glActiveTexture(static_cast<GLenum>(prev_active));
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prev_fbo));
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(prev_read));
    glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);
    if (prev_blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (prev_depth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (prev_cull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    if (prev_scissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
}

}  // namespace jpov
