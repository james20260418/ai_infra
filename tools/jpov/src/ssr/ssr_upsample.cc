// JPOV SSR — 升采样实现。设计与三条细节见 ssr_upsample.h 顶部。
//
// 这段数学原来长在 ssr_fusion.cc（"融合"）里，2026-11-27 拆出来：那次拆分的唯一目的是
// 让 SSR 装置（ssr_chain.h）能把"低分辨率结果 → 全分辨率反射图"这一整段封装进去，
// 不再与"用 env 项替换"（那是**消费端**的位置知识）焊在一起。
// 拆的时候公式一行没改 ⇒ 相同输入下输出逐字节相同。

#define GL_GLEXT_PROTOTYPES

#include "tools/jpov/src/ssr/ssr_upsample.h"

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

#include "tools/jpov/src/shader_manager.h"
#include "tools/jpov/src/ssr/ssr_shader.h"

namespace jpov {
namespace {

constexpr int kTexUnitMip = 0;
constexpr int kTexUnitDownW = 1;
constexpr int kTexUnitDownNormal = 2;
constexpr int kTexUnitQueryDepth = 3;
constexpr int kTexUnitQueryNormal = 4;
constexpr int kTexUnitQueryRough = 5;
constexpr int kTexUnitBackface = 6;
constexpr int kTexUnitCount = 7;

constexpr const char* kSsrUpsampleFs = R"glsl(
uniform sampler2D uSsrMip;      // 覆盖率预乘 mip 链：rgb = A 加权平均辐亮度，a = 该级覆盖率
uniform float uMipMaxLod;       // 最大可用 LOD（= 级数 − 1）
uniform float uRoughToLod;      // roughness → LOD 的斜率（面板可调）
uniform sampler2D uDownW;       // **查询**的 SSR 分辨率 R32F：w = 1/z（0 = 空）
uniform sampler2D uDownNormal;  // 查询的 SSR 分辨率 RGBA16F
uniform sampler2D uQueryDepth;  // 全分辨率 R32F 窗口深度（1 = 无数据）
uniform sampler2D uQueryNormal; // 全分辨率 RGBA16F（rgb = 三维视空间法线）
uniform sampler2D uQueryRough;  // 全分辨率 RGBA16F，**只读 .a** = roughness
uniform usampler2D uBackface;   // SSR 分辨率 R8UI：bit0 = 背面命中
uniform ivec2 uFullRes;
uniform vec2  uSsrRes;
uniform int   uScale;
uniform float uNear;
uniform float uFar;
uniform float uRangeDepthRatio;    // **软权尺度**：rel 等于它时权重约减半
uniform float uHardCut;            // rel 超过它 ⇒ 硬拒（数量级不同 = 真跨表面/剪影）
uniform float uWeightFloor;        // 权重下限（相对空间权）⇒ Σw 永不为 0，不出孔洞

layout(location = 0) out vec4 FragReflection;  // rgb = 平均辐亮度, a = 覆盖率 A
layout(location = 1) out uint FragBackface;    // bit0 = 背面命中

// 窗口深度 → 视距（米）。与 ssr_math.h::DepthFromWindowDepth 同一份公式（含 NaN 守卫）。
float ZView(float depth) {
    float ndc = 2.0 * depth - 1.0;
    float denom = (uFar + uNear) - ndc * (uFar - uNear);
    if (!(abs(denom) > 1.0e-6)) {
        return max(uNear, 1.0e-4);
    }
    return (2.0 * uNear * uFar) / denom;
}

// 属性纹理语义（2026-10-31 起）：rgb = **完整三维视空间法线**，a = 掩码编码。
// 不再用 sqrt(1−x²−y²) 重建 z —— 该式在极点（法线≈视轴）附近病态。
vec3 NormalOf(vec4 attr) {
    return normalize(attr.xyz);
}

// 一次取值同时给出「有没有数据」和 w。
// 原来 HasDepthAt + WAtPx 对**同一格**各取一次纹理（梯度最多 4 次取 → 8 次）；
// 两条都是纯函数、同一个 texel，合并后结果逐位相同。
float WOf(ivec2 p, out bool has) {
    float depth = texelFetch(uQueryDepth, p, 0).r;
    has = depth < 1.0;
    return 1.0 / max(ZView(depth), 1.0e-4);
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
    bool has_x = false;
    bool has_y = false;
    float wx = 0.0;
    float wy = 0.0;
    float w_px = WOf(clamp(ip + ivec2(1, 0), ivec2(0), hi), has_x);
    if (has_x) {
        wx = w_px - w_hi;
    } else {
        bool has_mx = false;
        float w_mx = WOf(clamp(ip - ivec2(1, 0), ivec2(0), hi), has_mx);
        wx = has_mx ? (w_hi - w_mx) : 0.0;   // 单侧退回另一侧
    }
    float w_py = WOf(clamp(ip + ivec2(0, 1), ivec2(0), hi), has_y);
    if (has_y) {
        wy = w_py - w_hi;
    } else {
        bool has_my = false;
        float w_my = WOf(clamp(ip - ivec2(0, 1), ivec2(0), hi), has_my);
        wy = has_my ? (w_hi - w_my) : 0.0;
    }
    return vec2(wx, wy);
}

void main() {
    ivec2 ip = ivec2(gl_FragCoord.xy);
    vec4 attr = texelFetch(uQueryNormal, ip, 0);

    // ── A 预乘双边上采样 ──
    // 空间：2×2 双线性权（低分辨率像素为单位）；值域：1/z 相对差 + 法线夹角。
    // **预乘**（w·A·rgb 与 w·A 分别累加）是必须的：否则 A=0 的邻居（rgb=0）会把结果
    // 拉暗，在 A 的边界形成一圈黑边（alpha bleeding）。
    vec2 lo = (vec2(ip) + 0.5) / (vec2(uSsrRes) * float(uScale));  // 低分辨率 uv
    vec2 f = lo * uSsrRes - 0.5;
    ivec2 q0 = ivec2(floor(f));
    vec2 t = f - vec2(q0);

    // 逐像素 roughness（查询那一侧）⇒ roughness → LOD。
    // **线性**映射（不是 α = r²）：r² 会让 0.2~0.4 这一段几乎不糊，而那正是金属/石砖
    // 最常出现的粗糙度。业界对"反射探针按 roughness 选 mip"普遍用近似线性
    // （UE 的 ComputeReflectionCaptureMipFromRoughness 就是线性），这里同口径。
    float rough = clamp(texelFetch(uQueryRough, ip, 0).a, 0.0, 1.0);
    float lod = clamp(rough * uRoughToLod, 0.0, uMipMaxLod);

    float z_hi = ZView(texelFetch(uQueryDepth, ip, 0).r);
    vec3 n_hi = NormalOf(attr);
    float w_hi = 1.0 / max(z_hi, 1.0e-4);
    // w 的屏幕梯度：把「低分格子的 w」换成本像素位置上的**预测值**再比残差，
    // 这样掠射（同一平面相邻行深度差几十个百分点）不会被误判成“跨表面”。
    vec2 gw = WScreenGrad(ip, w_hi);

    vec3 num = vec3(0.0);
    float den = 0.0;
    float wsum = 0.0;
    // 背面 flag 的判定：取**实际贡献颜色最多**的那个 tap（w × a_lo 最大）的硬 flag。
    // 用 w·a_lo 而不是 w：A=0 的相邻 tap 虽然权重大，但对颜色零贡献，让它的 flag 说话
    // 等于用一格"没有答案"的分类去决定整块像素的处置。
    // 为什么是硬判定而不是按权平均：flag 是"这块像素属于哪一类"的语义，平均出来的分数
    // 会让"关着两个 flag 时应当逐字节等于旧行为"这条不变量破掉。
    float wbest_bf = -1.0;
    bool is_backface = false;
    for (int j = 0; j < 2; ++j) {
        for (int i = 0; i < 2; ++i) {
            ivec2 q = clamp(q0 + ivec2(i, j), ivec2(0), ivec2(uSsrRes) - 1);
            float ws = (i == 0 ? 1.0 - t.x : t.x) * (j == 0 ? 1.0 - t.y : t.y);

            // 低分深度已经是 w = 1/z（降采样时换算过）⇒ 这里的值域比较**不需要除法**。
            float w_lo = texelFetch(uDownW, q, 0).r;
            // ⚠️ 必须读**完整三维**法线：以前这里只取 .xy 并把 z 补 0（当时的约定是
            //    "z 由 sqrt 重建"），改存三分量后那段没跟着改 ⇒ 斜面上 dot(n_hi,n_lo)
            //    系统性偏小 ⇒ 边缘权重被压、倒影边缘变暗/出洞。
            vec3 n_lo = NormalOf(texelFetch(uDownNormal, q, 0));

            // ── 值域项：深度 + 法线的**联合软权** ──
            //
            // 深度：拿**预测到本像素位置**的低分 w 比残差（不是拿相邻格子的原始 w 比）。
            // dpx = 本像素中心 → 低分 texel 中心的偏移（全分辨率像素）；scale=1 时恒为 0。
            vec2 dpx = (vec2(q) + 0.5) * float(uScale) - (vec2(ip) + 0.5);
            // 低分深度是**格内最近**（降采样取 min 窗口深度 = max w），所以期望值取
            // 本像素平面上「该格范围内」的**最大 w**：仿射函数在方形格上的极值在角上
            // = 中心值 + 半格·(|gx|+|gy|)。注意是 (uScale−1)：1:1 时格就是一个 texel
            // ⇒ 扩展量必须为 0（否则 1:1 变回归）。
            float w_center = w_hi + dot(gw, dpx);
            float w_pred = w_center + 0.5 * float(uScale - 1) * (abs(gw.x) + abs(gw.y));
            // 共享判据（2026-11-13）：残差与值域权重只有一份实现。
            float rel = SsrRelResidual(w_pred, w_lo);
            float wr = SsrRangeWeight(rel, uRangeDepthRatio);
            bool hard_reject = (rel > uHardCut);   // 数量级不同 ⇒ 真跨表面，硬拒（挡剪影串色）
            // 法线：连续权。0 = 正交（地面 vs 墙），1 = 同向。
            wr *= SsrNormalWeight(n_hi, n_lo);

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
            float w_color = w * a_lo;
            if (w_color > wbest_bf) {
                wbest_bf = w_color;
                is_backface = ((texelFetch(uBackface, q, 0).r & 1u) != 0u);
            }
        }
    }
    float A = (wsum > 0.0) ? (den / wsum) : 0.0;
    vec3 ssr_L = (den > 1.0e-6) ? (num / den) : vec3(0.0);
    FragReflection = vec4(ssr_L, A);
    FragBackface = is_backface ? 1u : 0u;
}
)glsl";

}  // namespace

SsrUpsample::SsrUpsample() = default;
SsrUpsample::~SsrUpsample() { Finalize(); }

void SsrUpsample::Init(ShaderManager* shader_mgr) {
    CHECK(shader_mgr != nullptr);
    if (prog_ != 0) {
        Finalize();
    }
    shader_mgr_ = shader_mgr;
    // 值域项的三个函数（RelResidual / RangeWeight / NormalWeight）来自共享块，
    // 与柱体 pass、融合层同一份 —— 阈值形式漂移是这一族算法最常见的事故。
    const std::string src = std::string(kSsrGlslVersion) + kSsrSameSurfaceGlsl + kSsrUpsampleFs;
    prog_ = shader_mgr_->GetOrCreate("ssr_upsample", {kSsrFullscreenVs, src.c_str(), {}});
    CHECK_NE(prog_, 0u);
}

void SsrUpsample::Finalize() {
    DestroyTargets();
    prog_ = 0;
    shader_mgr_ = nullptr;
}

void SsrUpsample::DestroyTargets() {
    if (fbo_ != 0) {
        glDeleteFramebuffers(1, &fbo_);
        fbo_ = 0;
    }
    if (outputs_.reflection_tex != 0) {
        glDeleteTextures(1, &outputs_.reflection_tex);
        outputs_.reflection_tex = 0;
    }
    if (outputs_.backface_tex != 0) {
        glDeleteTextures(1, &outputs_.backface_tex);
        outputs_.backface_tex = 0;
    }
    outputs_.width = 0;
    outputs_.height = 0;
    width_ = 0;
    height_ = 0;
}

void SsrUpsample::EnsureTargets(int w, int h) {
    if (w == width_ && h == height_ && fbo_ != 0) {
        return;
    }
    DestroyTargets();
    width_ = w;
    height_ = h;

    GLint prev_fbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);

    // ⚠️ 反射图用 **RGBA32F**，不是 RGBA16F：消费端会拿 (rgb, a) 直接做合成
    //    （hdr − A·env + A·L·weight），16F 的量化会在 8-bit 输出的取整边界上翻像素。
    //    这一张是唯一常驻的全分辨率缓冲（1280×720 ≈ 14 MB），换"合成算式可复现"值得。
    glGenTextures(1, &outputs_.reflection_tex);
    glBindTexture(GL_TEXTURE_2D, outputs_.reflection_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, w, h, 0, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    glGenTextures(1, &outputs_.backface_tex);
    glBindTexture(GL_TEXTURE_2D, outputs_.backface_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8UI, w, h, 0, GL_RED_INTEGER, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    glGenFramebuffers(1, &fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           outputs_.reflection_tex, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D,
                           outputs_.backface_tex, 0);
    const GLenum bufs[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
    glDrawBuffers(2, bufs);
    CHECK_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE))
        << "SSR 升采样 FBO 不完整（" << w << "×" << h << "）";
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prev_fbo));

    outputs_.width = w;
    outputs_.height = h;
}

const SsrUpsample::Outputs& SsrUpsample::Run(const Inputs& in) {
    CHECK(shader_mgr_ != nullptr) << "SsrUpsample::Run 前必须先 Init";
    CHECK_NE(in.mip_tex, 0u) << "SsrUpsample::Inputs::mip_tex 未设置";
    CHECK_GE(in.mip_levels, 1) << "SsrUpsample::Inputs::mip_levels 必须 ≥ 1";
    CHECK_NE(in.down_w_tex, 0u) << "SsrUpsample::Inputs::down_w_tex 未设置";
    CHECK_NE(in.down_normal_tex, 0u) << "SsrUpsample::Inputs::down_normal_tex 未设置";
    CHECK_NE(in.query_depth_tex, 0u) << "SsrUpsample::Inputs::query_depth_tex 未设置";
    CHECK_NE(in.query_normal_tex, 0u) << "SsrUpsample::Inputs::query_normal_tex 未设置";
    CHECK_NE(in.query_rough_tex, 0u) << "SsrUpsample::Inputs::query_rough_tex 未设置";
    CHECK_NE(in.backface_tex, 0u) << "SsrUpsample::Inputs::backface_tex 未设置";
    CHECK_GT(in.full_w, 0);
    CHECK_GT(in.full_h, 0);
    CHECK_GT(in.ssr_w, 0);
    CHECK_GT(in.ssr_h, 0);
    CHECK_GE(in.scale, 1);
    CHECK_LE(in.scale, 4);
    CHECK_GT(in.near_plane, 0.0f);
    CHECK_GT(in.far_plane, in.near_plane);
    CHECK_GT(in.depth_tolerance, 0.0f) << "depth_tolerance 必须 > 0（= 0 会让权重变成 inf）";
    CHECK_GT(in.hard_cut, 0.0f);
    CHECK_GE(in.weight_floor, 0.0f);
    CHECK_GE(in.roughness_to_lod, 0.0f);

    EnsureTargets(in.full_w, in.full_h);

    GLint prev_fbo = 0;
    GLint prev_viewport[4] = {0, 0, 0, 0};
    GLint prev_prog = 0;
    GLint prev_active = 0;
    GLint prev_blend = 0;
    GLint prev_depth = 0;
    GLint prev_cull = 0;
    GLint prev_scissor = 0;
    GLint prev_bind[kTexUnitCount] = {0, 0, 0, 0, 0, 0, 0};
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    glGetIntegerv(GL_VIEWPORT, prev_viewport);
    glGetIntegerv(GL_CURRENT_PROGRAM, &prev_prog);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_active);
    prev_blend = glIsEnabled(GL_BLEND);
    prev_depth = glIsEnabled(GL_DEPTH_TEST);
    prev_cull = glIsEnabled(GL_CULL_FACE);
    prev_scissor = glIsEnabled(GL_SCISSOR_TEST);
    for (int u = 0; u < kTexUnitCount; ++u) {
        glActiveTexture(GL_TEXTURE0 + u);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_bind[u]);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glViewport(0, 0, in.full_w, in.full_h);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glUseProgram(prog_);

    glActiveTexture(GL_TEXTURE0 + kTexUnitMip);
    glBindTexture(GL_TEXTURE_2D, in.mip_tex);
    glActiveTexture(GL_TEXTURE0 + kTexUnitDownW);
    glBindTexture(GL_TEXTURE_2D, in.down_w_tex);
    glActiveTexture(GL_TEXTURE0 + kTexUnitDownNormal);
    glBindTexture(GL_TEXTURE_2D, in.down_normal_tex);
    glActiveTexture(GL_TEXTURE0 + kTexUnitQueryDepth);
    glBindTexture(GL_TEXTURE_2D, in.query_depth_tex);
    glActiveTexture(GL_TEXTURE0 + kTexUnitQueryNormal);
    glBindTexture(GL_TEXTURE_2D, in.query_normal_tex);
    glActiveTexture(GL_TEXTURE0 + kTexUnitQueryRough);
    glBindTexture(GL_TEXTURE_2D, in.query_rough_tex);
    glActiveTexture(GL_TEXTURE0 + kTexUnitBackface);
    glBindTexture(GL_TEXTURE_2D, in.backface_tex);

    glUniform1i(shader_mgr_->GetUniform(prog_, "uSsrMip"), kTexUnitMip);
    glUniform1i(shader_mgr_->GetUniform(prog_, "uDownW"), kTexUnitDownW);
    glUniform1i(shader_mgr_->GetUniform(prog_, "uDownNormal"), kTexUnitDownNormal);
    glUniform1i(shader_mgr_->GetUniform(prog_, "uQueryDepth"), kTexUnitQueryDepth);
    glUniform1i(shader_mgr_->GetUniform(prog_, "uQueryNormal"), kTexUnitQueryNormal);
    glUniform1i(shader_mgr_->GetUniform(prog_, "uQueryRough"), kTexUnitQueryRough);
    glUniform1i(shader_mgr_->GetUniform(prog_, "uBackface"), kTexUnitBackface);
    glUniform2i(shader_mgr_->GetUniform(prog_, "uFullRes"), in.full_w, in.full_h);
    glUniform2f(shader_mgr_->GetUniform(prog_, "uSsrRes"), static_cast<float>(in.ssr_w),
                static_cast<float>(in.ssr_h));
    glUniform1i(shader_mgr_->GetUniform(prog_, "uScale"), in.scale);
    glUniform1f(shader_mgr_->GetUniform(prog_, "uNear"), in.near_plane);
    glUniform1f(shader_mgr_->GetUniform(prog_, "uFar"), in.far_plane);
    glUniform1f(shader_mgr_->GetUniform(prog_, "uMipMaxLod"),
                static_cast<float>(in.mip_levels - 1));
    glUniform1f(shader_mgr_->GetUniform(prog_, "uRoughToLod"), in.roughness_to_lod);
    glUniform1f(shader_mgr_->GetUniform(prog_, "uRangeDepthRatio"), in.depth_tolerance);
    glUniform1f(shader_mgr_->GetUniform(prog_, "uHardCut"), in.hard_cut);
    glUniform1f(shader_mgr_->GetUniform(prog_, "uWeightFloor"), in.weight_floor);

    glDrawArrays(GL_TRIANGLES, 0, 3);

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

    return outputs_;
}

}  // namespace jpov
