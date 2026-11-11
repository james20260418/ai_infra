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
//   · w = 1/d 单位 1/米。2026-11-11 起判据只用前表面深度，没有厚度参数。

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
//   每格的判据只剩两次比较（见 CellCrossesFront），不再每格做除法。窗口深度的
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
// 与任何厚度参数无关（§5.3）。着色器里同一行。
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
    Vec3 dir3;            // **视空间**单位方向（debug/灭点用；与 dir 是同一条光线的两种表达）
    float span = 0.0f;    // 未裁剪线段长度 S（像素）
    float w0 = 0.0f;      // σ = 0 处的 1/d
    float w1 = 0.0f;      // σ = span 处的 1/d
    float sigma_enter = 0.0f;  // 屏幕矩形裁剪后仍可见的 σ 区间
    float sigma_exit = 0.0f;
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

    const Vec2 delta = s1 - out->s0;
    out->span = Length(delta);
    if (!(out->span > 1.0e-6f)) {
        return false;   // 长度退化：整条射线在屏幕上是一个点，没有可采样的格
    }
    out->dir = delta * (1.0f / out->span);
    // r 已经是单位向量（ReflectDir 的两个入参都归一过）⇒ 不再 Normalize。
    out->dir3 = r;   // debug/灭点用（着色器里同一行）

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

// ─────────────────────────── ③ 体素柱判据 ───────────────────────────

// 每格唯一判据（2026-11-11 起）：**射线在这一格里穿过了前表面**。
//
//   z_in ≤ z_front ≤ z_out     ⟺     w_min ≤ w_cell ≤ w_max        （w = 1/z，反序）
//
// 为什么不再有厚度：`z_front` 就是那张面，射线穿面即命中。旧版把格子当成一根
// [z, z+T] 的柱子去和射线的深度区间求交，于是不得不凭空造一个 T（经验厚度 c·z、
// min_thickness、以及"分辨率下限" dz_cell）—— 三个魔数都是"没有背面数据"时代的产物。
// 有了 z_front（和 z_back）之后：
//   · 薄片（叶卡/单面 quad）：就是零厚度面，穿面即命中 ⇒ 不需要假厚度；
//   · 实体：射线若从内部穿过，则进格时已经 `z_in > z_front` ⇒ 自动不满足上式，
//     不会被误判成命中（旧的 fac9ing / 背面判据在这里是多余的）。
// 掠射时"一格跨几十米"也不再是问题：判的是穿没穿过这张面，不是比厚度大小，
// 因此不存在"逐行抛硬币"的梳齿。
//
// 着色器 SsrCellCrossesFront 逐字同一形式 —— 改这里必须同步改那边（对拍会红）。
inline bool CellCrossesFront(float w_in, float w_out, float w_cell) {
    if (!(w_cell > 0.0f)) {
        return false;   // 空格（天空/背景）：这里没有面
    }
    const float w_lo = std::min(w_in, w_out);
    const float w_hi = std::max(w_in, w_out);
    return (w_lo <= w_cell) && (w_cell <= w_hi);
}

// 命中面朝向判据（ssr_shader.h::SsrHitFacing 的 CPU 版）。
//
//   R·N < 0 ⇒ 射线**扎进**这个面 = 合法的正面命中；
//   R·N ≥ 0 ⇒ 射线是从这个面的背面/内部穿出来的 ⇒ 不算命中。
//
// 为什么 z_front 判据替代不了它（2026-11-11 复核）：深度图只存"相机看得见的那一面"。
// 镜子往上看桌子底面时，射线的**屏幕位置**上存的是桌面顶面（相机看得见的那面），
// 而射线实际打的是底面 —— 射线确实"穿过"了顶面那格的深度（区间包含成立），
// 但那是屏幕空间的巧合，不是真的打到了顶面。唯一能分开这两者的信息是**法线朝向**：
// 射线向上、顶面法线朝上 ⇒ R·N > 0 ⇒ 拒。所以这条判据不是旧时代的残留，而是
// 屏幕空间反射的固有限制（看不见的面没有深度，只能靠朝向近似）。
// Pre-condition：ray_dir 与 hit_normal 在同一空间（视空间），且 hit_normal 朝相机。
inline bool HitFacesRay(const Vec3& ray_dir, const Vec3& hit_normal) {
    return Dot(ray_dir, hit_normal) < 0.0f;
}

// 默认朝向判据：恒真。语义 = "这一格没有法线数据，不判方向"。
struct AcceptAllFacing {
    bool operator()(int /*cell_x*/, int /*cell_y*/, const Vec3& /*ray_dir*/) const {
        return true;
    }
};

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

// ─────────────────────────── ④ 派生参数 ───────────────────────────

// tile → lane 数与每段步数上限。**唯一一份算式**（Renderer 与测试共用）。
//
// 2026-11-11：lane 不再是"每段固定像素长度 L"，而是把射线的 [enter, exit] 等分成
// lanes 段。于是：
//   · 没有 uSlabPixels / uMaxRayPixels 这两个屏幕空间预算 —— 射线的长度已经由
//     世界空间的 max_distance_m / 远平面 / 近平面裁剪决定；
//   · 步数上限按**最坏可能的屏幕跨度**（屏幕对角线）算，DDA 到段尾就 break，
//     所以超额的上限一次都不会执行（见 MarchSegment 的 `sigma_out >= sigma_end`）。
//   · +3 不是保险丝：对角线最坏情况跨 ceil(√2·L)+1 格；少给就是"段尾那一格永远
//     扫不到"，只在特定角度缺一格（最难查的一类）。
struct DerivedParams {
    int lanes = 1;
    int steps_per_lane = 1;
};

// Pre-condition: tile_size ∈ {1,2,4,8}、width/height > 0
//（校验放在 SsrRenderer::Render —— 这里不重复 CHECK，免得 GL-free 头文件依赖 glog）。
inline DerivedParams DeriveParams(int tile_size, int width, int height) {
    DerivedParams d;
    d.lanes = tile_size * tile_size;
    const float diag = std::sqrt(static_cast<float>(width) * static_cast<float>(width) +
                                 static_cast<float>(height) * static_cast<float>(height));
    d.steps_per_lane =
        static_cast<int>(std::ceil(1.41421356f * diag / static_cast<float>(d.lanes))) + 3;
    return d;
}

// ─────────────────────────── ⑤ CPU 参考实现 ───────────────────────────

// 在段 [sigma_start, sigma_end] 上走 DDA，返回**最早命中格内**的中点 s_mid（无命中 = kNoHit）。
//
// 命中格内的中点（而不是出格点）是刻意的：从 s 反推 uv 时必须保证落在那一个格子里，
// 而一格之内的差异只来自网格对齐 ⇒ s 的截断误差不影响取色。
//
// skip_first_cell：起点就落在反射体表面上时（σ_start == 0）跳过起点格，否则射线会在
// 自己的身体上立刻命中自己。**只对 σ_start == 0 的那一段成立**，其余 lane 的起点不在
// 反射体上，不该跳。
//
// w_at(x, y) → 该格的 **w = 1/z_view**（0/负 = 没有几何）。越界由调用方保证不会发生
// （这里仍防御性 break）。
//
// 2026-11-11：判据退化成 CellCrossesFront（穿前表面），因此不再有厚度/朝向参数。
template <typename DepthFn, typename FacingFn = AcceptAllFacing>
float MarchSegment(const ScreenRay& ray, float sigma_start, float sigma_end, int max_cells,
                   bool skip_first_cell, DepthFn&& w_at, int width, int height,
                   int* hit_x = nullptr, int* hit_y = nullptr,
                   FacingFn facing = FacingFn{}) {
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
        if (CellCrossesFront(WAt(ray, sigma), WAt(ray, sigma_out), w_cell) &&
            facing(cx, cy, ray.dir3)) {
            if (hit_x != nullptr) {
                *hit_x = cx;
            }
            if (hit_y != nullptr) {
                *hit_y = cy;
            }
            return 0.5f * (sigma + sigma_out);
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
template <typename DepthFn, typename FacingFn = AcceptAllFacing>
float MarchWholeRay(const ScreenRay& ray, int max_cells, DepthFn&& w_at, int width, int height,
                    int* hit_x = nullptr, int* hit_y = nullptr,
                    FacingFn facing = FacingFn{}) {
    const bool skip = ray.sigma_enter <= 0.0f;
    return MarchSegment(ray, ray.sigma_enter, ray.sigma_exit, max_cells, skip, w_at, width,
                        height, hit_x, hit_y, facing);
}

// 分段并行版（CPU 模拟 GPU 的 T² 个 lane）：把 [enter, exit] **等分**成 lanes 段，
// 各自报本段最早命中，取 min。**必须与 MarchWholeRay 给出同一个 s** —— 这是 GPU 侧
// "分段 + min" 能成立的全部依据。等分（而不是固定像素长度）的理由：射线的长度已经由
// 世界空间裁剪决定，屏幕预算这个概念被删掉了（2026-11-11）。
template <typename DepthFn, typename FacingFn = AcceptAllFacing>
float MarchSlabs(const ScreenRay& ray, const DerivedParams& d, DepthFn&& w_at, int width,
                 int height, int* hit_x = nullptr, int* hit_y = nullptr,
                 FacingFn facing = FacingFn{}) {
    float best = kNoHit;
    int best_x = -1;
    int best_y = -1;
    const float t0 = ray.sigma_enter;
    const float t1 = ray.sigma_exit;
    if (!(t1 > t0)) {
        if (hit_x != nullptr) *hit_x = -1;
        if (hit_y != nullptr) *hit_y = -1;
        return kNoHit;
    }
    const float span = t1 - t0;
    // 段数上限 = 射线在屏幕上的像素弧长（向上取整）：避免出现"亚像素段"——
    // 亚像素段会把一格的判定区间切碎（同一个格被两个段各看一半），
    // 结果随 tile 变化（tile 本应只是性能旋钮）。见 ssr_gpu_test 的 TileIsOnlyPerformance。
    int lanes_used = d.lanes;
    const int span_px = static_cast<int>(std::ceil(span));
    if (span_px > 0 && span_px < lanes_used) {
        lanes_used = span_px;
    }
    const float lanes = static_cast<float>(lanes_used);
    for (int lane = 0; lane < lanes_used; ++lane) {
        const float start = t0 + span * (static_cast<float>(lane) / lanes);
        const float end = t0 + span * (static_cast<float>(lane + 1) / lanes);
        if (!(end > start)) {
            continue;
        }
        int cx = -1;
        int cy = -1;
        const float s = MarchSegment(ray, start, end, d.steps_per_lane, start <= 0.0f, w_at,
                                     width, height, &cx, &cy, facing);
        if (s < best) {
            best = s;
            best_x = cx;
            best_y = cy;
        }
    }
    if (hit_x != nullptr) *hit_x = best_x;
    if (hit_y != nullptr) *hit_y = best_y;
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
