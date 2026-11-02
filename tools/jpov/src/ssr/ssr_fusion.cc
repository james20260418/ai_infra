// JPOV SSR — 融合实现。设计见 ssr_fusion.h 顶部。

#define GL_GLEXT_PROTOTYPES

#include "tools/jpov/src/ssr/ssr_fusion.h"

#include <algorithm>
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

constexpr int kTexUnitIn0 = 0;
constexpr int kTexUnitIn1 = 1;
constexpr int kTexUnitIn2 = 2;
constexpr int kTexUnitIn3 = 3;
constexpr int kTexUnitIn4 = 4;
constexpr int kTexUnitIn5 = 5;      // 低分法线
constexpr int kTexUnitAttrMat = 6;  // 全分辨率材质附件（F0, roughness）
constexpr int kTexUnitSsrMip = 7;   // SSR 结果的预滤波 mip 链
constexpr int kTexUnitCount = 8;

// 合成：全分辨率。A 预乘的双边升采样 + 用 SSR 替换环境镜面项。
constexpr const char* kSsrCompositeFs = R"glsl(
uniform sampler2D uHdrCopy;     // 全分辨率 HDR 副本
uniform sampler2D uAttr;        // 全分辨率（rgb = 三维视空间法线，a = 掩码编码）
uniform sampler2D uAttrMat;     // 全分辨率附件1（rgb = F0，a = roughness）—— 逐像素材质
uniform sampler2D uSceneDepth;  // 全分辨率 R32F
uniform sampler2D uSsr;         // SSR 分辨率 RGBA16F（rgb 辐亮度，a = ray_ratio；调试档 4 用）
// SSR 结果的**粗糙度预滤波 mip 链**（见 ssr_filter.h）：
//   rgb = 覆盖率加权的平均辐亮度，a = 该级别覆盖率。textureLod 三线性 ⇒ 连续。
uniform sampler2D uSsrMip;
uniform float uMipMaxLod;       // 最大可用 LOD（= 级数 − 1）
uniform float uRoughToLod;      // roughness → LOD 的斜率（面板可调）
uniform sampler2D uDownDepth;   // SSR 分辨率 R32F
uniform sampler2D uDownNormal;  // SSR 分辨率 RG16F
uniform ivec2 uFullRes;
uniform vec2  uSsrRes;
uniform int   uScale;
uniform float uNear;
uniform float uFar;
uniform float uTanHalf;
uniform float uAspect;
uniform float uAo;              // 全局（本管线没有 SSAO；逐物体 ao 常量对场景无意义）
uniform float uRangeDepthRatio;    // **软权尺度**：rel 等于它时权重约减半（旧版这里是硬阈值）
uniform float uHardCut;            // rel 超过它 ⇒ 硬拒（数量级不同 = 真跨表面/剪影）
uniform float uWeightFloor;        // 权重下限（相对空间权）⇒ Σw 永不为 0，不出孔洞
uniform int   uCoverageDebug;      // 1 = A 覆盖率灰度；2 = 直通；3 = SSR 辐亮度；4 = ray_ratio

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
// 0 无数据。不再用 sqrt(1−x²−y²) 重建 z —— 该式在极点（法线≈视轴）附近病态：fp16 把
// 1.0 存成 0.999512 会凭空造出 1.79° 倾斜，掠射下把倒影射线抬高约 1 米（根因见 ssr_shader.h）。
vec3 NormalFromAttr(vec4 attr) {
    return normalize(attr.xyz);
}
float MaskFromAttr(vec4 attr) {
    return (attr.a > 0.75) ? 1.0 : 0.0;
}

// A（"这个答案来自屏幕数据"）现在来自**预滤波 mip 链的 alpha**（ssr_filter.h）：
// 它既是"命中覆盖率"（0/1 的加权平均），又已经随 mip 一起被软化 ⇒ 同时承担
// "粗糙度越高、越向 env 交接"和"反射体边缘抗锯齿"两件事。
// （旧的 CoverageOf 直接读类型图，是二值的 —— 那正是"粗糙度只是让斑块缩小"的根因。）

// 该 texel 有没有深度数据（1.0 = 清空值 = 天空）。与 ssr_math.h::HasDepthData 同一条规则。
bool HasDepthAt(ivec2 p) {
    return texelFetch(uSceneDepth, p, 0).r < 1.0;
}

// 某 texel 的 w = 1/z（视距倒数）。
float WAtPx(ivec2 p) {
    return 1.0 / max(ZView(texelFetch(uSceneDepth, p, 0).r), 1.0e-4);
}

// 全分辨率 w = 1/z 在屏幕上的**每像素梯度**（差分，边界 clamp）。
//
// 为什么必须用它（2026-10-31 根因）：`1/z_view` 在**任何平面**上对屏幕坐标是仿射的
// （射影几何：平面 n·X = c ⇒ 1/z 是 (x_ndc, y_ndc) 的线性函数）⇒ 用「本像素 w + 梯度·偏移」
// 可以**精确预测**低分辨率格子里应有的 w，残差在平面上恒 ≈ 0。
// 而直接拿"低分格子的 w"与"本像素的 w"比固定百分比（uRangeDepthRatio）在**掠射**下必然误判：
// 地平线附近同一平面相邻两行的深度比可以有 1.2～2 倍（远大于 5%），于是 2×2 里除了
// "贡献了最小深度"的那一行，其余行全部被判成"不同表面" ⇒ **周期 = 降采样倍率的横向条纹**
// （实测 1:2 → period 2、1:4 → period 4、1:1 无条纹；条纹带下边界正好落在深度比跌破 5% 的行）。
//
// ⚠️ 单侧差分必须有**回退**：紧贴地平线的那几行，上邻居是天空（窗口深度 1.0），
//    直接用它会算出 w_sky − w_hi ≈ −w_hi（远平面），预测值被拉到 0 ⇒ 整条带全拒
//    （实测 1:2 黑 8 行、1:4 黑 27 行，宽度 ∝ 偏移像素数）。平面仿射 ⇒ 换另一侧差分
//    得到的是**同一个斜率**，所以回退是无损的。两侧都没有数据才归零（退回旧的固定阈值）。
vec2 WScreenGrad(ivec2 ip, float w_hi) {
    ivec2 hi = uFullRes - 1;
    ivec2 px = clamp(ip + ivec2(1, 0), ivec2(0), hi);
    ivec2 py = clamp(ip + ivec2(0, 1), ivec2(0), hi);
    float wx = 0.0;
    if (HasDepthAt(px)) {
        wx = WAtPx(px) - w_hi;
    } else {
        ivec2 mx = clamp(ip - ivec2(1, 0), ivec2(0), hi);
        wx = HasDepthAt(mx) ? (w_hi - WAtPx(mx)) : 0.0;   // 单侧退回另一侧
    }
    float wy = 0.0;
    if (HasDepthAt(py)) {
        wy = WAtPx(py) - w_hi;
    } else {
        ivec2 my = clamp(ip - ivec2(0, 1), ivec2(0), hi);
        wy = HasDepthAt(my) ? (w_hi - WAtPx(my)) : 0.0;
    }
    return vec2(wx, wy);
}

void main() {
    ivec2 ip = ivec2(gl_FragCoord.xy);
    vec3 hdr = texelFetch(uHdrCopy, ip, 0).rgb;
    vec4 attr = texelFetch(uAttr, ip, 0);
    float mask = MaskFromAttr(attr);

    // ── A 预乘双边上采样 ──
    // 空间：2×2 双线性权（低分辨率像素为单位）；值域：1/z 相对差 + 法线夹角。
    // **预乘**（w·A·rgb 与 w·A 分别累加）是必须的：否则 A=0 的邻居（rgb=0）会把结果
    // 拉暗，在 A 的边界形成一圈黑边（alpha bleeding）。
    vec2 lo = (vec2(ip) + 0.5) / (vec2(uSsrRes) * float(uScale));  // 低分辨率 uv
    vec2 f = lo * uSsrRes - 0.5;
    ivec2 q0 = ivec2(floor(f));
    vec2 t = f - vec2(q0);

    // ── 逐像素材质（附件 1）：rgb = F0，a = roughness ──
    // 这是 2026-11-02 的核心修正之一：以前这里用 SsrConfig 的**一套**材质，
    // 同屏两种反射材质必然算错（10-31 铬人腰部蓝光的根因）。
    vec4 mat = texelFetch(uAttrMat, ip, 0);
    vec3 F0 = mat.rgb;
    float rough = clamp(mat.a, 0.0, 1.0);
    // roughness → LOD（**线性**，经验映射）：lod = r × k。
    //
    // 为什么不用 α = r²：那是 GGX 分布尾部的参数，把它直接当"视觉模糊量"会让
    // 0.2~0.4 这一段几乎不糊（α 只有 0.04~0.16），而这一段恰恰是金属/石砖最常出现的
    // 粗糙度，看起来仍然是"锐利的镜子" —— 也就是说"逐渐磨砂"在最有用的区间塌掉了。
    // 业界对"反射探针按 roughness 选 mip"普遍用近似线性（UE 的
    // ComputeReflectionCaptureMipFromRoughness 就是线性），这里保持同一口径。
    float lod = clamp(rough * uRoughToLod, 0.0, uMipMaxLod);

    float z_hi = ZView(texelFetch(uSceneDepth, ip, 0).r);
    vec3 n_hi = NormalFromAttr(attr);
    float w_hi = 1.0 / max(z_hi, 1.0e-4);
    // w 的屏幕梯度：把「低分格子的 w」换成本像素位置上的**预测值**再比残差，
    // 这样掠射（同一平面相邻行深度差几十个百分点）不会被误判成“跨表面”。
    vec2 gw = WScreenGrad(ip, w_hi);

    vec3 num = vec3(0.0);
    float den = 0.0;
    float wsum = 0.0;
    for (int j = 0; j < 2; ++j) {
        for (int i = 0; i < 2; ++i) {
            ivec2 q = clamp(q0 + ivec2(i, j), ivec2(0), ivec2(uSsrRes) - 1);
            float ws = (i == 0 ? 1.0 - t.x : t.x) * (j == 0 ? 1.0 - t.y : t.y);

            float z_lo = ZView(texelFetch(uDownDepth, q, 0).r);
            // ⚠️ 必须读**完整三维**法线：属性纹理自 2026-10-31 起存 rgb = 视空间法线。
            //    以前这里只取 .xy 并把 z 补 0（当时的约定是"z 由 sqrt 重建"），改存三分量后
            //    那段没跟着改 ⇒ 升采样的法线值域项拿的是 (x,y,0) 这个**平面内**的假法线，
            //    斜面上 dot(n_hi,n_lo) 系统性偏小 ⇒ 边缘权重被压、倒影边缘变暗/出洞。
            vec3 n_lo = NormalFromAttr(texelFetch(uDownNormal, q, 0));
            float w_lo = 1.0 / max(z_lo, 1.0e-4);

            // ── 值域项：深度 + 法线的**联合软权**（2026-11-02 只剩这一套）──
            //
            // 深度：拿**预测到本像素位置**的低分 w 比残差（不是拿相邻格子的原始 w 比）。
            // dpx = 本像素中心 → 低分 texel 中心的偏移（全分辨率像素）；scale=1 时恒为 0。
            vec2 dpx = (vec2(q) + 0.5) * float(uScale) - (vec2(ip) + 0.5);
            // 低分深度是**格内最近**（降采样取 min 窗口深度 = max w），所以期望值取
            // 本像素平面上「该格范围内」的**最大 w**：仿射函数在方形格上的极值在角上
            // = 中心值 + 半格·(|gx|+|gy|)。注意是 (uScale−1)：取的是格内各 texel 中心的
            // 最小深度，1:1 时格就是一个 texel ⇒ 扩展量必须为 0（否则 1:1 变回归）。
            float w_center = w_hi + dot(gw, dpx);
            float w_pred = w_center + 0.5 * float(uScale - 1) * (abs(gw.x) + abs(gw.y));
            float rel = abs(w_lo - w_pred) / max(abs(w_pred), 1.0e-6);
            float wr = 1.0 / (rel + uRangeDepthRatio);
            bool hard_reject = (rel > uHardCut);   // 数量级不同 ⇒ 真跨表面，硬拒（挡剪影串色）
            // 法线：连续权。0 = 正交（地面 vs 墙），1 = 同向。
            wr *= clamp(dot(n_hi, n_lo), 0.0, 1.0);

            // 空间权 × 值域权；**非硬拒**的再抬一个下限，保证 Σw > 0（不出孔洞）。
            float w = ws * wr;
            if (!hard_reject) {
                w = max(w, ws * uWeightFloor);
            } else {
                w = 0.0;
            }
            // 按 roughness 选 mip 级采样（同一级内是覆盖率加权的平均）。
            // uv 用 **texel 中心**：(q + 0.5) / uSsrRes。
            vec2 uv_lo = (vec2(q) + 0.5) / uSsrRes;
            vec4 s = textureLod(uSsrMip, uv_lo, lod);
            float a_lo = s.a;
            num += w * a_lo * s.rgb;
            den += w * a_lo;
            wsum += w;
        }
    }
    float A = (wsum > 0.0) ? (den / wsum) : 0.0;
    vec3 ssr_L = (den > 1.0e-6) ? (num / den) : vec3(0.0);

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
    // 调试档 4：ray_ratio（预算占用率）灰度图。命中越接近预算上限越亮 ——
    // 用来目视确认"提前结束的射线不会被误判成快到极限"。
    if (uCoverageDebug == 4) {
        ivec2 q0c = clamp(q0, ivec2(0), ivec2(uSsrRes) - 1);
        FragColor = vec4(vec3(texelFetch(uSsr, q0c, 0).a), 1.0);
        return;
    }

    if (mask < 0.5 || uCoverageDebug == 2) {
        FragColor = vec4(hdr, 1.0);
        return;
    }

    // ── 材质 / 环境镜面项（与 3D pass 同一份实现：kEnvSpecularGlsl）──
    vec2 ndc = (vec2(ip) + 0.5) / vec2(uFullRes) * 2.0 - 1.0;
    vec3 p_view = vec3(ndc.x * uAspect * uTanHalf, ndc.y * uTanHalf, -1.0) * z_hi;
    vec3 V = normalize(-p_view);
    float r = clamp(rough, 0.02, 1.0);
    vec2 AB = EnvBrdfApprox(r, clamp(dot(n_hi, V), 1.0e-4, 1.0));
    vec3 weight = F0 * AB.x + AB.y;
    // env 项用**同一个逐像素 F0/roughness** 重算 ⇒ 被替换掉的那一份与被加上的那一份
    // 是同一套材质参数（这是"单调改进"的前提）。
    vec3 env_term = EnvSpecularTermF0(n_hi, V, rough, F0);

    // 本式**不做**逐通道钳位（钳到 hdr 以下会改变能量、掩盖问题）。
    // 逐像素材质（2026-11-02）让"hdr − env_term"的两侧用了同一套参数，负通道已不再是
    // 常态；残留的一种可能是**属性 pass 与主 pass 的材质来源不同**：主 pass 的 base_color
    // 可以被纹理覆盖，而属性 pass 的非镂空变体只有材质常量（镂空变体才有 UV 去采样）。
    // 带纹理的反射体因此在 env 项上有一份残差 —— 如实记录，不在合成层打补丁。
    vec3 out_rgb = hdr + mask * A * uAo * (ssr_L * weight - env_term);
    FragColor = vec4(out_rgb, 1.0);
}
)glsl";

}  // namespace

SsrFusion::SsrFusion() = default;
SsrFusion::~SsrFusion() { Finalize(); }

void SsrFusion::Init(ShaderManager* shader_mgr) {
    CHECK(shader_mgr != nullptr);
    shader_mgr_ = shader_mgr;
    // 合成的 FS = 环境镜面（GLSL 只有一份，见 env_specular.h）+ 本模块主体。
    const std::string composite_src =
        std::string(kSsrGlslVersion) + kEnvSpecularGlsl + kSsrCompositeFs;
    prog_ = shader_mgr_->GetOrCreate("ssr_composite",
                                     {kSsrFullscreenVs, composite_src.c_str(), {}});
    CHECK_NE(prog_, 0u);
}

void SsrFusion::Finalize() {
    prog_ = 0;
    shader_mgr_ = nullptr;
}

void SsrFusion::Run(const SsrFusionInputs& in) {
    CHECK(shader_mgr_ != nullptr) << "SsrFusion::Run 前必须先 Init";
    CHECK_NE(in.hdr_copy_tex, 0u);
    CHECK_NE(in.attr_tex, 0u);
    CHECK_NE(in.scene_depth_tex, 0u);
    CHECK_NE(in.ssr_radiance_tex, 0u);
    CHECK_NE(in.attr_mat_tex, 0u) << "缺属性纹理的材质附件（F0/roughness）";
    CHECK_NE(in.ssr_mip_tex, 0u) << "缺 SSR 预滤波 mip 链（SsrFilter 的输出）";
    CHECK_GE(in.ssr_mip_levels, 1);
    CHECK_NE(in.down_depth_tex, 0u);
    CHECK_NE(in.down_normal_tex, 0u);
    CHECK_NE(in.dst_fbo, 0u);
    CHECK_GT(in.full_w, 0);
    CHECK_GT(in.full_h, 0);
    CHECK_GT(in.ssr_w, 0);
    CHECK_GT(in.ssr_h, 0);
    CHECK_GE(in.scale, 1);
    CHECK_LE(in.scale, 4);
    CHECK_NE(in.env_cube_tex, 0u) << "融合需要环境 cubemap（与 3D pass 同一份）";

    GLint prev_fbo = 0;
    GLint prev_viewport[4] = {0, 0, 0, 0};
    GLint prev_active = 0;
    GLint prev_prog = 0;
    GLint prev_draw_buffer = GL_COLOR_ATTACHMENT0;
    GLint prev_blend = 0;
    GLint prev_depth = 0;
    GLint prev_cull = 0;
    GLint prev_scissor = 0;
    GLint prev_bind[kTexUnitCount] = {0, 0, 0, 0, 0, 0, 0, 0};
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    glGetIntegerv(GL_VIEWPORT, prev_viewport);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_active);
    glGetIntegerv(GL_CURRENT_PROGRAM, &prev_prog);
    prev_blend = glIsEnabled(GL_BLEND);
    prev_depth = glIsEnabled(GL_DEPTH_TEST);
    prev_cull = glIsEnabled(GL_CULL_FACE);
    prev_scissor = glIsEnabled(GL_SCISSOR_TEST);
    for (int u = 0; u < kTexUnitCount; ++u) {
        glActiveTexture(GL_TEXTURE0 + u);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_bind[u]);
    }

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
    glGetIntegerv(GL_DRAW_BUFFER, &prev_draw_buffer);
    glDrawBuffer(GL_COLOR_ATTACHMENT0);
    glUseProgram(prog_);

    glActiveTexture(GL_TEXTURE0 + kTexUnitIn0);
    glBindTexture(GL_TEXTURE_2D, in.hdr_copy_tex);
    glActiveTexture(GL_TEXTURE0 + kTexUnitIn1);
    glBindTexture(GL_TEXTURE_2D, in.attr_tex);
    glActiveTexture(GL_TEXTURE0 + kTexUnitIn2);
    glBindTexture(GL_TEXTURE_2D, in.scene_depth_tex);
    glActiveTexture(GL_TEXTURE0 + kTexUnitIn3);
    glBindTexture(GL_TEXTURE_2D, in.ssr_radiance_tex);
    glActiveTexture(GL_TEXTURE0 + kTexUnitIn4);
    glBindTexture(GL_TEXTURE_2D, in.down_depth_tex);
    glActiveTexture(GL_TEXTURE0 + kTexUnitIn5);
    glBindTexture(GL_TEXTURE_2D, in.down_normal_tex);
    glActiveTexture(GL_TEXTURE0 + kTexUnitAttrMat);
    glBindTexture(GL_TEXTURE_2D, in.attr_mat_tex);
    glActiveTexture(GL_TEXTURE0 + kTexUnitSsrMip);
    glBindTexture(GL_TEXTURE_2D, in.ssr_mip_tex);

    glUniform1i(shader_mgr_->GetUniform(prog_, "uHdrCopy"), kTexUnitIn0);
    glUniform1i(shader_mgr_->GetUniform(prog_, "uAttr"), kTexUnitIn1);
    glUniform1i(shader_mgr_->GetUniform(prog_, "uSceneDepth"), kTexUnitIn2);
    glUniform1i(shader_mgr_->GetUniform(prog_, "uSsr"), kTexUnitIn3);
    glUniform1i(shader_mgr_->GetUniform(prog_, "uDownDepth"), kTexUnitIn4);
    glUniform1i(shader_mgr_->GetUniform(prog_, "uDownNormal"), kTexUnitIn5);
    glUniform1i(shader_mgr_->GetUniform(prog_, "uAttrMat"), kTexUnitAttrMat);
    glUniform1i(shader_mgr_->GetUniform(prog_, "uSsrMip"), kTexUnitSsrMip);

    glUniform2i(shader_mgr_->GetUniform(prog_, "uFullRes"), in.full_w, in.full_h);
    glUniform2f(shader_mgr_->GetUniform(prog_, "uSsrRes"),
                static_cast<float>(in.ssr_w), static_cast<float>(in.ssr_h));
    glUniform1i(shader_mgr_->GetUniform(prog_, "uScale"), in.scale);
    glUniform1f(shader_mgr_->GetUniform(prog_, "uNear"), in.near_plane);
    glUniform1f(shader_mgr_->GetUniform(prog_, "uFar"), in.far_plane);
    glUniform1f(shader_mgr_->GetUniform(prog_, "uTanHalf"), in.tan_half_fov_y);
    glUniform1f(shader_mgr_->GetUniform(prog_, "uAspect"), in.aspect);
    glUniform1f(shader_mgr_->GetUniform(prog_, "uAo"), in.cfg.ao);
    glUniform1f(shader_mgr_->GetUniform(prog_, "uMipMaxLod"),
                static_cast<float>(std::max(0, in.ssr_mip_levels - 1)));
    glUniform1f(shader_mgr_->GetUniform(prog_, "uRoughToLod"), in.cfg.roughness_to_lod);
    glUniform1f(shader_mgr_->GetUniform(prog_, "uRangeDepthRatio"),
                in.cfg.upsample_depth_tolerance);
    glUniform1f(shader_mgr_->GetUniform(prog_, "uHardCut"), in.cfg.upsample_hard_cut);
    glUniform1f(shader_mgr_->GetUniform(prog_, "uWeightFloor"), in.cfg.upsample_weight_floor);
    glUniform1i(shader_mgr_->GetUniform(prog_, "uCoverageDebug"), in.cfg.debug_coverage);
    // 环境镜面：**每帧必须重传**（uniform 是 program 状态，上一帧开着本帧不传就还用旧的）。
    UploadEnvSpecular(*shader_mgr_, prog_, in.env_cube_tex, in.env_max_lod,
                      in.env_sky_scale, in.env_enabled);

    glDrawArrays(GL_TRIANGLES, 0, 3);

    glDrawBuffer(static_cast<GLenum>(prev_draw_buffer));
    glUseProgram(static_cast<GLuint>(prev_prog));
    for (int u = 0; u < kTexUnitCount; ++u) {
        glActiveTexture(GL_TEXTURE0 + u);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prev_bind[u]));
    }
    glActiveTexture(static_cast<GLenum>(prev_active));
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prev_fbo));
    glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);
    if (prev_blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (prev_depth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (prev_cull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    if (prev_scissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
}

}  // namespace jpov
