// JPOV SSR —— GLSL（march + resolve 两趟，算法见 docs/jpov_ssr_v2_design.md）
//
// ⚠️ 本文件的算式与 `src/ssr/ssr_math.h` **必须逐字对应**（注释里指回那边的函数名）。
//    改一处不改另一处 ⇒ `test/ssr/ssr_gpu_test.cc` 里"CPU 参考 vs GPU"那条会红。
//    这是刻意的耦合方式：Shader 与 CPU 各写一遍不可能靠自觉保持一致，只能靠测试对拍。
//
// 三份源：
//   kSsrCommonGlsl  —— uniform 声明 + 射线反构/裁剪 + 判据（**两份 FS 共用**，只此一份）
//   kSsrMarchFs     —— 4W×4H：每 texel = 一个 lane，写本段最早命中的格内中点 s
//   kSsrResolveFs   —— W×H：T² 取 min → s → 命中格 → 取色 + A（可选写调试原因位）
//
// GLSL 版本锁 330 core（JPOV 全项目一致）。**没有 textureGather**（那是 4.00），
// 所以 T² 归约只能逐个 texelFetch —— 见 ssr_renderer.h 里"为什么不做 mip 金字塔"的算式。

#ifndef JPOV_SRC_SSR_SSR_SHADER_H_
#define JPOV_SRC_SSR_SSR_SHADER_H_

namespace jpov {

// 全屏三角形（gl_VertexID，无 VAO/VBO）：裁剪空间直接铺满 [-1,1]²。
inline constexpr const char* kSsrFullscreenVs = R"glsl(
#version 330 core
void main() {
    // 三个顶点 (-1,-1) (3,-1) (-1,3)：一条对角线切开屏幕，比 quad 少一个三角形。
    vec2 p = vec2((gl_VertexID == 1) ? 3.0 : -1.0,
                  (gl_VertexID == 2) ? 3.0 : -1.0);
    gl_Position = vec4(p, 0.0, 1.0);
}
)glsl";

// GLSL 版本头。**必须拼在最前面**（#version 只能出现在第一行），
// 而公共块与 FS 体都不带它 —— 拼装顺序见 ssr_renderer.cc 的 Init。
inline constexpr const char* kSsrGlslVersion = "#version 330 core\n";

// 两份 FS 共用的头部：uniform + 射线反构/裁剪 + 每格判据。**不带 #version**。
inline constexpr const char* kSsrCommonGlsl = R"glsl(
uniform sampler2D uDepth;     // R32F：MRT#1 的 gl_FragCoord.z
uniform sampler2D uNormal;    // 反射体视空间法线 .xy（z 由 sqrt 重建）
uniform vec2  uRes;           // 镜面像素栅格 (W, H)
uniform float uNear;
uniform float uFar;
uniform float uTanHalf;       // tan(垂直 FOV / 2)
uniform float uAspect;
uniform float uMaxDistance;   // 米，射线长度上限（内部再与 uFar 取 min）
uniform float uThicknessC;    // c = N_px · θ_px（ssr_math.h::DeriveParams）
uniform float uMinThickness;  // 米，T 的下限
uniform float uSentinel;      // 无命中哨兵（= ssr::kNoHit）
uniform int   uTile;          // T，tile 边长
uniform float uSlabPixels;    // L = max_ray_pixels / T²（ssr_math.h::DerivedParams::slab_pixels）
uniform int   uStepsPerLane;  // 每段步数上限（含 +3 余量，DDA 到段尾就 break）

// 屏幕射线：σ = 未裁剪线段上的像素弧长，w = 1/d 对 σ 仿射（ssr_math.h::ScreenRay）。
struct SsrRay {
    vec2 s0;
    vec2 dir;
    float span;
    float w0;
    float w1;
    float enter;
    float exit;
    bool exits;
};

// 该像素有没有深度数据（1.0 = 清空值 = 天空）。ssr_math.h::HasDepthData
bool SsrHasDepth(float depth) {
    return depth < 1.0;
}

// 窗口深度 → 视距（米）。ssr_math.h::DepthFromWindowDepth（含 NaN 守卫）。
float SsrZView(float depth) {
    float ndc = 2.0 * depth - 1.0;
    float denom = (uFar + uNear) - ndc * (uFar - uNear);
    if (!(abs(denom) > 1.0e-6)) {
        return max(uNear, 1.0e-4);
    }
    return (2.0 * uNear * uFar) / denom;
}

// σ 处的 1/d。**只在这里做插值，永远不要在 d 空间插值**（§3.1）。
// ssr_math.h::WAt
float SsrWAt(SsrRay ray, float sigma) {
    float t = (ray.span > 0.0) ? sigma / ray.span : 0.0;
    return ray.w0 + (ray.w1 - ray.w0) * t;
}

// 每格判据三行（§5.1）。ssr_math.h::CellHit
bool SsrCellHit(float w_in, float w_out, float z_cell, float thickness) {
    if (!(min(w_in, w_out) > 0.0)) {
        return false;
    }
    float d_lo = 1.0 / max(w_in, w_out);
    float d_hi = 1.0 / min(w_in, w_out);
    if (d_hi < z_cell) {
        return false;                    // 整段在柱子前方
    }
    if (d_lo > z_cell + thickness) {
        return false;                    // 整段在柱子后方
    }
    return true;
}

// mir = 镜面像素下标。返回 false = 射线出屏/退化（ssr_math.h::BuildScreenRay）。
//
// ⚠️ 采样点用 **tile 中心** uv = (mir + 0.5) / uRes，**不是** gl_FragCoord/uTile/uRes：
//    后者会把采样点偏 (T−1)/(2T) 个像素，倒影整体平移 + 边缘一圈脏边。
bool SsrBuildRay(ivec2 mir, out SsrRay ray) {
    float depth = texelFetch(uDepth, mir, 0).r;
    float d_surf = SsrZView(depth);

    vec2 n2 = texelFetch(uNormal, mir, 0).xy;
    vec3 N = vec3(n2, sqrt(max(0.0, 1.0 - dot(n2, n2))));
    N = normalize(N);   // xy 长度可能 > 1（法线贴图越界），不归一化会把 R 拉长

    vec2 uv = (vec2(mir) + 0.5) / uRes;
    vec2 ndc = uv * 2.0 - 1.0;
    vec3 P = vec3(ndc.x * uAspect * uTanHalf, ndc.y * uTanHalf, -1.0) * d_surf;
    vec3 R = reflect(normalize(P), N);
    P += N * (d_surf * 1.0e-3);   // 起点卫生（相对量，与厚度 T 是两件事）

    float len = min(uMaxDistance, uFar);
    if (R.z > 1.0e-9) {
        len = min(len, (-uNear - P.z) / R.z);   // 裁到近平面之前（d → 0 的极点）
    }
    if (!(len > 0.0)) {
        return false;
    }
    vec3 Q = P + R * len;
    float d1 = -Q.z;
    float d0 = -P.z;
    if (!(d1 > 1.0e-6) || !(d0 > 1.0e-6)) {
        return false;
    }

    vec2 nrm0 = vec2(P.x / (d0 * uAspect * uTanHalf), P.y / (d0 * uTanHalf));
    vec2 nrm1 = vec2(Q.x / (d1 * uAspect * uTanHalf), Q.y / (d1 * uTanHalf));
    vec2 s0 = (nrm0 * 0.5 + 0.5) * uRes;
    vec2 s1 = (nrm1 * 0.5 + 0.5) * uRes;

    ray.s0 = s0;
    ray.w0 = 1.0 / d0;
    ray.w1 = 1.0 / d1;

    vec2 delta = s1 - s0;
    ray.span = length(delta);
    if (!(ray.span > 1.0e-6)) {
        return false;
    }
    ray.dir = delta / ray.span;

    // 矩形裁剪（Liang–Barsky），结果用 σ 表示。
    float t0 = 0.0;
    float t1 = ray.span;
    if (abs(delta.x) < 1.0e-9) {
        if (s0.x < 0.0 || s0.x > uRes.x) {
            return false;
        }
    } else {
        float ta = (0.0 - s0.x) / delta.x;
        float tb = (uRes.x - s0.x) / delta.x;
        t0 = max(t0, min(ta, tb) * ray.span);
        t1 = min(t1, max(ta, tb) * ray.span);
    }
    if (abs(delta.y) < 1.0e-9) {
        if (s0.y < 0.0 || s0.y > uRes.y) {
            return false;
        }
    } else {
        float ta = (0.0 - s0.y) / delta.y;
        float tb = (uRes.y - s0.y) / delta.y;
        t0 = max(t0, min(ta, tb) * ray.span);
        t1 = min(t1, max(ta, tb) * ray.span);
    }
    if (!(t1 > t0)) {
        return false;
    }
    ray.enter = t0;
    ray.exit = t1;
    ray.exits = (t1 < ray.span - 1.0e-4);
    return true;
}
)glsl";

// march：每 texel = 一个 lane，输出本段**最早命中格内的中点** s（无命中 = uSentinel）。
inline constexpr const char* kSsrMarchFs = R"glsl(
out float FragS;

void main() {
    ivec2 px = ivec2(gl_FragCoord.xy);
    ivec2 mir = px / uTile;
    ivec2 sub = px - mir * uTile;
    int lane = sub.y * uTile + sub.x;

    FragS = uSentinel;

    // 远平面（清空值）：没有深度数据，不发射线。天空占屏的场景里这一步最省。
    if (!SsrHasDepth(texelFetch(uDepth, mir, 0).r)) {
        return;
    }
    SsrRay ray;
    if (!SsrBuildRay(mir, ray)) {
        return;
    }

    // lane 覆盖 [k·L, (k+1)·L)（切段见 ssr_math.h::MarchSlabs）
    float lane_begin = float(lane) * uSlabPixels;
    if (lane_begin >= ray.exit) {
        return;                       // 短射线：后面的段整段空转
    }
    float start = max(lane_begin, ray.enter);
    float end = min(lane_begin + uSlabPixels, ray.exit);
    if (!(end > start)) {
        return;
    }

    vec2 pos = ray.s0 + ray.dir * start;
    ivec2 cell = ivec2(floor(pos));
    float sigma = start;
    float tmx = 1.0e30;
    float tdx = 1.0e30;
    float tmy = 1.0e30;
    float tdy = 1.0e30;
    if (ray.dir.x > 1.0e-9) {
        tdx = 1.0 / ray.dir.x;
        tmx = sigma + (float(cell.x + 1) - pos.x) / ray.dir.x;
    } else if (ray.dir.x < -1.0e-9) {
        tdx = -1.0 / ray.dir.x;
        tmx = sigma + (float(cell.x) - pos.x) / ray.dir.x;
    }
    if (ray.dir.y > 1.0e-9) {
        tdy = 1.0 / ray.dir.y;
        tmy = sigma + (float(cell.y + 1) - pos.y) / ray.dir.y;
    } else if (ray.dir.y < -1.0e-9) {
        tdy = -1.0 / ray.dir.y;
        tmy = sigma + (float(cell.y) - pos.y) / ray.dir.y;
    }

    // 起点格跳过：只在"起点确实落在反射体表面上"（σ=0）的那一段需要，否则射线会在
    // 自己的柱子里立刻命中自己（ssr_renderer.h 文件头的"待确认"一条）。
    if (start <= 0.0) {
        if (tmx < tmy) {
            sigma = tmx;
            cell.x += (ray.dir.x > 0.0) ? 1 : -1;
            tmx += tdx;
        } else {
            sigma = tmy;
            cell.y += (ray.dir.y > 0.0) ? 1 : -1;
            tmy += tdy;
        }
    }

    for (int i = 0; i < uStepsPerLane; ++i) {
        if (cell.x < 0 || cell.y < 0 || cell.x >= int(uRes.x) || cell.y >= int(uRes.y)) {
            return;
        }
        float sigma_out = min(min(tmx, tmy), end);
        if (!(sigma_out > sigma)) {
            return;
        }
        // 空格子（天空/无深度数据）没有柱子：穿过去，**不要**当成 far 处的墙。
        // 见 ssr_math.h::kEmptyCell 的反例（掠射时会在近地平线造出一大片假高光）。
        float win = texelFetch(uDepth, cell, 0).r;
        if (!SsrHasDepth(win)) {
            if (sigma_out >= end) {
                return;
            }
            sigma = sigma_out;
            if (tmx < tmy) {
                cell.x += (ray.dir.x > 0.0) ? 1 : -1;
                tmx += tdx;
            } else {
                cell.y += (ray.dir.y > 0.0) ? 1 : -1;
                tmy += tdy;
            }
            continue;
        }
        float z = SsrZView(win);
        float thickness = max(uThicknessC * z, uMinThickness);
        if (SsrCellHit(SsrWAt(ray, sigma), SsrWAt(ray, sigma_out), z, thickness)) {
            FragS = 0.5 * (sigma + sigma_out);
            return;
        }
        if (sigma_out >= end) {
            return;
        }
        sigma = sigma_out;
        if (tmx < tmy) {
            cell.x += (ray.dir.x > 0.0) ? 1 : -1;
            tmx += tdx;
        } else {
            cell.y += (ray.dir.y > 0.0) ? 1 : -1;
            tmy += tdy;
        }
    }
}
)glsl";

// resolve：T² 取 min → s → 命中格 → 取色 + A。（打开 SSR_DEBUG 时多写两个调试附件。）
inline constexpr const char* kSsrResolveFs = R"glsl(
uniform sampler2D uSlice;
uniform sampler2D uColor;

layout(location = 0) out vec4 FragRadiance;
#ifdef SSR_DEBUG
layout(location = 1) out uint FragReason;   // 0=HIT 1=SKY 2=OFFSCREEN 3=TOO_SHORT
layout(location = 2) out float FragHitS;    // 像素弧长；无命中 = uSentinel
#endif

void main() {
    ivec2 mir = ivec2(gl_FragCoord.xy);

    // T² 取 min（这一趟本来就要做 s → 命中格 → 取色，min 塞进来 pass 数 +0）。
    float best = uSentinel;
    ivec2 base = mir * uTile;
    for (int j = 0; j < uTile; ++j) {
        for (int i = 0; i < uTile; ++i) {
            best = min(best, texelFetch(uSlice, base + ivec2(i, j), 0).r);
        }
    }

    uint reason = 3u;    // kTooShort
    vec3 rgb = vec3(0.0);
    float alpha = 0.0;
    float hit_s = uSentinel;

    bool have_hit = best < uSentinel * 0.5;
    if (!SsrHasDepth(texelFetch(uDepth, mir, 0).r)) {
        reason = 3u;                       // 无深度数据（天空/背景）
    } else {
        SsrRay ray;
        if (!SsrBuildRay(mir, ray)) {
            reason = 2u;                   // kOffscreen：整条射线在屏幕外
        } else if (have_hit) {
            // s 是**命中格内的中点** ⇒ floor 回去一定是那个命中格（ssr_math.h::CellAtSigma）。
            float s = clamp(best, ray.enter, ray.exit);
            vec2 pos = ray.s0 + ray.dir * s;
            ivec2 cell = ivec2(floor(pos));
            cell = clamp(cell, ivec2(0), ivec2(int(uRes.x) - 1, int(uRes.y) - 1));
            rgb = texelFetch(uColor, cell, 0).rgb;
            alpha = 1.0;
            hit_s = s;
            reason = 0u;                   // kHit
        } else {
            reason = ray.exits ? 2u : 1u;  // 出屏 / 全程在屏内没碰到柱子
        }
    }
    FragRadiance = vec4(rgb, alpha);
#ifdef SSR_DEBUG
    FragReason = reason;
    FragHitS = hit_s;
#endif
}
)glsl";

// ── 属性 pass（接入层用；不属于模块契约）──
// 只画反射体：输出视空间法线 .xy 与 mask(.a=1)。**不写材质**（本版合成层用一套 uniform，
// 见 ssr_integration.h 的"已知限制"）。
//
// 法线在 VS 里算完再传：`mat3(uModel)` 把物体法线转到世界，`uWorldToView` 再转到视空间。
// 两个矩阵都是 per-object / per-frame 的 uniform，由 Renderer 上传。
inline constexpr const char* kSsrAttrVs = R"glsl(
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
uniform mat4 uMVP;
uniform mat4 uModel;
uniform mat3 uWorldToView;
out vec2 vNormalXY;
void main() {
    vec3 n_world = normalize(mat3(uModel) * aNormal);
    vec3 n_view = uWorldToView * n_world;
    vNormalXY = normalize(n_view).xy;
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)glsl";

inline constexpr const char* kSsrAttrFs = R"glsl(
#version 330 core
in vec2 vNormalXY;
out vec4 FragAttr;   // .xy = 视空间法线；.z 预留（roughness）；.a = mask
void main() {
    FragAttr = vec4(vNormalXY, 0.0, 1.0);
}
)glsl";

}  // namespace jpov

#endif  // JPOV_SRC_SSR_SSR_SHADER_H_
