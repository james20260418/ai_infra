// JPOV SSR —— GL-free 的算法数学（单测的落点，也是着色器算式的**唯一文字形式**）
//
// 为什么单独一份（而不是全塞进 ssr_renderer.cc）：
//   ① 整套算法只有两条"公理"：`w = 1/d` 对屏幕弧长**线性**、体素柱的**区间重叠**判据。
//      它们错了画面不会崩，只会得到"倒影位置偏一点 / 薄栏杆不漏"这种极难归因的症状
//      ⇒ 必须在 CPU 上钉死（`test/ssr/ssr_math_test.cc`，无 GL、秒级、可负向验证）。
//   ② 有了 CPU 参考实现，GPU 测试就有了 oracle：断言"同输入下 CPU 与 GPU 逐条 s 相同"，
//      不需要维护 png gold，也不怕 llvmpipe 的精度漂移。
//   ③ `ssr_shader.h` 的 GLSL 必须与本文件的函数**逐字对应**（那边的注释指回函数名）。
//      只改一处 ⇒ GPU 测试的"CPU vs GPU"那条会红 —— 这正是我们要的耦合方式。
//
// 坐标系与单位（与 ssr_renderer.h 的契约一致）：
//   · 视空间：x 右、y 上、相机看向 −z；视距 d = −z_view > 0，单位米。
//   · 屏幕 σ：沿屏幕直线的**像素弧长**，起点 σ = 0；像素坐标 (0,0) 在左下（GL 习惯）。
//   · 厚度 T 单位米；w = 1/d 单位 1/米。

#ifndef JPOV_SRC_SSR_SSR_MATH_H_
#define JPOV_SRC_SSR_SSR_MATH_H_

#include <cmath>
#include <algorithm>

namespace jpov {
namespace ssr {

// 无命中的哨兵。与着色器里的 uSentinel 必须一致：R32F 里可精确表示，
// 且远大于任何合理 s（像素）。**不要用 inf** —— 某些驱动把它规整掉。
constexpr float kNoHit = 1.0e30f;

// `w_at(x,y)` 返回 ≤ 0 = 该格**没有几何**（天空/背景；降采样时天空写成 0）。
// 语义是"这里没有柱子"，march 直接穿过去 —— **不是**"far 处有一堵墙"。
// 反例（2026-10-30 实拍踩到）：把天空格当成 z=far、T=c·far 的柱子 ⇒ 掠射射线在 near-horizon
// 处扫到它 ⇒ 返回天空像素的颜色、A=1 ⇒ 画面上出现一大片假的高光带。
//
// 2026-11-02 起"空"统一用 **0** 表示（降采样把天空/远平面写成 0），所以这个哨兵只作为
// "调用方想显式表达空"的可选写法；判定一律是 `!(w > 0)`。
constexpr float kEmptyCell = 0.0f;

// ─────────────────────────── 极简向量（本文件不依赖 geom）───────────────────────────
// 只有这个 header 用到，且必须能在没有 glog/protobuf 的最小环境里编译（CPU 单测）。

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;
};

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

inline Vec2 operator+(const Vec2& a, const Vec2& b) { return {a.x + b.x, a.y + b.y}; }
inline Vec2 operator-(const Vec2& a, const Vec2& b) { return {a.x - b.x, a.y - b.y}; }
inline Vec2 operator*(const Vec2& a, float s) { return {a.x * s, a.y * s}; }
inline Vec3 operator+(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(const Vec3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float Dot(const Vec2& a, const Vec2& b) { return a.x * b.x + a.y * b.y; }
inline float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float Length(const Vec2& a) { return std::sqrt(Dot(a, a)); }
inline float Length(const Vec3& a) { return std::sqrt(Dot(a, a)); }
inline Vec2 Normalize(const Vec2& a) {
    const float len = Length(a);
    return len > 0.0f ? a * (1.0f / len) : Vec2{0.0f, 0.0f};
}
inline Vec3 Normalize(const Vec3& a) {
    const float len = Length(a);
    return len > 0.0f ? a * (1.0f / len) : Vec3{0.0f, 0.0f, 0.0f};
}

// ─────────────────────────── ① 反构射线 ───────────────────────────

// 窗口深度 [0,1]（= gl_FragCoord.z）→ 视距（米，正）。
//
// 恒等式：ndc_z = 2·depth − 1，d = 2·near·far / ((far + near) − ndc_z·(far − near))
// 与 particles/particle_renderer.h 的 ZView 是同一份公式（那边也带同一条 NaN 守卫）。
//
// 退化守卫：near ≈ 0 且 ndc_z → 1 时分母趋 0 ⇒ 0/0 = NaN，而 NaN 会顺着后处理污染
// **整帧**。用 `!(x > eps)` 而不是 `x < eps`：前者同时把 NaN 拦下来。
// 正常相机（near=0.1, far=1000）下 |denom| ≥ 2·near，此分支永不进入 ⇒ 结果逐位不变。
inline float DepthFromWindowDepth(float depth, float near_plane, float far_plane) {
    const float ndc_z = 2.0f * depth - 1.0f;
    const float denom = (far_plane + near_plane) - ndc_z * (far_plane - near_plane);
    if (!(std::fabs(denom) > 1.0e-6f)) {
        return std::max(near_plane, 1.0e-4f);
    }
    return (2.0f * near_plane * far_plane) / denom;
}

// 窗口深度 → w = 1/z_view（SSR 内部**唯一**使用的深度形式，2026-11-02 起）。
//
// 为什么 SSR 链路内部改用 w 而不是窗口深度：
//   判据全部是"1/d 落在哪个区间"，而 1/d 对屏幕坐标是**仿射**的 —— 用 w 表达时
//   每格的判据只剩两次比较（见 CellHit），不再每格做 3 次除法。窗口深度的
//   ndc→z 映射每格都要做一次除法，纯属白花。
//   降采样时把窗口深度换成 w 的代价是每 SSR 像素 1 次除法（而不是每格一次）。
//
// 约定：**w = 0 表示没有深度数据（天空/背景）**。远平面值 z=far 在降采样时也写成 0，
// 这样"空"只有一个表示，着色器与 CPU 参考不会分叉（旧版靠 `window_depth < 1` 判空）。
inline float WFromWindowDepth(float depth, float near_plane, float far_plane) {
    if (!(depth < 1.0f)) {
        return 0.0f;   // 清空值 ⇒ 天空 ⇒ 约定为 0
    }
    return 1.0f / DepthFromWindowDepth(depth, near_plane, far_plane);
}

// 该像素有没有深度数据。窗口深度 1.0 = 远平面清空值 ⇒ 天空/背景，**不发射线**。
//
// 为什么把这句收成函数：着色器与 CPU 参考必须用同一条规则。少了它，天空像素上两边就会
// 分叉 —— 射线从 far 朝远处走，而"柱子"也在 far（z=far, T=c·far），区间必然重叠 ⇒
// CPU 报命中、GPU 报 a=0。这种分叉只在有天空的场景里出现（也就是真实场景里必然出现）。
//
// SSR 链路内部用的是 w 形式（`WFromWindowDepth` 已把天空写成 0），这个函数留给
// 全分辨率的窗口深度（融合层/测试构造输入）。
inline bool HasDepthData(float window_depth) { return window_depth < 1.0f; }

// 像素中心 uv（∈(0,1)）→ 视空间点，给定该点的视距 d。
//   dir = vec3(n.x·aspect·tan_half, n.y·tan_half, −1)，n = 2·uv − 1
//   P = dir · d  ⇒ P.z = −d
inline Vec3 ViewPointAtUv(float u, float v, float d, float tan_half_fov_y, float aspect) {
    const float nx = 2.0f * u - 1.0f;
    const float ny = 2.0f * v - 1.0f;
    return Vec3{nx * aspect * tan_half_fov_y * d, ny * tan_half_fov_y * d, -d};
}

// 视空间反射：入射方向 = 相机→表面（= normalize(P)），N 朝相机。
inline Vec3 ReflectDir(const Vec3& p_view, const Vec3& normal) {
    const Vec3 incident = Normalize(p_view);
    return incident - normal * (2.0f * Dot(normal, incident));
}

// 起点卫生：沿法线抬高一个**相对量**（0.1% 的视距），把起点移出反射体表面。
// 与厚度 T 是两件事，别合成一个常数（§5.3）。着色器里同一行。
inline Vec3 OffsetRayOrigin(const Vec3& p, const Vec3& normal, float d_surf) {
    return p + normal * (d_surf * 1.0e-3f);
}

// 法线重建（只给 .xy 时）：z = sqrt(max(0, 1 − x² − y²))，再**归一化一次**。
// xy 长度 > 1（法线贴图/波浪扰动越界）时 z 取 0；不归一化的话 |N| > 1，
// reflect 会把 R 一起拉长 —— 这是真会踩的坑，所以归一化写在函数里而不是调用处。
inline Vec3 ReconstructNormal(float nx, float ny) {
    const float z2 = 1.0f - nx * nx - ny * ny;
    const Vec3 n{nx, ny, std::sqrt(std::max(0.0f, z2))};
    return Normalize(n);
}

// 射线长度裁剪（米）：朝相机走（R.z > 0）会穿过 d = 0 的极点（Möbius 另一支），
// 裁到**近平面之前**；再与 max_distance / far 取 min。
// 返回 ≤ 0 表示射线不可用（调用方写 kTooShort）。
inline float ClipRayLength(const Vec3& p, const Vec3& r, float near_plane, float far_plane,
                           float max_distance_m) {
    float len = std::min(max_distance_m, far_plane);
    if (r.z > 1.0e-9f) {
        const float to_near = (-near_plane - p.z) / r.z;
        len = std::min(len, to_near);
    }
    return len;
}

// ─────────────────────────── ② 屏幕参数化 + 裁剪 ───────────────────────────

// 一条射线在屏幕空间的参数化（σ = 未裁剪线段上的像素弧长，从 s0 起算）。
//
// 关键恒等式（§3.1）：w = 1/d 对 σ **线性** ⇒ 任何 σ 处 w 都能用 mix(w0, w1, σ/span)
// **精确**求得。绝不要用 mix(d0, d1, ·)：掠射时误差可达两个数量级。
struct ScreenRay {
    Vec2 s0;              // 起点像素坐标（未裁剪）
    Vec2 dir;             // 单位方向（像素空间）
    Vec3 dir3;            // **视空间**单位方向（朝向判据用；与 dir 是同一条光线的两种表达）
    Vec3 p0;              // **视空间**起点（= BuildScreenRay 的 p，已做卫生偏移）。只给面斜率补偿用
    float span = 0.0f;    // 未裁剪线段长度 S（像素）
    float w0 = 0.0f;      // σ = 0 处的 1/d
    float w1 = 0.0f;      // σ = span 处的 1/d
    float sigma_enter = 0.0f;  // 屏幕矩形裁剪后仍可见的 σ 区间
    float sigma_exit = 0.0f;
    // 本射线所在的 SSR 栅格尺寸（像素）。只给"命中点到屏幕边的距离"用（见 HitConfidence）。
    // 存在这里而不是当参数传：BuildScreenRay 本来就知道，且它与 s0/dir 是同一套坐标。
    Vec2 res{0.0f, 0.0f};
    // true = 射线在 sigma_exit 处撞到屏幕边界（或近平面裁剪）；false = 走完了整段。
    // 只用于调试原因位（kSky vs kOffscreen），不参与取色。
};

// 由视空间起点 P（**已做卫生偏移**）与单位方向 R 构造屏幕参数化。
//
// 返回 false ⇒ 整条射线在屏幕外 / 长度退化（调用方写 kOffscreen 或 kTooShort）。
// 失败时不保证 out 的内容有意义。
inline bool BuildScreenRay(const Vec3& p, const Vec3& r, float ray_len_m,
                           float tan_half_fov_y, float aspect, int width, int height,
                           ScreenRay* out) {
    const float d0 = -p.z;
    if (!(d0 > 1.0e-6f) || !(ray_len_m > 0.0f)) {
        return false;
    }
    const Vec3 q = p + r * ray_len_m;
    const float d1 = -q.z;
    if (!(d1 > 1.0e-6f)) {
        return false;   // 理论上被 ClipRayLength 挡掉；防御式再拦一次
    }
    const float tan_y = tan_half_fov_y;

    // 每点只做 1 次除法（原来两个分量各除一次；uAspect 与 tan_half 可以提出来）。
    // 与着色器 SsrBuildRay 同一形式。
    const float inv0 = 1.0f / (d0 * tan_y);
    const float inv1 = 1.0f / (d1 * tan_y);
    const Vec2 np{p.x / aspect * inv0, p.y * inv0};
    const Vec2 nq{q.x / aspect * inv1, q.y * inv1};
    const float w = static_cast<float>(width);
    const float h = static_cast<float>(height);

    out->s0 = Vec2{(np.x * 0.5f + 0.5f) * w, (np.y * 0.5f + 0.5f) * h};
    const Vec2 s1{(nq.x * 0.5f + 0.5f) * w, (nq.y * 0.5f + 0.5f) * h};
    out->w0 = 1.0f / d0;
    out->w1 = 1.0f / d1;
    out->p0 = p;
    out->res = Vec2{w, h};

    const Vec2 delta = s1 - out->s0;
    out->span = Length(delta);
    if (!(out->span > 1.0e-6f)) {
        return false;   // 长度退化：整条射线在屏幕上是一个点，没有可采样的格
    }
    out->dir = delta * (1.0f / out->span);
    // r 已经是单位向量（ReflectDir 的两个入参都归一过）⇒ 不再 Normalize。
    out->dir3 = r;   // 朝向判据用（着色器里同一行）

    // 矩形裁剪（Liang–Barsky），结果用 σ 表示（像素弧长）。
    float t0 = 0.0f;
    float t1 = out->span;
    const float lo[2] = {0.0f, 0.0f};
    const float hi[2] = {w, h};
    const float p0[2] = {out->s0.x, out->s0.y};
    const float dd[2] = {delta.x, delta.y};
    for (int axis = 0; axis < 2; ++axis) {
        if (std::fabs(dd[axis]) < 1.0e-9f) {
            if (p0[axis] < lo[axis] || p0[axis] > hi[axis]) {
                return false;   // 平行于该轴且在界外
            }
            continue;
        }
        float ta = (lo[axis] - p0[axis]) / dd[axis];
        float tb = (hi[axis] - p0[axis]) / dd[axis];
        if (ta > tb) {
            std::swap(ta, tb);
        }
        t0 = std::max(t0, ta * out->span);
        t1 = std::min(t1, tb * out->span);
    }
    if (!(t1 > t0)) {
        return false;
    }
    out->sigma_enter = t0;
    out->sigma_exit = t1;
    return true;
}

// σ 处的 1/d（线性插值，精确）。
inline float WAt(const ScreenRay& ray, float sigma) {
    const float t = ray.span > 0.0f ? sigma / ray.span : 0.0f;
    return ray.w0 + (ray.w1 - ray.w0) * t;
}

// σ 处的位置（像素坐标）。
inline Vec2 PosAt(const ScreenRay& ray, float sigma) {
    return ray.s0 + ray.dir * sigma;
}

// ─────────────────── ②.5 置信度 A（2026-11-06 的柔和过渡）───────────────────
//
// A = "这个像素的答案有多少值得用"，∈ [0,1]。以前它是二值的（命中 1 / 未命中 0），
// 于是"射线刚好在预算内命中"与"刚好差一格没命中"相邻时会硬切。现在乘上两个衰减：
//
//   d_budget = max_ray_pixels − σ                    命中点离**march 预算**还有多远
//   d_edge   = **反射体像素**到最近屏幕边的距离       （图像空间，不是沿射线）
//   A        = fade(d_budget, 预算带宽) · fade(d_edge, 边缘带宽)
//
// 为什么 d_edge 用图像空间而不是 "ray.sigma_exit − σ"（沿射线到出屏点的距离）：
//   后者看着更"正确"（它度量的是射线还能走多远），但**在掠射下会把整片倒影吃掉** ——
//   掠射时地面反射的是远处物体，而这些命中点本来就贴近射线的出屏点，
//   sigma_exit − σ 对所有"远处的倒影"都天然很小。实测（1280×720、近掠射、
//   带宽 163 屏幕像素、其它旋钮全关）：倒影覆盖区里 A 的保留率只有 60.6%，
//   全屏 10.8% 的像素被改动 —— 观感就是"镜面掠射直接糊成哑光"（Danis 2026-11-06 报）。
//   图像空间的判据只在**真正贴着画面边框**的那一圈衰减，符合"给边缘留个像素 buffer"的本意。
//   代价：一条平行于边框飞的射线，命中点贴着边框时也会被衰减 —— 但那只是沿边框的一条
//   窄带，不会误伤画面中部的反射。
//
// 为什么 d_budget **不**取 min(max_ray_pixels, ray.span)（2026-11-06 实测改回来）：
//   掠射时"投影后的射线全长"span 会被压到几十像素（射线在世界上飞十几米、屏幕上只走
//   30 像素），于是几乎每个命中点都贴着这个上限 ⇒ 一开带宽就整片变淡
//   （实测：带宽 50 屏幕像素时覆盖区 91% 被压）。span 短**不等于**预算用尽：
//   射线是被世界距离/近平面截断的，那里本来就该由边缘/兜底逻辑管，不该算作"快到预算极限"。
//   代价：被 max_distance_m 截断的射线不再有预算淡出 —— 目前 max_distance 远大于实际射线，
//   这个损失是零。
//
// 与 ssr_shader.h 的 SsrFade / SsrHitConfidence **逐字同一形式**。

// 剩余可用距离 → 权重。width ≤ 0 ⇒ 1（**关闭**；定义写清：不是"衰减 0 像素"）。
// 曲线 smoothstep：线性斜坡两端一阶导跳变，在平滑地板上能看见两条"折线"。
inline float FadeWeight(float remain_px, float width_px) {
    if (!(width_px > 0.0f)) {
        return 1.0f;
    }
    float t = remain_px / width_px;
    t = std::min(1.0f, std::max(0.0f, t));
    return t * t * (3.0f - 2.0f * t);
}

// 三个带宽（单位一律 **SSR 像素**；接入层负责把屏幕像素的配置按 1/N 折下来）。
// 0 = 关闭该衰减。
struct FadeParams {
    float budget_px = 0.0f;          // ① 命中点离 march 预算还有多远
    float reflector_edge_px = 0.0f;  // ② **反射体像素**离画面边框多远（观感缓冲带）
    float hit_edge_px = 0.0f;        // ③ **命中点**离画面边框多远（数据可信度）
};

// 到最近画面边的像素距离。
inline float DistToScreenEdge(const Vec2& p, const ScreenRay& ray) {
    return std::min(std::min(p.x, ray.res.x - p.x), std::min(p.y, ray.res.y - p.y));
}

// 命中点 σ 处的 A。三个带宽相乘。
//
// ② 与 ③ 是 Danis 2026-11-06 定稿的**两个都留**（各自一个 buffer 参数）：
//   ③ 的理由 = 不希望"屏幕内/外"的界线硬邦邦地体现在 rad 上；
//   ② 的理由 = 对柔和观感无害，而且只开 ③ 时 box 那类场景会显得奇怪（角上被硬切）。
//
// ② 与 ③ 是两个**不同的问题**，都很便宜、都直接用图像空间量，没有近似游戏：
//   ② 反射体（= 倒影在屏幕上的位置）贴边 ⇒ 镜子被画面边缘切断的观感，需要缓冲带；
//   ③ 命中点（= 反射源）贴边     ⇒ 那块内容本身就在画面外沿，可信度低，需要软化。
//   同一个画面里它们落的位置完全不同：② 贴着画面边框，③ 跟着"倒影映到的东西"走。
//   关掉任何一个都只是少一个旋钮，不会互相干扰（乘法）。
inline float HitConfidence(const ScreenRay& ray, float sigma, float max_ray_pixels,
                           const FadeParams& fade) {
    float a = FadeWeight(max_ray_pixels - sigma, fade.budget_px);
    if (fade.reflector_edge_px > 0.0f) {
        a *= FadeWeight(DistToScreenEdge(ray.s0, ray), fade.reflector_edge_px);
    }
    if (fade.hit_edge_px > 0.0f) {
        a *= FadeWeight(DistToScreenEdge(PosAt(ray, sigma), ray), fade.hit_edge_px);
    }
    return a;
}

// ─────────────────────────── ③ 体素柱判据 ───────────────────────────

// 每格三行判据（§5.1）：射线在本格内的**视深区间** [d_lo, d_hi] 与本格柱体
// [z, z+T] 是否有交集。w_in/w_out 是 1/d 端点，顺序无所谓；w_cell 是本格的 1/z（>0）。
//
// ⚠️ 2026-11-02 改成 **w 空间**：判据里不再出现任何除法（近平面退化分支除外）。
//    d_hi ≥ z      ⟺  w_min ≤ w_cell
//    d_lo ≤ z + T  ⟺  w_max ≥ 1/(z+T)；T = c·z ⇒ 1/(z+T) = w_cell/(1+c)
//                    ⇒ 两边乘 (1+c)：w_max·(1+c) ≥ w_cell
//    （c·z ≥ min_thickness 恒成立时；极近像素走下面的退化分支）
// 着色器 SsrCellHit 逐字同一形式 —— 改这里必须同步改那边（对拍会红）。
inline bool CellHit(float w_in, float w_out, float w_cell,
                    float thickness_coefficient, float min_thickness_m) {
    const float w_min = std::min(w_in, w_out);
    const float w_max = std::max(w_in, w_out);
    if (!(w_min > 0.0f) || !(w_cell > 0.0f)) {
        return false;   // 防御：w ≤ 0 表示 d ≤ 0 / 空格（理论上已被前面挡掉）
    }
    if (w_min > w_cell) {
        return false;   // 整段在柱子前方（水面最常见）
    }
    // 退化分支：厚度下限生效（z 极近，c·z < min_thickness）。正常相机永不进入。
    if (!(w_cell <= thickness_coefficient / min_thickness_m)) {
        const float z = 1.0f / w_cell;
        return w_max >= 1.0f / (z + min_thickness_m);
    }
    return w_max * (1.0f + thickness_coefficient) >= w_cell;
}

// ────────── ③.6 鲁棒版面斜率补偿：**前置扩 z 区间**（2026-11-08）──────────
//
// 上一版在 march 里做“逐格射线-平面求交”，引入锚点近似与 P 依赖，还出过
// “背面擦过被当命中 ⇒ backface 点亮 ⇒ env×0.2 ⇒ 矩形黑块”。这一版只做一件事：
// **在判交之前，按面斜率把该格的 [z_lo, z_hi] 放宽**，march 的区间判据一行不改。
// 因此：无锚点、无除法、无 P 依赖。
//
//   c_slope = 格角宽 · (|n_x|+|n_y|)/|n_z|
//   z_lo = max(z·(1−c_slope), min(z_3x3))                        // 向近补，但不比邻域更近
//   z_hi = max(z·(1+c), min(z·(1+c)·(1+c_slope), max(z_3x3)))    // 向远补，但不比邻域更远
//   z_hi = min(z_hi, far_z)                                      // 全局上界 = far_plane
//
// 两个 cap 都由**邻域数据**给出，不是手调常数：平面上相邻格 z 连续，所以
// “本格最多漂到邻域范围”就是几何允许的最大跨度；掠射 |n_z|→0 时 c_slope 会爆，
// 爆到超出邻域就被这条掐住 ⇒ 不需要 cap 参数。c（原厚度 N_px）完全不动。
struct CellZRange {
    float z_lo = 0.0f;
    float z_hi = 0.0f;
};

inline float CellNormalSlope(float nx, float ny, float nz) {
    return (std::fabs(nx) + std::fabs(ny)) / std::max(std::fabs(nz), 1.0e-3f);
}

inline CellZRange WidenCellZ(float z, float slope, float cell_angle, float thickness_coefficient,
                             float z_min_3x3, float z_max_3x3, float far_z) {
    CellZRange r;
    const float cs = cell_angle * slope;
    const float z_far0 = z * (1.0f + thickness_coefficient);
    r.z_lo = std::max(z * (1.0f - cs), z_min_3x3);
    r.z_lo = std::min(r.z_lo, z);
    r.z_hi = std::max(z_far0, std::min(z_far0 * (1.0f + cs), z_max_3x3));
    r.z_hi = std::min(r.z_hi, far_z);
    r.z_hi = std::max(r.z_hi, r.z_lo);
    return r;
}

// 扩区间后的每格判据：w_lo = 1/z_lo、w_hi = 1/z_hi。语义与 CellHit 完全相同（区间重叠）。
inline bool CellRangeHit(float w_in, float w_out, float w_lo, float w_hi) {
    const float w_min = std::min(w_in, w_out);
    const float w_max = std::max(w_in, w_out);
    if (!(w_min > 0.0f) || !(w_lo > 0.0f) || !(w_hi > 0.0f)) {
        return false;
    }
    return w_min <= w_lo && w_max >= w_hi;
}

// 命中面朝向判据（ssr_shader.h::SsrHitFacing 的 CPU 版）。
//
//   R·N < 0 ⇒ 射线**扎进**这个面 = 合法的正面命中；
//   R·N ≥ 0 ⇒ 射线是从这个面的背面/内部穿出来的 ⇒ 不算命中。
//
// 为什么必须有它：深度缓冲里只有"相机看得见的那一面"的颜色与法线。镜子往上看桌子
// 底面/柱体背面时，屏幕空间里没有那些面的像素，以前的判据会拿**正面**的颜色当结果
// ⇒ 倒影里桌子底下也在发光（2026-10-30 Danis 实拍发现）。
// Pre-condition：ray_dir 与 hit_normal 在同一空间（视空间），且 hit_normal 朝相机。
inline bool HitFacesRay(const Vec3& ray_dir, const Vec3& hit_normal) {
    return Dot(ray_dir, hit_normal) < 0.0f;
}

// ─────────── ⑥ 升采样的深度值域判据（composite；ssr_integration.cc 里同一份算式）───────────
//
// 场景：低分辨率 A/rgb 升到全分辨率时，要用「本像素的 1/z」与「低分格子的 1/z」比一次，
// 拒绝跨表面（剪影）的邻格。**固定百分比的直接比较在掠射下必然误判**：
// 地平线附近同一个平面相邻两行的深度比可以是 1.2～2 倍，远超 5% 的容差 ⇒ 2×2 里除了
// 「贡献了最小深度」的那一行，其余行整行被拒 ⇒ **周期 = 降采样倍率的横向条纹**
// （实测 1:2 → period 2 / 1:4 → period 4 / 1:1 无条纹；2026-10-31 Danis 实拍）。
//
// 修法的依据只有一条：**1/z_view 在任何平面上对屏幕坐标是仿射的**（射影几何：平面
// n·X = c ⇒ 1/z 是 (x_ndc, y_ndc) 的线性函数）。于是「低分格子应有的 w」可以用
// 「本像素 w + 屏幕梯度·偏移」**预测**出来，平面上残差恒 ≈ 0：
//
//   低分深度取的是**格内最近**（min 窗口深度 = max w）⇒ 预测值要取该格范围内的最大 w
//   （仿射函数在方形格上的极值在角上）：pred = w_hi + g·dp + ½·(scale−1)·(|gx|+|gy|)
//
//   ⚠️ 扩展量是 (scale−1) 而不是 scale：降采样取的是**各 texel 中心**的最小值，
//      1:1 时格子就是一个 texel ⇒ 扩展量必须恰好为 0（否则 1:1 被推歪，变成回归）。
//
// 判据：|w_lo − pred| ≤ tol·pred。平面 ⇒ 过；剪影（block 里是另一个表面）⇒ 拒。
//
// 单位：w = 1/米；gw_* = 每像素的 Δw；dpx/dpy = 本像素中心到低分 texel 中心的**全分辨率像素**偏移。
inline float UpsamplePredictedW(float w_hi, float gw_x, float gw_y, float dpx, float dpy,
                                int scale) {
    const float expand =
        0.5f * static_cast<float>(scale - 1) * (std::fabs(gw_x) + std::fabs(gw_y));
    return w_hi + gw_x * dpx + gw_y * dpy + expand;
}

inline bool UpsampleDepthAccept(float w_hi, float gw_x, float gw_y, float dpx, float dpy,
                                float w_lo, int scale, float tol) {
    const float pred = UpsamplePredictedW(w_hi, gw_x, gw_y, dpx, dpy, scale);
    return std::fabs(w_lo - pred) <= tol * std::max(std::fabs(pred), 1.0e-6f);
}

// 默认朝向判据：恒真。语义 = "这一格没有法线数据，不判方向" —— 保持引入朝向判据
// 之前的行为（老调用点/老测试因此逐字不变）。
struct AcceptAllFacing {
    bool operator()(int /*cell_x*/, int /*cell_y*/, const Vec3& /*ray_dir*/) const {
        return true;
    }
};

// 默认"没有放宽区间"：返回 false ⇒ 只用旧的区间判据（开关关闭时的行为）。
struct NoRange {
    bool operator()(int /*cell_x*/, int /*cell_y*/, float* /*w_lo*/, float* /*w_hi*/) const {
        return false;
    }
};

// ─────────────────────────── ④ 派生参数 ───────────────────────────

// tile 与预算 → 每段长度 / 每段步数 / 厚度系数。**唯一一份算式**（Renderer 与测试共用）。
//
//   c        = N_px · θ_px，θ_px = 垂直 FOV(弧度) / 屏幕高
//   lanes    = T²
//   L        = max_ray_pixels / lanes
//   steps    = ceil(1.4142·L) + 3
//              （对角线最坏情况：跨格数 ≤ 3 + L·(|dx|+|dy|) ≤ 3 + 1.4142·L。
//                +3 不是保险丝：DDA 在 σ ≥ 段尾时就 break，**超额的上限一次都不会执行**，
//                所以宁可给足 —— 少了就是"段尾那一格永远扫不到"，只在特定角度缺一格。）
struct DerivedParams {
    int lanes = 1;
    float slab_pixels = 0.0f;
    int steps_per_lane = 1;
    float thickness_coefficient = 0.0f;
};

// 两个衰减带宽（与 DerivedParams 同路的**派生量**，不进单测以外的契约）：
// 这里不加函数 —— 折算是接入层的一个除法（屏幕像素 / N），写在这里会多一层无法验证的间接。

// Pre-condition: tile_size ∈ {1,2,4,8}、max_ray_pixels > 0、height > 0
//（校验放在 SsrRenderer::Render —— 这里不重复 CHECK，免得 GL-free 头文件依赖 glog）。
inline DerivedParams DeriveParams(int tile_size, int max_ray_pixels, float thickness_pixels,
                                  float tan_half_fov_y, int height) {
    DerivedParams d;
    d.lanes = tile_size * tile_size;
    d.slab_pixels = static_cast<float>(max_ray_pixels) / static_cast<float>(d.lanes);
    d.steps_per_lane = static_cast<int>(std::ceil(1.41421356f * d.slab_pixels)) + 3;
    const float fov_y = 2.0f * std::atan(tan_half_fov_y);
    const float theta_px = fov_y / static_cast<float>(std::max(1, height));
    d.thickness_coefficient = thickness_pixels * theta_px;
    return d;
}

// 段长不足 4 px 时判据次数会按 ≈ 1 + 1/L 放大（相邻 lane 重复判同一格）。
// **只用于文档/单测**：渲染器不据此告警（画面完全相同，不值得占一行日志）。
inline bool SlabTooShortForEfficiency(const DerivedParams& d) {
    return d.slab_pixels < 4.0f;
}

// ─────────────────────────── ⑤ CPU 参考实现 ───────────────────────────

// 在段 [sigma_start, sigma_end] 上走 DDA，返回**最早命中格内**的中点 s_mid（无命中 = kNoHit）。
//
// 命中格内的中点（而不是出格点）是刻意的：从 s 反推 uv 时必须保证落在那一个格子里，
// 而柱内颜色处处相同 ⇒ s 的截断误差不影响取色（ssr_renderer.h 文件头的并行化一节）。
//
// skip_first_cell：起点就落在反射体表面上时（σ_start == 0）跳过起点格，否则射线会在
// 自己的柱子里立刻命中自己。**只对 σ_start == 0 的那一段成立**，其余 lane 的起点不在
// 反射体上，不该跳。
//
// w_at(x, y) → 该格的 **w = 1/z_view**（0/负 = 没有几何）。越界由调用方保证不会发生
// （这里仍防御性 break）。
//
// facing(cell_x, cell_y, ray_dir) → 该格能不能算命中（默认恒真 = 不判朝向）。
//   **命中但朝向不对（背面命中）不算命中，继续 march** —— 射线应该穿过那个物体，
//   而不是把它的正面颜色当结果（ssr_math.h::HitFacesRay 的来龙去脉）。
//   `out_saw_backface`（可选出参）：整段里**有过**"进了柱体但朝向不对"的格 ⇒ true。
//   融合层拿它把"没有答案"的像素的环境镜面项压暗（背面命中 = 屏幕里没有那个面的数据，
//   纯 env 会透出一块过亮的探针色）。它与命中不互斥：后面若有合法命中，命中优先。
template <typename DepthFn, typename FacingFn = AcceptAllFacing, typename RangeFn = NoRange>
float MarchSegment(const ScreenRay& ray, float sigma_start, float sigma_end, int max_cells,
                   bool skip_first_cell, float thickness_coefficient, float min_thickness_m,
                   DepthFn&& w_at, int width, int height, int* hit_x = nullptr,
                   int* hit_y = nullptr, FacingFn facing = FacingFn{},
                   bool* out_saw_backface = nullptr, RangeFn range = RangeFn{}) {
    const Vec2 pos = PosAt(ray, sigma_start);
    int cx = static_cast<int>(std::floor(pos.x));
    int cy = static_cast<int>(std::floor(pos.y));
    float sigma = sigma_start;

    // 下一个 x/y 网格线的 σ 与跨一格的 σ 步长。
    float tmax_x = 1.0e30f;
    float tdelta_x = 1.0e30f;
    float tmax_y = 1.0e30f;
    float tdelta_y = 1.0e30f;
    if (ray.dir.x > 1.0e-9f) {
        tdelta_x = 1.0f / ray.dir.x;
        tmax_x = sigma + (static_cast<float>(cx + 1) - pos.x) / ray.dir.x;
    } else if (ray.dir.x < -1.0e-9f) {
        tdelta_x = -1.0f / ray.dir.x;
        tmax_x = sigma + (static_cast<float>(cx) - pos.x) / ray.dir.x;
    }
    if (ray.dir.y > 1.0e-9f) {
        tdelta_y = 1.0f / ray.dir.y;
        tmax_y = sigma + (static_cast<float>(cy + 1) - pos.y) / ray.dir.y;
    } else if (ray.dir.y < -1.0e-9f) {
        tdelta_y = -1.0f / ray.dir.y;
        tmax_y = sigma + (static_cast<float>(cy) - pos.y) / ray.dir.y;
    }

    // 起点格跳过：只前进一格，不判定。
    if (skip_first_cell) {
        if (tmax_x < tmax_y) {
            sigma = tmax_x;
            cx += ray.dir.x > 0.0f ? 1 : -1;
            tmax_x += tdelta_x;
        } else {
            sigma = tmax_y;
            cy += ray.dir.y > 0.0f ? 1 : -1;
            tmax_y += tdelta_y;
        }
    }

    for (int step = 0; step < max_cells; ++step) {
        if (cx < 0 || cx >= width || cy < 0 || cy >= height) {
            break;
        }
        const float sigma_out = std::min(std::min(tmax_x, tmax_y), sigma_end);
        if (!(sigma_out > sigma)) {
            break;   // 退化（浮点抖动）—— 别在这里死循环
        }
        const float w_cell = w_at(cx, cy);
        if (!(w_cell > 0.0f)) {
            // 空格子（天空/无深度数据）：这里没有柱子，穿过去（见 kEmptyCell 的注释）。
            // ⚠️ 别在这里 return —— 屏内天空后面还可能有几何。
            if (sigma_out >= sigma_end) {
                break;
            }
            sigma = sigma_out;
            if (tmax_x < tmax_y) {
                cx += ray.dir.x > 0.0f ? 1 : -1;
                tmax_x += tdelta_x;
            } else {
                cy += ray.dir.y > 0.0f ? 1 : -1;
                tmax_y += tdelta_y;
            }
            continue;
        }
        bool cell_hit = CellHit(WAt(ray, sigma), WAt(ray, sigma_out), w_cell,
                                thickness_coefficient, min_thickness_m);
        if (!cell_hit) {
            float w_lo = 0.0f;
            float w_hi = 0.0f;
            if (range(cx, cy, &w_lo, &w_hi)) {
                cell_hit = CellRangeHit(WAt(ray, sigma), WAt(ray, sigma_out), w_lo, w_hi);
            }
        }
        if (cell_hit) {
            if (facing(cx, cy, ray.dir3)) {
                if (hit_x != nullptr) {
                    *hit_x = cx;
                }
                if (hit_y != nullptr) {
                    *hit_y = cy;
                }
                return 0.5f * (sigma + sigma_out);
            }
            // 背面命中：不算结果，但**记下**来（融合层用它压暗 env fallback）。
            if (out_saw_backface != nullptr) {
                *out_saw_backface = true;
            }
        }
        if (sigma_out >= sigma_end) {
            break;
        }
        sigma = sigma_out;
        if (tmax_x < tmax_y) {
            cx += ray.dir.x > 0.0f ? 1 : -1;
            tmax_x += tdelta_x;
        } else {
            cy += ray.dir.y > 0.0f ? 1 : -1;
            tmax_y += tdelta_y;
        }
    }
    return kNoHit;
}

// 单线程版：整条射线一条 DDA 走到底（"K=1"的语义，也是等价性测试的对照）。
template <typename DepthFn, typename FacingFn = AcceptAllFacing, typename RangeFn = NoRange>
float MarchWholeRay(const ScreenRay& ray, float thickness_coefficient, float min_thickness_m,
                    int max_cells, DepthFn&& w_at, int width, int height,
                    int* hit_x = nullptr, int* hit_y = nullptr,
                    FacingFn facing = FacingFn{}, bool* out_saw_backface = nullptr,
                    RangeFn range = RangeFn{}) {
    const bool skip = ray.sigma_enter <= 0.0f;
    return MarchSegment(ray, ray.sigma_enter, ray.sigma_exit, max_cells, skip,
                        thickness_coefficient, min_thickness_m, w_at, width, height,
                        hit_x, hit_y, facing, out_saw_backface, range);
}

// 分段并行版（CPU 模拟 GPU 的 T² 个 lane）：每段等长 L，各自报本段最早命中，取 min。
// **必须与 MarchWholeRay 给出同一个 s** —— 这是 GPU 侧"分段 + min"能成立的全部依据。
template <typename DepthFn, typename FacingFn = AcceptAllFacing, typename RangeFn = NoRange>
float MarchSlabs(const ScreenRay& ray, const DerivedParams& d, float min_thickness_m,
                 DepthFn&& w_at, int width, int height, int* hit_x = nullptr,
                 int* hit_y = nullptr, FacingFn facing = FacingFn{},
                 bool* out_saw_backface = nullptr, RangeFn range = RangeFn{}) {
    float best = kNoHit;
    int best_x = -1;
    int best_y = -1;
    bool saw_backface = false;
    for (int lane = 0; lane < d.lanes; ++lane) {
        const float lane_begin = d.slab_pixels * static_cast<float>(lane);
        if (lane_begin >= ray.sigma_exit) {
            break;   // 短射线：后面的段整段空转
        }
        const float start = std::max(lane_begin, ray.sigma_enter);
        const float end = std::min(lane_begin + d.slab_pixels, ray.sigma_exit);
        if (!(end > start)) {
            continue;
        }
        int cx = -1;
        int cy = -1;
        const float s = MarchSegment(ray, start, end, d.steps_per_lane, start <= 0.0f,
                                     d.thickness_coefficient, min_thickness_m, w_at,
                                     width, height, &cx, &cy, facing, &saw_backface, range);
        if (s < best) {
            best = s;
            best_x = cx;
            best_y = cy;
        }
    }
    if (hit_x != nullptr) {
        *hit_x = best_x;
    }
    if (hit_y != nullptr) {
        *hit_y = best_y;
    }
    if (out_saw_backface != nullptr) {
        *out_saw_backface = saw_backface;
    }
    return best;
}

// s → 命中格（resolve pass 的唯一一步"反推"）。
//   pos = s0 + dir·s ⇒ 格 = floor(pos)
// s_mid 是格内中点 ⇒ 结果**必然**是那个命中格（单测 ssr_math_test 的 S_MidLandsInHitCell）。
inline void CellAtSigma(const ScreenRay& ray, float sigma, int* cell_x, int* cell_y) {
    const Vec2 pos = PosAt(ray, sigma);
    if (cell_x != nullptr) {
        *cell_x = static_cast<int>(std::floor(pos.x));
    }
    if (cell_y != nullptr) {
        *cell_y = static_cast<int>(std::floor(pos.y));
    }
}

}  // namespace ssr
}  // namespace jpov

#endif  // JPOV_SRC_SSR_SSR_MATH_H_
