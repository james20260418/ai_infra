// JPOV SSR — 粗糙度预滤波实现。设计与三条细节见 ssr_filter.h 顶部。

#define GL_GLEXT_PROTOTYPES

#include "tools/jpov/src/ssr/ssr_filter.h"

#include <algorithm>
#include <cmath>
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

// level 0：把 (radiance, type) 变成 (rgb, coverage)，并给空像素填一个"邻居的颜色"。
// 填充只改 rgb、不改 a ⇒ 覆盖率语义不变（空像素仍然不是命中）。
constexpr const char* kSsrFilterFillFs = R"glsl(
#version 330 core
uniform sampler2D  uRadiance;
uniform usampler2D uType;
uniform ivec2      uRes;

layout(location = 0) out vec4 FragColor;

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    ivec2 hi = uRes - 1;
    bool hit = (texelFetch(uType, p, 0).r == 1u);   // 只有 kHit 算"有答案"
    vec4 rt = texelFetch(uRadiance, p, 0);
    vec3 c = rt.rgb;
    // A 不再恒为 1：resolve 把"掠射淡出后的置信度"写在 .a 里（2026-11-13）。
    // 老行为（a=1）等价于 .a 恒 1，所以关掉淡出时逐字节不变。
    float a = hit ? clamp(rt.a, 0.0, 1.0) : 0.0;
    if (!hit) {
        // **5×5** 里最近的命中（不是 3×3）：填充半径必须 ≥ 2，否则下一级 mip 的
        // "整块无覆盖"分支平均到的仍是"半亮"的填充色 —— 覆盖率边界上会留下一圈
        // 约 25% 的暗边（3×3 版本实测就是这样）。5×5 的 25 次取色只在 SSR 分辨率
        // 上每帧跑一次，成本可以忽略，而它刚好覆盖 2×2 降采样块的斜对角。
        // 距离相同时按 (j, i) 扫描顺序取第一份（确定性，无随机）。
        float best_d = 1.0e9;
        for (int j = -2; j <= 2; ++j) {
            for (int i = -2; i <= 2; ++i) {
                ivec2 q = clamp(p + ivec2(i, j), ivec2(0), hi);
                if (texelFetch(uType, q, 0).r != 1u) {
                    continue;
                }
                float d = float(i * i + j * j);
                if (d < best_d) {
                    best_d = d;
                    c = texelFetch(uRadiance, q, 0).rgb;
                }
            }
        }
    }
    FragColor = vec4(c, a);
}
)glsl";

// ── 三个遮丑滤波器共用的头部（2026-11-13）──
// 输入统一是 level 0 的 (rgb, a)：a = 覆盖率/置信度，rgb 已经在 fill pass 里补齐过
// （空像素有邻居颜色，所以 A 加权平均不会把颜色拉向黑）。
// 引导：uW = 1/z、uNormal = 视空间法线，都是 SSR 分辨率。
constexpr const char* kSsrFilterGuidesFs = R"glsl(
uniform sampler2D uSrc;
uniform sampler2D uW;
uniform sampler2D uNormal;
uniform usampler2D uType;   // R8UI：1 = kHit。**结构空位与"淡出后的低 A"必须分开判**
uniform ivec2 uRes;
uniform float uDepthTol;    // 深度软权尺度（相对残差）
uniform float uCosMin;      // 法线门限：低于它 = 跨面，权重 0

// 每像素的 1/z 梯度（单侧差分，一侧没数据就退回另一侧）。
// 平面在屏幕空间上 1/z 是仿射的 ⇒ 预测值精确，掠射下不会把同一平面判成两个面。
vec2 SsrWGrad(ivec2 p, float w_c) {
    ivec2 hi = uRes - 1;
    vec2 g;
    float wx1 = texelFetch(uW, clamp(p + ivec2(1, 0), ivec2(0), hi), 0).r;
    if (wx1 > 0.0) {
        g.x = wx1 - w_c;
    } else {
        float wx0 = texelFetch(uW, clamp(p - ivec2(1, 0), ivec2(0), hi), 0).r;
        g.x = (wx0 > 0.0) ? (w_c - wx0) : 0.0;
    }
    float wy1 = texelFetch(uW, clamp(p + ivec2(0, 1), ivec2(0), hi), 0).r;
    if (wy1 > 0.0) {
        g.y = wy1 - w_c;
    } else {
        float wy0 = texelFetch(uW, clamp(p - ivec2(0, 1), ivec2(0), hi), 0).r;
        g.y = (wy0 > 0.0) ? (w_c - wy0) : 0.0;
    }
    return g;
}

// 邻居 q 相对中心 p 的引导权重：0 = 不同表面（不许串色）。
// 邻居没有几何（w = 0）也算 0 —— 那种地方 A 本来就是 0，不该被当成"远处的同一面"。
float SsrGuideWeight(ivec2 p, ivec2 q, float w_c, vec2 gw, vec3 n_c) {
    float w_n = texelFetch(uW, q, 0).r;
    if (!(w_n > 0.0)) {
        return 0.0;
    }
    float nw = 1.0;
    vec3 n_raw = texelFetch(uNormal, q, 0).xyz;
    if (dot(n_raw, n_raw) > 0.25) {
        float c = dot(n_c, normalize(n_raw));
        if (c < uCosMin) {
            return 0.0;
        }
        nw = c;
    }
    float pred = w_c + dot(gw, vec2(q - p));
    float rel = SsrRelResidual(pred, w_n);
    // 软权之外再加一道**硬拒**：跨剪影时 1/(rel+tol) 只是"变小"，长尾会把 2~3 格外的
    // 另一个面拉进来（表现为倒影边缘的浅色渗色）。超过 4× 尺度直接判 0，宁缺毋滥。
    if (rel > 4.0 * uDepthTol) {
        return 0.0;
    }
    return SsrRangeWeight(rel, uDepthTol) * nw;
}

// 同一表面（布尔版，给补洞用）。
// 深度门槛比软权尺度**宽 4 倍**：缝里的邻居本来就该被当成同一面，门槛太紧会让掠射处
// 一条缝都补不上（软权版本里的 1/(rel+tol) 会自然衰减，这里要的是一句"算不算"）。
bool SsrSameSurfaceAt(ivec2 p, ivec2 q, float w_c, vec2 gw, vec3 n_c) {
    float w_n = texelFetch(uW, q, 0).r;
    if (!(w_n > 0.0)) {
        return false;
    }
    vec3 n_raw = texelFetch(uNormal, q, 0).xyz;
    if (dot(n_raw, n_raw) > 0.25 && dot(n_c, normalize(n_raw)) < uCosMin) {
        return false;
    }
    return SsrRelResidual(w_c + dot(gw, vec2(q - p)), w_n) < (uDepthTol * 4.0);
}
)glsl";

// ① 形态学补洞：A < 0.5 的格子，若"左右都命中"或"上下都命中"（且同面），
//    则用这些命中邻居的 A 加权平均补上颜色与覆盖率。宽洞一律不动。
constexpr const char* kSsrFilterHoleFs = R"glsl(
layout(location = 0) out vec4 FragColor;

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec4 s = texelFetch(uSrc, p, 0);
    // 只处理**结构上空**的格子（type != kHit）。命中格哪怕 A 被淡到 0.2 也不碰 ——
    // 否则补洞会把"淡出"好心办坏事地补回 1，两条通路的语义就打架了。
    if (texelFetch(uType, p, 0).r == 1u) {
        FragColor = s;
        return;
    }
    float w_c = texelFetch(uW, p, 0).r;
    if (!(w_c > 0.0)) {
        FragColor = s;      // 没有几何 ⇒ 这不是"反射里的洞"
        return;
    }
    vec3 n_c = texelFetch(uNormal, p, 0).xyz;
    vec2 gw = SsrWGrad(p, w_c);
    ivec2 offs[4] = ivec2[4](ivec2(-1, 0), ivec2(1, 0), ivec2(0, -1), ivec2(0, 1));
    vec4 nb[4];
    bool hit[4];
    for (int k = 0; k < 4; ++k) {
        ivec2 q = clamp(p + offs[k], ivec2(0), uRes - 1);
        nb[k] = texelFetch(uSrc, q, 0);
        hit[k] = (texelFetch(uType, q, 0).r == 1u) && SsrSameSurfaceAt(p, q, w_c, gw, n_c);
    }
    // 只有"两侧都通"才叫缝；单侧命中是真实的边界。
    bool lr = hit[0] && hit[1];
    bool ud = hit[2] && hit[3];
    if (!(lr || ud)) {
        FragColor = s;
        return;
    }
    vec3 acc_c = vec3(0.0);
    float acc_a = 0.0;
    float wsum = 0.0;
    for (int k = 0; k < 4; ++k) {
        if (!hit[k]) {
            continue;
        }
        acc_c += nb[k].a * nb[k].rgb;
        acc_a += nb[k].a * nb[k].a;
        wsum += nb[k].a;
    }
    if (!(wsum > 0.0)) {
        FragColor = s;
        return;
    }
    FragColor = vec4(acc_c / wsum, acc_a / wsum);
}
)glsl";

// ② 方差引导的离群抑制：3×3 加权均值 + 每通道方差，把偏离 k·σ 的部分夹回去。
//    只动"离群"，不动正常样本 ⇒ 锐度几乎不变；A 原样输出（不凭空造覆盖率）。
constexpr const char* kSsrFilterClampFs = R"glsl(
uniform float uClampK;
layout(location = 0) out vec4 FragColor;

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec4 s = texelFetch(uSrc, p, 0);
    // 只有命中格有"颜色"可夹（未命中格的 rgb 是抄来的填充色）。淡出后的低 A 仍是命中。
    if (texelFetch(uType, p, 0).r != 1u) {
        FragColor = s;
        return;
    }
    float w_c = texelFetch(uW, p, 0).r;
    if (!(w_c > 0.0)) {
        FragColor = s;
        return;
    }
    vec3 n_c = texelFetch(uNormal, p, 0).xyz;
    vec2 gw = SsrWGrad(p, w_c);
    // 9 个样本一次取齐（颜色 + 权重）：均值与方差用同一批数据，避免重复取纹理/重复算引导。
    vec3 cols[9];
    float wts[9];
    vec3 mean = vec3(0.0);
    float wsum = 0.0;
    for (int k = 0; k < 9; ++k) {
        ivec2 o = ivec2(k % 3 - 1, k / 3 - 1);
        ivec2 q = clamp(p + o, ivec2(0), uRes - 1);
        vec4 t = texelFetch(uSrc, q, 0);
        float w = t.a * SsrGuideWeight(p, q, w_c, gw, n_c);
        cols[k] = t.rgb;
        wts[k] = w;
        mean += w * t.rgb;
        wsum += w;
    }
    if (!(wsum > 1.0e-6)) {
        FragColor = s;
        return;
    }
    mean /= wsum;
    vec3 var = vec3(0.0);
    for (int k = 0; k < 9; ++k) {
        vec3 d = cols[k] - mean;
        var += wts[k] * d * d;
    }
    var /= wsum;
    vec3 sd = sqrt(max(var, vec3(0.0)));
    vec3 lim = uClampK * sd;
    vec3 c = mean + clamp(s.rgb - mean, -lim, lim);
    FragColor = vec4(c, s.a);
}
)glsl";

// ③ 边缘保持 à-trous（5×5 B3 核，步长 = uStep）。A 当作"样本数"一起平均：
//    颜色按 A 加权、A 按核权平均 ⇒ 梳齿处覆盖率由邻域抬起来，真空洞仍会掉到 0。
constexpr const char* kSsrFilterAtrousFs = R"glsl(
uniform int uStep;
layout(location = 0) out vec4 FragColor;

const float kB3[5] = float[5](1.0, 4.0, 6.0, 4.0, 1.0);

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec4 center = texelFetch(uSrc, p, 0);
    float w_c = texelFetch(uW, p, 0).r;
    if (!(w_c > 0.0)) {
        FragColor = center;
        return;
    }
    vec3 n_c = texelFetch(uNormal, p, 0).xyz;
    vec2 gw = SsrWGrad(p, w_c);
    vec3 acc_c = vec3(0.0);
    float acc_a = 0.0;
    float wsum = 0.0;
    for (int j = -2; j <= 2; ++j) {
        for (int i = -2; i <= 2; ++i) {
            ivec2 o = ivec2(i, j) * uStep;
            ivec2 q = clamp(p + o, ivec2(0), uRes - 1);
            float w = kB3[i + 2] * kB3[j + 2] * SsrGuideWeight(p, q, w_c, gw, n_c);
            if (!(w > 0.0)) {
                continue;
            }
            vec4 t = texelFetch(uSrc, q, 0);
            acc_c += w * t.rgb * t.a;
            acc_a += w * t.a;
            wsum += w;
        }
    }
    if (!(wsum > 1.0e-6)) {
        FragColor = center;
        return;
    }
    float a = acc_a / wsum;
    vec3 c = (acc_a > 1.0e-6) ? (acc_c / acc_a) : center.rgb;
    FragColor = vec4(c, a);
}
)glsl";

// level n → n+1：2×2 box，颜色按覆盖率加权（细节 ①），覆盖率取平均（细节 ③）。
constexpr const char* kSsrFilterDownFs = R"glsl(
#version 330 core
uniform sampler2D uSrc;

layout(location = 0) out vec4 FragColor;

void main() {
    ivec2 src_res = textureSize(uSrc, 0);
    ivec2 q = ivec2(gl_FragCoord.xy) * 2;
    ivec2 hi = src_res - 1;
    vec4 s0 = texelFetch(uSrc, clamp(q + ivec2(0, 0), ivec2(0), hi), 0);
    vec4 s1 = texelFetch(uSrc, clamp(q + ivec2(1, 0), ivec2(0), hi), 0);
    vec4 s2 = texelFetch(uSrc, clamp(q + ivec2(0, 1), ivec2(0), hi), 0);
    vec4 s3 = texelFetch(uSrc, clamp(q + ivec2(1, 1), ivec2(0), hi), 0);

    float sum_a = s0.a + s1.a + s2.a + s3.a;
    vec3 sum_c = s0.rgb * s0.a + s1.rgb * s1.a + s2.rgb * s2.a + s3.rgb * s3.a;
    vec3 avg_c = 0.25 * (s0.rgb + s1.rgb + s2.rgb + s3.rgb);
    // sum_a > 0 ⇒ 只平均有答案的那几格（否则黑像素会把颜色拉暗）；
    // sum_a == 0 ⇒ 整块都没答案，但填充色仍要往下传（下一级双线性才不会插到黑）。
    vec3 rgb = (sum_a > 0.0) ? (sum_c / sum_a) : avg_c;
    FragColor = vec4(rgb, sum_a * 0.25);
}
)glsl";

constexpr int kTexUnitRadiance = 0;
constexpr int kTexUnitType = 1;
constexpr int kTexUnitSrc = 0;
constexpr int kTexUnitW = 1;
constexpr int kTexUnitNormal = 2;
constexpr int kTexUnitTypeTex = 3;
constexpr int kTexUnitCount = 4;

int ComputeLevelCount(int w, int h) {
    int m = std::max(w, h);
    int levels = 1;
    while (m > 1) {
        m >>= 1;
        ++levels;
    }
    return levels;
}

int LevelSize(int base, int level) {
    return std::max(1, base >> level);
}

}  // namespace

SsrFilter::SsrFilter() = default;
SsrFilter::~SsrFilter() { Finalize(); }

void SsrFilter::Init(ShaderManager* shader_mgr) {
    CHECK(shader_mgr != nullptr);
    shader_mgr_ = shader_mgr;
    prog_fill_ = shader_mgr_->GetOrCreate(
        "ssr_filter_fill", {kSsrFullscreenVs, kSsrFilterFillFs, {}});
    prog_down_ = shader_mgr_->GetOrCreate(
        "ssr_filter_down", {kSsrFullscreenVs, kSsrFilterDownFs, {}});
    // 三个遮丑滤波器：版本头 + 共享的同面数学 + 引导函数 + 各自的 main。
    // （kSsrSameSurfaceGlsl 只含纯函数，无 uniform，可直接前置。）
    const std::string head = std::string(kSsrGlslVersion) + kSsrSameSurfaceGlsl +
                             kSsrFilterGuidesFs;
    prog_hole_ = shader_mgr_->GetOrCreate(
        "ssr_filter_hole", {kSsrFullscreenVs, (head + kSsrFilterHoleFs).c_str(), {}});
    prog_clamp_ = shader_mgr_->GetOrCreate(
        "ssr_filter_clamp", {kSsrFullscreenVs, (head + kSsrFilterClampFs).c_str(), {}});
    prog_atrous_ = shader_mgr_->GetOrCreate(
        "ssr_filter_atrous", {kSsrFullscreenVs, (head + kSsrFilterAtrousFs).c_str(), {}});
    CHECK_NE(prog_fill_, 0u);
    CHECK_NE(prog_down_, 0u);
    CHECK_NE(prog_hole_, 0u);
    CHECK_NE(prog_clamp_, 0u);
    CHECK_NE(prog_atrous_, 0u);
}

void SsrFilter::Finalize() {
    DestroyTargets();
    prog_fill_ = 0;
    prog_down_ = 0;
    prog_hole_ = 0;
    prog_clamp_ = 0;
    prog_atrous_ = 0;
    shader_mgr_ = nullptr;
}

void SsrFilter::DestroyTargets() {
    for (unsigned int tex : level_tex_) {
        if (tex != 0) {
            glDeleteTextures(1, &tex);
        }
    }
    level_tex_.clear();
    for (unsigned int& tex : post_tex_) {
        if (tex != 0) {
            glDeleteTextures(1, &tex);
            tex = 0;
        }
    }
    if (fbo_src_ != 0) {
        glDeleteFramebuffers(1, &fbo_src_);
        fbo_src_ = 0;
    }
    if (fbo_dst_ != 0) {
        glDeleteFramebuffers(1, &fbo_dst_);
        fbo_dst_ = 0;
    }
    if (mip_tex_ != 0) {
        glDeleteTextures(1, &mip_tex_);
        mip_tex_ = 0;
    }
    levels_ = 0;
    width_ = 0;
    height_ = 0;
}

void SsrFilter::EnsureTargets(int w, int h) {
    if (mip_tex_ != 0 && width_ == w && height_ == h) {
        return;
    }
    DestroyTargets();
    width_ = w;
    height_ = h;
    levels_ = ComputeLevelCount(w, h);

    glGenTextures(1, &mip_tex_);
    glBindTexture(GL_TEXTURE_2D, mip_tex_);
    for (int l = 0; l < levels_; ++l) {
        glTexImage2D(GL_TEXTURE_2D, l, GL_RGBA16F, LevelSize(w, l), LevelSize(h, l), 0,
                     GL_RGBA, GL_FLOAT, nullptr);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    // 手工定义每一级 ⇒ 必须显式声明最大值，否则"未定义完整 mip 链"的纹理是不完整的：
    // 采样返回黑且**不报错**（这条坑在天空 cubemap 那边也写过一次）。
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, levels_ - 1);
    glBindTexture(GL_TEXTURE_2D, 0);

    level_tex_.resize(levels_, 0);
    for (int l = 0; l < levels_; ++l) {
        glGenTextures(1, &level_tex_[l]);
        glBindTexture(GL_TEXTURE_2D, level_tex_[l]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, LevelSize(w, l), LevelSize(h, l), 0,
                     GL_RGBA, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glBindTexture(GL_TEXTURE_2D, 0);

    // 遮丑滤波器的中转（ping-pong）：只有打开对应开关时才会被写，
    // 但**总是**按当前尺寸建好 —— 免得开关切换那一帧遇到尺寸不匹配。
    for (unsigned int& tex : post_tex_) {
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glBindTexture(GL_TEXTURE_2D, 0);

    glGenFramebuffers(1, &fbo_src_);
    glGenFramebuffers(1, &fbo_dst_);
}

void SsrFilter::Run(unsigned int ssr_radiance_tex, unsigned int ssr_type_tex, int w, int h) {
    Run(ssr_radiance_tex, ssr_type_tex, w, h, SsrFilterGuides{}, SsrFilterConfig{});
}

void SsrFilter::Run(unsigned int ssr_radiance_tex, unsigned int ssr_type_tex, int w, int h,
                    const SsrFilterGuides& guides, const SsrFilterConfig& cfg) {
    CHECK(shader_mgr_ != nullptr) << "SsrFilter::Run 前必须先 Init";
    CHECK_NE(ssr_radiance_tex, 0u);
    CHECK_NE(ssr_type_tex, 0u);
    CHECK_GT(w, 0);
    CHECK_GT(h, 0);
    // 打开任何一条遮丑滤波器都必须有引导纹理：采样未绑定的采样器不可预期，不静默兜底。
    const bool any_post = cfg.hole_fill || cfg.outlier_clamp || cfg.atrous;
    if (any_post) {
        CHECK_NE(guides.down_w_tex, 0u)
            << "SsrFilterConfig 打开了遮丑滤波器，但 guides.down_w_tex 未设置";
        CHECK_NE(guides.down_normal_tex, 0u)
            << "SsrFilterConfig 打开了遮丑滤波器，但 guides.down_normal_tex 未设置";
    }
    CHECK_GE(cfg.atrous_iterations, 1) << "SsrFilterConfig::atrous_iterations 必须 ≥ 1";
    CHECK_LE(cfg.atrous_iterations, 3) << "SsrFilterConfig::atrous_iterations 必须 ≤ 3（步长 1/2/4）";
    // 数值参数一律拦住非法值（NaN 用 "!(x > 0)" 表达，因为 NaN 与任何数比较都为假）。
    CHECK_GT(cfg.clamp_sigma, 0.0f)
        << "SsrFilterConfig::clamp_sigma 必须 > 0，得到 " << cfg.clamp_sigma;
    CHECK_GT(cfg.guide_depth_tol, 0.0f)
        << "SsrFilterConfig::guide_depth_tol 必须 > 0（= 0 会让引导权重变成 inf/NaN），得到 "
        << cfg.guide_depth_tol;
    CHECK_GE(cfg.guide_normal_cos, 0.0f) << "SsrFilterConfig::guide_normal_cos 必须 ∈ [0,1]，得到 "
                                         << cfg.guide_normal_cos;
    CHECK_LE(cfg.guide_normal_cos, 1.0f) << "SsrFilterConfig::guide_normal_cos 必须 ∈ [0,1]，得到 "
                                         << cfg.guide_normal_cos;
    cur_guides_ = guides;
    cur_cfg_ = cfg;
    cur_type_tex_ = ssr_type_tex;

    GLint prev_read_fbo = 0;
    GLint prev_draw_fbo = 0;
    GLint prev_viewport[4] = {0, 0, 0, 0};
    GLint prev_prog = 0;
    GLint prev_active = 0;
    GLint prev_blend = 0;
    GLint prev_depth = 0;
    GLint prev_cull = 0;
    GLint prev_scissor = 0;
    GLint prev_bind[kTexUnitCount] = {};
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read_fbo);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_draw_fbo);
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

    EnsureTargets(w, h);

    glBindFramebuffer(GL_FRAMEBUFFER, fbo_src_);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);

    // ── level 0 ──
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, level_tex_[0], 0);
    CHECK_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE))
        << "SSR 预滤波 FBO 不完整（level 0，" << w << "×" << h << "）";
    glViewport(0, 0, w, h);
    glUseProgram(prog_fill_);
    glActiveTexture(GL_TEXTURE0 + kTexUnitRadiance);
    glBindTexture(GL_TEXTURE_2D, ssr_radiance_tex);
    glUniform1i(shader_mgr_->GetUniform(prog_fill_, "uRadiance"), kTexUnitRadiance);
    // 类型图是 usampler2D：单独占单元 + 单独 glUniform1i（"基址+下标"的坑）。
    glActiveTexture(GL_TEXTURE0 + kTexUnitType);
    glBindTexture(GL_TEXTURE_2D, ssr_type_tex);
    glUniform1i(shader_mgr_->GetUniform(prog_fill_, "uType"), kTexUnitType);
    glUniform2i(shader_mgr_->GetUniform(prog_fill_, "uRes"), w, h);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glActiveTexture(GL_TEXTURE0 + kTexUnitType);
    glBindTexture(GL_TEXTURE_2D, 0);

    // ── 遮丑滤波器（2026-11-13）：level 0 → 若干趟 → 得到 mip 链的 level 0 ──
    // 顺序固定：补洞（形态学）→ 离群抑制（统计）→ à-trous（多尺度）。
    // 每一趟都是 src → post[k]，再翻转 ping-pong ⇒ 永不"边采样边写"。
    unsigned int lvl0_tex = level_tex_[0];
    int ping = 0;
    if (cur_cfg_.hole_fill) {
        PassToTexture(prog_hole_, lvl0_tex, post_tex_[ping], /*step=*/0);
        lvl0_tex = post_tex_[ping];
        ping ^= 1;
    }
    if (cur_cfg_.outlier_clamp) {
        PassToTexture(prog_clamp_, lvl0_tex, post_tex_[ping], /*step=*/0);
        lvl0_tex = post_tex_[ping];
        ping ^= 1;
    }
    if (cur_cfg_.atrous) {
        int step = 1;
        for (int it = 0; it < cur_cfg_.atrous_iterations; ++it) {
            PassToTexture(prog_atrous_, lvl0_tex, post_tex_[ping], step);
            lvl0_tex = post_tex_[ping];
            ping ^= 1;
            step *= 2;
        }
    }

    // ── level 1..n ──（读写不同纹理，无反馈环）
    glUseProgram(prog_down_);
    glUniform1i(shader_mgr_->GetUniform(prog_down_, "uSrc"), kTexUnitSrc);
    for (int l = 1; l < levels_; ++l) {
        const int lw = LevelSize(w, l);
        const int lh = LevelSize(h, l);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                               level_tex_[l], 0);
        CHECK_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER),
                 static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE))
            << "SSR 预滤波 FBO 不完整（level " << l << "，" << lw << "×" << lh << "）";
        glViewport(0, 0, lw, lh);
        glActiveTexture(GL_TEXTURE0 + kTexUnitSrc);
        glBindTexture(GL_TEXTURE_2D, (l == 1) ? lvl0_tex : level_tex_[l - 1]);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    // ── 拼进 mip_tex_ 的各 level（blit：读端写端是两个不同 FBO，无反馈环）──
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo_src_);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo_dst_);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    for (int l = 0; l < levels_; ++l) {
        const int lw = LevelSize(w, l);
        const int lh = LevelSize(h, l);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo_src_);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                               (l == 0) ? lvl0_tex : level_tex_[l], 0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo_dst_);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                               mip_tex_, l);
        CHECK_EQ(glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER),
                 static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE))
            << "SSR 预滤波的 mip 目标 FBO 不完整（level " << l << "）";
        glBlitFramebuffer(0, 0, lw, lh, 0, 0, lw, lh, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
    // 拼完把 dst 上的挂载摘掉，避免下一帧误用（句柄还在，只是不再挂在 FBO 上）。
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(prev_read_fbo));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(prev_draw_fbo));

    // 恢复调用方状态
    for (int u = 0; u < kTexUnitCount; ++u) {
        glActiveTexture(GL_TEXTURE0 + u);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prev_bind[u]));
    }
    glActiveTexture(static_cast<GLenum>(prev_active));
    glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);
    glUseProgram(static_cast<GLuint>(prev_prog));
    if (prev_blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (prev_depth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (prev_cull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    if (prev_scissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
}

void SsrFilter::PassToTexture(unsigned int prog, unsigned int src_tex, unsigned int dst_tex,
                              int step) {
    CHECK_NE(prog, 0u);
    CHECK_NE(src_tex, 0u);
    CHECK_NE(dst_tex, 0u);
    CHECK_NE(src_tex, dst_tex) << "PassToTexture：src 与 dst 不能是同一张纹理（反馈环）";
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_src_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dst_tex, 0);
    CHECK_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE))
        << "SSR 遮丑滤波 FBO 不完整（" << width_ << "×" << height_ << "）";
    glViewport(0, 0, width_, height_);
    glUseProgram(prog);
    glActiveTexture(GL_TEXTURE0 + kTexUnitSrc);
    glBindTexture(GL_TEXTURE_2D, src_tex);
    glUniform1i(shader_mgr_->GetUniform(prog, "uSrc"), kTexUnitSrc);
    glActiveTexture(GL_TEXTURE0 + kTexUnitW);
    glBindTexture(GL_TEXTURE_2D, cur_guides_.down_w_tex);
    glUniform1i(shader_mgr_->GetUniform(prog, "uW"), kTexUnitW);
    glActiveTexture(GL_TEXTURE0 + kTexUnitNormal);
    glBindTexture(GL_TEXTURE_2D, cur_guides_.down_normal_tex);
    glUniform1i(shader_mgr_->GetUniform(prog, "uNormal"), kTexUnitNormal);
    glUniform2i(shader_mgr_->GetUniform(prog, "uRes"), width_, height_);
    glUniform1f(shader_mgr_->GetUniform(prog, "uDepthTol"), cur_cfg_.guide_depth_tol);
    glUniform1f(shader_mgr_->GetUniform(prog, "uCosMin"), cur_cfg_.guide_normal_cos);
    glUniform1f(shader_mgr_->GetUniform(prog, "uClampK"), cur_cfg_.clamp_sigma);
    glActiveTexture(GL_TEXTURE0 + kTexUnitTypeTex);
    glBindTexture(GL_TEXTURE_2D, cur_type_tex_);
    glUniform1i(shader_mgr_->GetUniform(prog, "uType"), kTexUnitTypeTex);
    glUniform1i(shader_mgr_->GetUniform(prog, "uStep"), step);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glActiveTexture(GL_TEXTURE0 + kTexUnitW);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0 + kTexUnitNormal);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0 + kTexUnitTypeTex);
    glBindTexture(GL_TEXTURE_2D, 0);
}

}  // namespace jpov
