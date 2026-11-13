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
// ── 共享的「同面判定」数学（2026-11-13）──
//
// 这个模型有三处用到（以前各写一份，阈值/形式容易漂）：
//   ① 体素柱的同面并集（ssr_integration.cc 的 ssr_slab_back pass）
//   ② 融合层双边升采样的值域项（ssr_fusion.cc）
//   ③ 掠射淡出里对 Q 的两个视角比较（同一套"预测—残差"思路）
// 只放与分辨率/取纹理无关的纯函数；调用方自己算好角射线与预测值再传进来。
inline constexpr const char* kSsrSameSurfaceGlsl = R"glsl(
float SsrRelResidual(float w_pred, float w_actual) {
    return abs(w_actual - w_pred) / max(abs(w_pred), 1.0e-9);
}

float SsrRangeWeight(float rel, float tol) {
    return 1.0 / (rel + tol);
}

float SsrNormalWeight(vec3 n0, vec3 n1) {
    return clamp(dot(n0, n1), 0.0, 1.0);
}

bool SsrSameSurface(float w_pred, float w_actual, vec3 n0, vec3 n1,
                    float rel_max, float cos_min) {
    if (!(w_pred > 0.0) || !(w_actual > 0.0)) {
        return false;
    }
    if (dot(n0, n1) < cos_min) {
        return false;
    }
    return SsrRelResidual(w_pred, w_actual) < rel_max;
}

// inv_res = 1/res（宿主预算；把每像素的除法换成乘法）
vec3 SsrCellRay(ivec2 p, vec2 inv_res, float aspect_tan, float tan_half) {
    vec2 ndc = (vec2(p) + 0.5) * inv_res * 2.0 - 1.0;
    return vec3(ndc.x * aspect_tan, ndc.y * tan_half, -1.0);
}

// 平面 (n, P) 与角射线 c 的交点视距；<= 0 = 退化。
float SsrPlaneViewDist(vec3 n, vec3 P, vec3 c) {
    float den = dot(n, c);
    if (abs(den) < 1.0e-7) {
        return 0.0;
    }
    float t = dot(n, P) / den;
    return (t > 1.0e-4) ? t : 0.0;
}
)glsl";

inline constexpr const char* kSsrCommonGlsl = R"glsl(
// R32F：**w = 1/z_view**（不是窗口深度）。0 = 没有几何（天空/背景）。
// SSR 内部一律用 w：判据全是"1/d 落在哪个区间"，而 1/d 对屏幕坐标仿射 ⇒ 每格零除法。
uniform sampler2D uW;
uniform sampler2D uNormal;    // 视空间法线 **rgb 三分量**（.a = 掩码：1/0.5/0 无数据）
// 反射层**逐像素** roughness（R16F，降采样后）。2026-11-02 起材质进属性纹理，
// 不再有"全场景一套粗糙度"；这里只用于一件事：超过 uRoughnessCutoff 的像素**不发射线**。
uniform sampler2D uRough;
uniform float uRoughnessCutoff;   // [0,1]；1 = 永不跳过（等价于旧行为）
uniform vec2  uRes;           // 镜面像素栅格 (W, H)
uniform float uNear;
uniform float uFar;
uniform float uTanHalf;       // tan(垂直 FOV / 2)
uniform float uAspect;
uniform float uMaxDistance;   // 米，射线长度上限（内部再与 uFar 取 min）
uniform float uThicknessC;    // c = N_px · θ_px（ssr_math.h::DeriveParams）
uniform float uInvTanHalf;    // 1/uTanHalf（同上；把 1/(d·tan) 变成 w·本体
uniform sampler2D uSlabBack;  // 同面并集后的柱体 back（w 空间，R32F，SSR 分辨率；无条件由 ②b pass 产出）
uniform float uMinThickness;  // 米，T 的下限
uniform float uSentinel;      // 无命中哨兵（= ssr::kNoHit）
uniform int   uTile;          // T，tile 边长
uniform float uSlabPixels;    // L = max_ray_pixels / T²（ssr_math.h::DerivedParams::slab_pixels）
uniform int   uStepsPerLane;  // 每段步数上限（含 +3 余量，DDA 到段尾就 break）
// 屏幕预算（SSR 像素弧长）。宿主侧用它推 DerivedParams（每段长度 / 步数上限），
// 着色器内已不再消费（原来的 ray_ratio 输出 2026-11-13 删除）。
uniform float uMaxRayPixels;

// 背面命中的编码偏移（2026-11-13）。march 每 lane 只能输出一个 float：
//   s ∈ [0, kSsrBackfaceBias) = 正面命中；s + kSsrBackfaceBias = 只有背面命中；
//   uSentinel = 什么都没撞到。resolve 对 T² 取 min ⇒ 正面命中天然优先，
//   两个优先级共用一次归约（零额外通道 / 零额外 pass）。
// 取值约束：远大于任何可能的 s（屏幕对角线 < 1e4），远小于 uSentinel（1e30）。
// **必须与 ssr_math.h::kBackfaceBias 一致**（CPU 镜像对拍用）。
const float kSsrBackfaceBias = 1.0e5;

// 屏幕射线：σ = 未裁剪线段上的像素弧长，w = 1/d 对 σ 仿射（ssr_math.h::ScreenRay）。
struct SsrRay {
    vec2 s0;
    vec2 dir;
    vec3 d3;          // 视空间单位方向（朝向判据用；ssr_math.h::ScreenRay::dir3）
    float span;
    float inv_span;   // 1/span（预算好：SsrWAt 每格调用两次，去掉内层两次除法）
    float w0;
    float w1;
    float enter;
    float exit;
};

// 该格有没有几何。**w = 0 是唯一的"空"表示**（降采样时把天空/远平面写成 0），
// 这样 CPU 参考与着色器不会在"far 处到底是天空还是墙"上分叉。
bool SsrHasDepth(float w) {
    return w > 0.0;
}

// 法线：属性纹理存**完整三维视空间法线**（rgb），这里只归一化。
//
// ⚠️ 2026-10-31 根因（掠射倒影"抬高一米 / 两层 / 红移 / 压扁"）：以前只存 .xy、z 用
// `sqrt(1−x²−y²)` 重建 —— 这个式子在**极点附近病态**。掠射时地面法线 ny→1，fp16 把
// 1.0 存成 0.999512（实测 16×16 邻域 256/256 像素），重建出 z = 0.0312 ⇒ 法线凭空偏
// **1.79°** ⇒ 反射方向偏 **3.6°**，而真实掠射角只有 1.3°（放大近 3 倍）⇒ 倒影射线被抬高
// 约 1 米、命中错像素。存三分量后单分量 fp16 误差 5e-4 ⇒ 方向误差 ~0.03°（差 60 倍）。
vec3 SsrNormalOf(vec4 attr) {
    return normalize(attr.xyz);
}

// 该镜面像素的 roughness（逐像素材质）。> uRoughnessCutoff ⇒ 本像素不发射线：
// 粗糙表面的真实答案是一个**宽积分**，单条镜面射线对它没有意义（业界做法同：
// FidelityFX SSSR 的 tile classification 直接不给高粗糙像素发射线，交回 env 探针）。
float SsrRoughnessAt(ivec2 mir) {
    return texelFetch(uRough, mir, 0).r;
}

// σ 处的 1/d。**只在这里做插值，永远不要在 d 空间插值**（§3.1）。
// ssr_math.h::WAt
float SsrWAt(SsrRay ray, float sigma) {
    // span 在 SsrBuildRay 里已校验 > 1e-6，inv_span 一定有限 ⇒ 内层无除法。
    float t = sigma * ray.inv_span;
    return ray.w0 + (ray.w1 - ray.w0) * t;
}

// 每格判据（§5.1）。ssr_math.h::CellHit 逐字同一形式。
//
//   d_hi ≥ z      ⟺  w_min ≤ w_cell
//   d_lo ≤ z + T  ⟺  w_max·(1+c) ≥ w_cell      （T = c·z ⇒ 1/(z+T) = z⁻¹/(1+c)）
// 老模型（只有 c·T 时）两条都是比较/乘法、内层零除法；2026-11-13 加入柱体 back 后
// 每格只剩 1 次除法（见下面的代数改写），其余仍是比较。
// cell 是本格坐标：要取那一格的柱体 back。
bool SsrCellHit(float w_in, float w_out, float w_cell, ivec2 cell) {
    float w_min = min(w_in, w_out);
    float w_max = max(w_in, w_out);
    if (!(w_min > 0.0) || !(w_cell > 0.0)) {
        return false;
    }
    if (w_min > w_cell) {
        return false;                    // 整段在柱子前方
    }
    // ── 体素柱（2026-11-13 定稿）：front = w_cell（真实最近深度），back 取三者最厚 ──
    //   ① 同面并集（uSlabBack）      ② c·T：back = w/(1+c)
    //   ③ 全局最小厚度 t_min：back = 1/(1/w + t_min)
    // 三者都只往远处扩 ⇒ 取 min（w 越小越远）。区间 [w_min,w_max] ∩ [back,w_cell] 即命中。
    float back = texelFetch(uSlabBack, cell, 0).r;
    // 三源取最厚：同面并集 / c·T / 全局最小厚度。三者都只往远处扩 ⇒ back 取 min。
    //
    // 2026-11-13：判据**交叉相乘**写成比较，内层彻底无除法（原来每格 3 次）。
    //   w_max ≥ back_ct   ⟺ w_max·(1+c) ≥ w_cell            （T = c·z）
    //   w_max ≥ back_tmin ⟺ w_max·(1 + t·w_cell) ≥ w_cell   （1/(1/w+t) = w/(1+t·w)，两边乘正数）
    //   w_max ≥ back_slab ⟺ 直接比较（纹理值）
    // 三者取"或"（任意一源认为命中即命中），与"back 取 min 后比较"完全等价。
    if (back > 0.0 && w_max >= back) {
        return true;
    }
    if (w_max * (1.0 + uThicknessC) >= w_cell) {
        return true;
    }
    return w_max * (1.0 + uMinThickness * w_cell) >= w_cell;
}

// 命中面朝向判据（2026-10-30 第一档）。ssr_math.h::HitFacesRay
//
//   R·N < 0 ⇒ 射线**扎进**命中面 = 合法的正面命中；
//   R·N ≥ 0 ⇒ 射线是从这个面的背面/内部穿出来的 ⇒ 不算命中，继续 march。
//
// 为什么必须有它：深度缓冲里只有"相机看得见的那一面"的颜色与法线。镜子往上看桌子
// 底面 / 柱体背面时，屏幕空间里没有那些面的像素，旧判据会把**桌面顶面的高光**当结果
// ⇒ 倒影里桌子底下也在发光。
//
// attr.a ≥ 0.25 ⇒ 这一格有视空间法线（属性 pass 画过几何）。没有数据（天空 / 未纳入
// 属性 pass 的几何）时不判、维持旧行为（见 ssr_renderer.h "已知边界"）。
bool SsrHitFacing(vec3 ray_dir, ivec2 cell) {
    vec4 attr = texelFetch(uNormal, cell, 0);
    if (attr.a < 0.25) {          // .a = 掩码编码：1 反射体 / 0.5 有法线非反射体 / 0 无数据
        return true;              // 没有法线数据 ⇒ 不判方向（维持引入判据之前的行为）
    }
    return dot(ray_dir, SsrNormalOf(attr)) < 0.0;
}

// mir = 镜面像素下标。返回 false = 射线出屏/退化（ssr_math.h::BuildScreenRay）。
//
// ⚠️ 采样点用 **tile 中心** uv = (mir + 0.5) / uRes，**不是** gl_FragCoord/uTile/uRes：
//    后者会把采样点偏 (T−1)/(2T) 个像素，倒影整体平移 + 边缘一圈脏边。
bool SsrBuildRay(ivec2 mir, out SsrRay ray) {
    float w_surf = texelFetch(uW, mir, 0).r;
    float d_surf = 1.0 / w_surf;   // w = 1/z ⇒ z = 1/w（每像素一次除法）

    vec3 N = SsrNormalOf(texelFetch(uNormal, mir, 0));

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

    // 2026-11-13：inv = 1/(d·tan) = w·(1/tan) —— 复用已算的 w0/w1，两条除法变乘法。
    float w0_ = 1.0 / d0;
    float w1_ = 1.0 / d1;
    float inv0 = w0_ * uInvTanHalf;
    float inv1 = w1_ * uInvTanHalf;
    vec2 nrm0 = vec2(P.x / uAspect, P.y) * inv0;
    vec2 nrm1 = vec2(Q.x / uAspect, Q.y) * inv1;
    vec2 s0 = (nrm0 * 0.5 + 0.5) * uRes;
    vec2 s1 = (nrm1 * 0.5 + 0.5) * uRes;

    ray.s0 = s0;
    ray.w0 = w0_;
    ray.w1 = w1_;
    // R 已经是单位向量：reflect(I, N) 在 |I| = |N| = 1 时保长度，因此**不再 normalize**
    //（一次 rsqrt 省下来；四个 lane 各建一次射线 ⇒ 每像素省 4 次）。
    ray.d3 = R;   // 朝向判据用（ssr_math.h::BuildScreenRay 同一行）

    vec2 delta = s1 - s0;
    ray.span = length(delta);
    if (!(ray.span > 1.0e-6)) {
        return false;
    }
    ray.inv_span = 1.0 / ray.span;   // 只在这里除一次；SsrWAt 用乘法
    ray.dir = delta * ray.inv_span;

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
    return true;
}

// 反射方向的**灭点**（屏幕像素坐标）。反射方向指向无穷远时，沿射线的所有点都收敛到它。
// 返回 false = 没有灭点（R.z ≥ 0，反射方向朝相机/身后）。
bool SsrVanishingPoint(vec3 dir_view, out vec2 vp) {
    if (dir_view.z >= -1.0e-6) {
        return false;
    }
    vec2 ndc = vec2(dir_view.x / (-dir_view.z * uAspect * uTanHalf),
                    dir_view.y / (-dir_view.z * uTanHalf));
    vp = (ndc * 0.5 + 0.5) * uRes;
    return true;
}

)glsl";

// march：每 texel = 一个 lane，输出本段**最早命中格内的中点** s。
//
// 编码（2026-11-13）：
//   s ∈ [0, bias)   正面命中（真实命中，优先）
//   s + bias        只有背面命中（射线从面背后穿出，见 SsrHitFacing）
//   uSentinel       什么都没撞到
// resolve 对 T² 个 lane 取 min ⇒ 正面命中天然优先，背面命中只是"没有正面命中时的退路"。
//
// **背面命中一律按命中输出颜色**（2026-11-13 Danis 定）：模块只报事实，不替调用方决定
// 该不该用它。用 env 还是用这个颜色、要不要压暗，全在融合层（两个正交 flag）。
inline constexpr const char* kSsrMarchFs = R"glsl(
out float FragS;

// 一条 lane 的 DDA。
//
// 抽成函数只为一件事：背面候选必须在**每一个出口**都写出去。原地有 5 处 return
// （出屏 / 退化 / 段尾 / 命中），逐处改写极易漏掉某条分支 —— 而"漏一条"的表现是
// 偶发的整行回落 env，极难定位。这里统一成 result/backface 两个变量 + 单出口。
float SsrMarchLane(SsrRay ray, int lane) {
    float result = uSentinel;
    float backface = -1.0;   // 本 lane 最早的"背面命中"候选（<0 = 没有）

    // lane 覆盖 [k·L, (k+1)·L)（切段见 ssr_math.h::MarchSlabs）
    float lane_begin = float(lane) * uSlabPixels;
    if (lane_begin >= ray.exit) {
        return result;                       // 短射线：后面的段整段空转
    }
    float start = max(lane_begin, ray.enter);
    float end = min(lane_begin + uSlabPixels, ray.exit);
    if (!(end > start)) {
        return result;
    }

    vec2 pos = ray.s0 + ray.dir * start;
    ivec2 cell = ivec2(floor(pos));
    float sigma = start;
    float tmx = 1.0e30;
    float tdx = 1.0e30;
    float tmy = 1.0e30;
    float tdy = 1.0e30;
    // 2026-11-13：先算 1/dir，再用乘法（原来每个分量多除一次）。
    if (ray.dir.x > 1.0e-9) {
        tdx = 1.0 / ray.dir.x;
        tmx = sigma + (float(cell.x + 1) - pos.x) * tdx;
    } else if (ray.dir.x < -1.0e-9) {
        tdx = -1.0 / ray.dir.x;
        tmx = sigma + (pos.x - float(cell.x)) * tdx;
    }
    if (ray.dir.y > 1.0e-9) {
        tdy = 1.0 / ray.dir.y;
        tmy = sigma + (float(cell.y + 1) - pos.y) * tdy;
    } else if (ray.dir.y < -1.0e-9) {
        tdy = -1.0 / ray.dir.y;
        tmy = sigma + (pos.y - float(cell.y)) * tdy;
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
            break;
        }
        float sigma_out = min(min(tmx, tmy), end);
        if (!(sigma_out > sigma)) {
            break;
        }
        // 空格子（天空/无深度数据）没有柱子：穿过去，**不要**当成 far 处的墙。
        // 见 ssr_math.h::kEmptyCell 的反例（掠射时会在近地平线造出一大片假高光）。
        float w_cell = texelFetch(uW, cell, 0).r;
        if (!SsrHasDepth(w_cell)) {
            if (sigma_out >= end) {
                break;
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
        if (SsrCellHit(SsrWAt(ray, sigma), SsrWAt(ray, sigma_out), w_cell, cell)) {
            float s_mid = 0.5 * (sigma + sigma_out);
            if (SsrHitFacing(ray.d3, cell)) {
                result = s_mid;              // 正经的正面命中：立刻结束（优先级最高）
                break;
            }
            if (backface < 0.0) {
                backface = s_mid;            // 记下最早的背面命中，继续找正面命中
            }
        }
        if (sigma_out >= end) {
            break;
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
    // ⚠️ 正面命中优先：result 一旦被置上，就必须原样返回（它本来就在 break 之前写好）。
    // 这里曾经写成"先看 backface"，结果同一个 lane 里既有正面命中又有背面命中时会把
    // 背面那个报出去 —— 表现为"该走的远命中变成了近处的背面命中"，只有对拍抓得到。
    if (result < uSentinel * 0.5) {
        return result;
    }
    if (backface >= 0.0) {
        return backface + kSsrBackfaceBias;
    }
    return result;
}

void main() {
    ivec2 px = ivec2(gl_FragCoord.xy);
    ivec2 mir = px / uTile;
    ivec2 sub = px - mir * uTile;
    int lane = sub.y * uTile + sub.x;

    FragS = uSentinel;

    // 远平面（清空值）：没有深度数据，不发射线。天空占屏的场景里这一步最省。
    if (!SsrHasDepth(texelFetch(uW, mir, 0).r)) {
        return;
    }
    // 太粗糙 ⇒ 不发射线（预算省下来）：单条镜面射线表达不了宽反射瓣，交给 env 的 mip。
    if (SsrRoughnessAt(mir) > uRoughnessCutoff) {
        return;
    }
    SsrRay ray;
    if (!SsrBuildRay(mir, ray)) {
        return;
    }
    FragS = SsrMarchLane(ray, lane);
}
)glsl";

// resolve：T² 取 min → 命中格 → 取色 + A（置信度）+ 类型 + 背面 flag。
//
// 输出（四样，紧凑排列）：
//   location 0  FragRadiance  RGBA16F  rgb = 命中颜色辐亮度, a = **A（置信度）**
//                                 命中 = 1（掠射淡出时 < 1）、非命中 = 0。
//                                 （2026-11-13 起 ray_ratio 不再输出：预算占用率没人消费）
//   location 1  FragType      R8UI     0=None 1=Hit 2=Sky 3=Offscreen 4=Exhausted
//   location 2  FragHitS      R32F     调试：命中位置的像素弧长（SSR_DEBUG 时才有）
//
// A（"结果有多可信"）走 radiance.a：2026-11-13 起由 resolve 写入（掠射淡出乘在这里）。
inline constexpr const char* kSsrResolveFs = R"glsl(
uniform sampler2D uSlice;
uniform sampler2D uColor;
uniform float uGrazingFadeStart;   // kappa 起淡
uniform float uGrazingFadeEnd;     // kappa 全淡
// A 的三个淡出（2026-11-13）。单位一律 **SSR 像素**（接入层把屏幕像素 / N）。
// 0 = 关闭该条；三条都是乘法，互不干扰。语义与曲线见 ssr_math.h 的 ②.5 节。
uniform float uFadeBudget;         // ① 命中点离 march 预算还有多远（P→Q = s）
uniform float uFadeEdge;           // ② 反射体像素 P 贴边
uniform float uFadeHitEdge;        // ③ 命中点 Q 贴边

// 剩余可用距离 → 权重。width ≤ 0 ⇒ 1（关闭）。
float SsrFadeWeight(float remain_px, float width_px) {
    if (!(width_px > 0.0)) {
        return 1.0;
    }
    float t = clamp(remain_px / width_px, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

// 到最近画面边的像素距离（uRes = SSR 栅格尺寸）。
float SsrDistToEdge(vec2 p) {
    return min(min(p.x, uRes.x - p.x), min(p.y, uRes.y - p.y));
}

// 命中点 s 处的 A（ssr_math.h::HitConfidence 的逐字镜像）。
float SsrHitConfidence(SsrRay ray, float s) {
    float a = SsrFadeWeight(uMaxRayPixels - s, uFadeBudget);
    if (uFadeEdge > 0.0) {
        a *= SsrFadeWeight(SsrDistToEdge(ray.s0), uFadeEdge);
    }
    if (uFadeHitEdge > 0.0) {
        a *= SsrFadeWeight(SsrDistToEdge(ray.s0 + ray.dir * s), uFadeHitEdge);
    }
    return a;
}

layout(location = 0) out vec4 FragRadiance;
layout(location = 1) out uint FragType;
// R8UI，位置 3（位置 2 留给 SSR_DEBUG 的 hit_s）：bit0 = 本次命中是**背面命中**。
// 模块只报事实；"用 env 还是用这个颜色 / 要不要压暗"由融合层的两个正交 flag 决定。
layout(location = 3) out uint FragBackface;
#ifdef SSR_DEBUG
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

    uint type = 0u;            // kNone
    vec3 rgb = vec3(0.0);
    // 输出给滤波层的 **A**（覆盖率/置信度）：命中=1，掠射淡出时 <1，非命中=0。
    float a_out = 0.0;
    float hit_s = uSentinel;

    bool have_hit = best < uSentinel * 0.5;
    // 背面命中（2026-11-13）：march 把 s 编成 s + kSsrBackfaceBias 写出来（见 kSsrMarchFs）。
    // T² 的 min 归约保证"有正面命中就一定不是背面命中"。
    bool is_backface = have_hit && best >= kSsrBackfaceBias;
    if (!SsrHasDepth(texelFetch(uW, mir, 0).r)) {
        type = 0u;                         // kNone：本像素不是反射候选
    } else if (SsrRoughnessAt(mir) > uRoughnessCutoff) {
        type = 0u;                         // kNone：太粗糙，本像素没有发射线
    } else {
        SsrRay ray;
        if (!SsrBuildRay(mir, ray)) {
            type = 3u;                     // kOffscreen：整条射线在屏幕外
        } else if (have_hit) {
            // s 是**命中格内的中点** ⇒ floor 回去一定是那个命中格（ssr_math.h::CellAtSigma）。
            float s = clamp(is_backface ? (best - kSsrBackfaceBias) : best, ray.enter, ray.exit);
            vec2 pos = ray.s0 + ray.dir * s;
            // 双线性取色（2026-11-02，替换原来的 texelFetch 最近邻）：
            // pos 是**连续**的屏幕坐标（像素单位，texel 中心 = +0.5），所以 uv = pos / uRes
            // 正好落在正确的双线性位置。最近邻会让倒影边缘逐像素跳一格 —— 这正是"反射
            // 锯齿"的主要内容（斜面/球面上尤其明显），改成线性后免费好一大截。
            rgb = texture(uColor, pos / uRes).rgb;
            hit_s = s;
            type = 1u;                     // kHit
            // A 的三个淡出（预算 / P 贴边 / Q 贴边）在这一行定稿：s 就是 P→Q 的像素距离。
            a_out = SsrHitConfidence(ray, s);

            // ── 掠射淡出（2026-11-13）──
            // kappa = cos(θ_p)/cos(θ_m)：主视图看 Q 有多掠射 vs 镜像视图看 Q 有多正。
            // kappa 大 ⇒ 需要的源信息在屏幕空间里根本不存在（缝/极掠射）⇒ A 淡出、交回 env。
            // **只动 A**：颜色/命中/距离都不变，所以下游（滤波/融合）逻辑不受影响。
            if (uGrazingFadeEnd > uGrazingFadeStart) {
                ivec2 hcell = clamp(ivec2(pos), ivec2(0), ivec2(uRes) - 1);
                float w_hit = SsrWAt(ray, s);
                vec3 nq = SsrNormalOf(texelFetch(uNormal, hcell, 0));
                if (w_hit > 0.0 && dot(nq, nq) > 0.5) {
                    float d_hit = 1.0 / w_hit;
                    vec2 ndc_h = pos / uRes * 2.0 - 1.0;
                    vec3 Qv = vec3(ndc_h.x * uAspect * uTanHalf, ndc_h.y * uTanHalf, -1.0) * d_hit;
                    // 反射体像素的视空间位置与法线（P 就是本像素）
                    float w_p = texelFetch(uW, mir, 0).r;
                    if (w_p > 0.0) {
                        vec3 np = SsrNormalOf(texelFetch(uNormal, mir, 0));
                        vec2 uv_p = (vec2(mir) + 0.5) / uRes;
                        vec2 ndc_p = uv_p * 2.0 - 1.0;
                        vec3 Pv = vec3(ndc_p.x * uAspect * uTanHalf, ndc_p.y * uTanHalf, -1.0) / w_p;
                        // 眼睛对反射平面的镜像位置（相机在原点）
                        vec3 eye_m = 2.0 * dot(np, Pv) * np;
                        vec3 Vp = normalize(Qv);
                        vec3 Vm = normalize(Qv - eye_m);
                        float cp = abs(dot(nq, Vp));
                        float cm = abs(dot(nq, Vm));
                        // kappa = 镜像视图的前缩 / 主视图的前缩。>1 表示"镜像里看得更大，
                        // 但屏幕里只存了一条缝" ⇒ 需要凭空放大。（cp→0 时发散，正是最坏情形。）
                        float kappa = cm / max(cp, 1.0e-3);
                        // 乘上去（不是赋值）：上面三个淡出已经写进 a_out 了。
                        a_out *= 1.0 - smoothstep(uGrazingFadeStart, uGrazingFadeEnd, kappa);
                    }
                }
            }
        } else {
            // 没命中：按**灭点**分类（旧版按 ray.exits 分，语义是反的）。
            vec2 vp;
            if (!SsrVanishingPoint(ray.d3, vp) ||
                vp.x < 0.0 || vp.y < 0.0 || vp.x >= uRes.x || vp.y >= uRes.y) {
                type = 3u;                 // kOffscreen
            } else {
                ivec2 vpi = clamp(ivec2(vp), ivec2(0), ivec2(uRes) - 1);
                // 灭点处**没有深度数据** = 反射方向真的指向天空；有几何 = 我们漏检/预算不够。
                type = SsrHasDepth(texelFetch(uW, vpi, 0).r) ? 4u : 2u;
            }
        }
    }
    FragRadiance = vec4(rgb, a_out);
    FragType = type;
    // flag 与 type 同源（只有 kHit 才可能有背面语义）：不做任何"先写后改"的隐式约定。
    FragBackface = (type == 1u && is_backface) ? 1u : 0u;
#ifdef SSR_DEBUG
    FragHitS = hit_s;
#endif
}
)glsl";

// ── 属性 pass（接入层用；不属于模块契约）──
// 画**所有** object3d（不只反射体）：SSR 的命中面朝向判据需要**遮挡物**的视空间法线，
// 而遮挡物不是反射体。只测不写深度 ⇒ 仍然只写"真正可见的那一层"。
// 输出（**两个附件**，MRT）：
//   location 0  rgb = **完整三维**视空间法线；a = 掩码（1 反射体 / 0.5 有法线非反射体 / 0 无数据）
//   location 1  rgb = **F0**（= mix(0.04, base_color, metallic)）；a = **roughness**
// 合成层只对 a > 0.75（= 反射体）的像素叠 SSR；模块自己**不读** .a。
//
// 为什么材质要进属性纹理（2026-11-02）：合成层原来用 `SsrConfig` 里的**一套** F0/roughness，
// 同屏只要有两种反射材质就必然算错（10-31 已实测：铬人腰部一圈蓝光，因为地板 F0=0.03 被
// 拿来重算了人的 env 项）。F0 是 per-material、roughness 是"要糊多少"的输入，两者都必须
// 逐像素。ao 仍是全局（本管线没有 SSAO，材质 ao 常量对场景无意义）。
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
out vec3 vNormalView;     // **完整三维**视空间法线（不再只给 .xy）
void main() {
    vec3 n_world = normalize(mat3(uModel) * aNormal);
    vNormalView = normalize(uWorldToView * n_world);
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)glsl";

// 蒙皮网格的属性 FS（2026-10-31）：与 kSsrAttrVs 同一套输出，但 VS 复用
// **蒙皮 VS 本身**（kSkinnedVs，它已经输出 vWorldNormal）——不再抄一份蒙皮代码。
// 所以这里把世界法线转到视空间（uWorldToView 由 Renderer 每趟上传一次）。
// 曲面（人体）的法线逐顶点不同，正是"曲面镜面"这条用例要测的东西。
inline constexpr const char* kSsrAttrSkinnedFs = R"glsl(
#version 330 core
in vec3 vWorldNormal;
uniform mat3  uWorldToView;
uniform float uAttrIsReflector;
uniform vec3  uAttrBaseColor;
uniform float uAttrMetallic;
uniform float uAttrRoughness;
out vec4 FragAttr;
out vec4 FragMat;
void main() {
    vec3 n_view = normalize(uWorldToView * normalize(vWorldNormal));
    // 双面渲染：背面法线翻转 —— 与主 pass（skeleton_renderer.h 的 gl_FrontFacing 段）同一约定。
    // 不翻的话，背面可见的薄片/双面网格在属性纹理里存的是**背离相机**的法线，
    // 命中面朝向判据 dot(R,N) 会给出与着色相反的结论（2026-11-01）。
    if (!gl_FrontFacing) n_view = -n_view;
    FragAttr = vec4(n_view, (uAttrIsReflector > 0.5) ? 1.0 : 0.5);   // 同 kSsrAttrFs
    FragMat = vec4(mix(vec3(0.04), uAttrBaseColor, uAttrMetallic), uAttrRoughness);
}
)glsl";

// 属性 pass 的**镂空变体**（2026-11-01 补）：alpha-cutout 材质（树叶/镂空贴片）在
// 主 pass 里是靠 `discard` 才没有写颜色/深度的；属性 pass 若不 discard，就会在**被丢弃的
// 透明区域**也写一遍法线（深度测试 LEQUAL 会放行更近的片元），把那一片的法线覆盖成
// "看不见的叶子" —— 命中面朝向判据于是就按一片不存在的几何做判断。
//
// VS 多传一个 UV（location 2，与主 pass 的顶点布局一致），FS 采样 baseColor.a 后 discard。
inline constexpr const char* kSsrAttrCutoutVs = R"glsl(
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aTexCoord;
uniform mat4 uMVP;
uniform mat4 uModel;
uniform mat3 uWorldToView;
out vec3 vNormalView;
out vec2 vTexCoord;
void main() {
    vec3 n_world = normalize(mat3(uModel) * aNormal);
    vNormalView = normalize(uWorldToView * n_world);
    vTexCoord = aTexCoord;
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)glsl";

inline constexpr const char* kSsrAttrCutoutFs = R"glsl(
#version 330 core
in vec3 vNormalView;
in vec2 vTexCoord;
uniform float uAttrIsReflector;   // 1 = 反射体（per-object uniform）
uniform vec3  uAttrBaseColor;     // 无纹理时 F0 的来源（有纹理时用采到的 rgb）
uniform float uAttrMetallic;
uniform float uAttrRoughness;
uniform sampler2D uBaseColorTex;  // 由 DrawObject3DSelected 的 cutout 分支上传/绑定
uniform int   uHasBaseColorTex;
uniform float uAlphaCutoff;
out vec4 FragAttr;
out vec4 FragMat;
void main() {
    // 有纹理就连 F0 一起用纹理色 —— 这是本 pass 唯一能拿到逐像素 baseColor 的地方
    //（非镂空变体没有 UV，只能用材质常量）。
    vec3 base = uAttrBaseColor;
    if (uHasBaseColorTex == 1) {
        vec4 texel = texture(uBaseColorTex, vTexCoord);
        if (texel.a < uAlphaCutoff) {
            discard;   // 与主 pass 的 JPOV_ALPHA_CUTOUT 分支同一判据
        }
        base = texel.rgb;
    }
    vec3 n_view = vNormalView;
    if (!gl_FrontFacing) n_view = -n_view;   // 同 kSsrAttrFs
    FragAttr = vec4(n_view, (uAttrIsReflector > 0.5) ? 1.0 : 0.5);
    FragMat = vec4(mix(vec3(0.04), base, uAttrMetallic), uAttrRoughness);
}
)glsl";

inline constexpr const char* kSsrAttrFs = R"glsl(
#version 330 core
in vec3 vNormalView;
uniform float uAttrIsReflector;   // 1 = 反射体（per-object uniform）
// 逐像素材质（per-object uniform）：主 pass 用的同一份 PBRMaterial 常量。
// base_color 若来自纹理，这里用的是材质常量（见 ssr_shader.h 顶部"已知偏差"）。
uniform vec3  uAttrBaseColor;
uniform float uAttrMetallic;
uniform float uAttrRoughness;
// rgb = **完整三维视空间法线**（不再只存 .xy 重建 z，见 SsrNormalOf 的注释）；
// a   = 掩码编码：1.0 = 反射体，0.5 = 有法线但不反射，0 = 无数据（未绘制/清屏）
out vec4 FragAttr;
out vec4 FragMat;    // rgb = F0；a = roughness
// F0 的唯一出处（与 env_specular.h 的 EnvSpecularTerm 同一式）：
vec3 SsrF0(vec3 base_color, float metallic) {
    return mix(vec3(0.04), base_color, metallic);
}
// ⚠️ 双面材质（`double_sided` 会关背面剔除）**必须**在这里翻转法线，与主 pass
//    object3d_renderer.h 的 `if (!gl_FrontFacing) N = -N;` 保持同一约定。
//    2026-11-01 修：此前属性 pass 不翻 ⇒ 背面可见的薄片（叶卡/布片/条带）写进
//    属性纹理的是背离相机的法线 ⇒ SsrHitFacing 的 dot(R,N) 与着色结论相反，
//    这些表面在倒影里会被整块判成"背面命中"而漏掉（或反过来吃掉别的命中）。
void main() {
    vec3 n_view = vNormalView;
    if (!gl_FrontFacing) n_view = -n_view;
    FragAttr = vec4(n_view, (uAttrIsReflector > 0.5) ? 1.0 : 0.5);
    FragMat = vec4(SsrF0(uAttrBaseColor, uAttrMetallic), uAttrRoughness);
}
)glsl";

}  // namespace jpov

#endif  // JPOV_SRC_SSR_SSR_SHADER_H_
