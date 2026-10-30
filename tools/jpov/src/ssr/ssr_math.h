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

// `depth_at(x,y)` 返回这个值 = 该格**没有几何**（窗口深度是清空值 1.0，天空/背景）。
// 语义是"这里没有柱子"，march 直接穿过去 —— **不是**"far 处有一堵墙"。
// 反例（2026-10-30 实拍踩到）：把天空格当成 z=far、T=c·far 的柱子 ⇒ 掠射射线在 near-horizon
// 处扫到它 ⇒ 返回天空像素的颜色、A=1 ⇒ 画面上出现一大片假的高光带。
constexpr float kEmptyCell = -1.0f;

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

// 该像素有没有深度数据。窗口深度 1.0 = 远平面清空值 ⇒ 天空/背景，**不发射线**。
//
// 为什么把这句收成函数：着色器与 CPU 参考必须用同一条规则。少了它，天空像素上两边就会
// 分叉 —— 射线从 far 朝远处走，而"柱子"也在 far（z=far, T=c·far），区间必然重叠 ⇒
// CPU 报命中、GPU 报 a=0。这种分叉只在有天空的场景里出现（也就是真实场景里必然出现）。
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
    float span = 0.0f;    // 未裁剪线段长度 S（像素）
    float w0 = 0.0f;      // σ = 0 处的 1/d
    float w1 = 0.0f;      // σ = span 处的 1/d
    float sigma_enter = 0.0f;  // 屏幕矩形裁剪后仍可见的 σ 区间
    float sigma_exit = 0.0f;
    // true = 射线在 sigma_exit 处撞到屏幕边界（或近平面裁剪）；false = 走完了整段。
    // 只用于调试原因位（kSky vs kOffscreen），不参与取色。
    bool exits_screen = false;
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
    const float tan_x = aspect * tan_half_fov_y;
    const float tan_y = tan_half_fov_y;

    const Vec2 np{p.x / (d0 * tan_x), p.y / (d0 * tan_y)};
    const Vec2 nq{q.x / (d1 * tan_x), q.y / (d1 * tan_y)};
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
    out->dir3 = Normalize(r);   // 朝向判据用（着色器里同一行）

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
    out->exits_screen = t1 < out->span - 1.0e-4f;
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

// 厚度：T = max(c · z_cell, min_thickness)。§9.2：z 必须是**本格**深度，
// 不是反射体的、也不是格内平均值（轮廓格内 d_surf 可以差 50 倍）。
inline float CellThickness(float z_cell, float thickness_coefficient, float min_thickness_m) {
    return std::max(thickness_coefficient * z_cell, min_thickness_m);
}

// 每格三行判据（§5.1）：射线在本格内的**视深区间** [d_lo, d_hi] 与本格柱体
// [z, z+T] 是否有交集。w_in/w_out 是 1/d 端点，顺序无所谓。
//
// 注意这不是"是否留在厚度带内"（双线性深度面的旧判据，依赖每格穿透量）——
// 那是放弃双线性的真正原因（§4.2 的墙/栏杆对照）。
inline bool CellHit(float w_in, float w_out, float z_cell, float thickness) {
    const float w_min = std::min(w_in, w_out);
    const float w_max = std::max(w_in, w_out);
    if (!(w_min > 0.0f)) {
        return false;   // 防御：w ≤ 0 表示 d ≤ 0（理论上已被裁剪挡住）
    }
    const float d_lo = 1.0f / w_max;
    const float d_hi = 1.0f / w_min;
    if (d_hi < z_cell) {
        return false;   // 整段在柱子前方（水面最常见）
    }
    if (d_lo > z_cell + thickness) {
        return false;   // 整段在柱子后方（穿过去了）
    }
    return true;
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

// 默认朝向判据：恒真。语义 = "这一格没有法线数据，不判方向" —— 保持引入朝向判据
// 之前的行为（老调用点/老测试因此逐字不变）。
struct AcceptAllFacing {
    bool operator()(int /*cell_x*/, int /*cell_y*/, const Vec3& /*ray_dir*/) const {
        return true;
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
// depth_at(x, y) → 该格的视距（米）。越界时由调用方保证不会发生（这里仍防御性 break）。
//
// facing(cell_x, cell_y, ray_dir) → 该格能不能算命中（默认恒真 = 不判朝向）。
//   **命中但朝向不对（背面命中）不算命中，继续 march** —— 射线应该穿过那个物体，
//   而不是把它的正面颜色当结果（ssr_math.h::HitFacesRay 的来龙去脉）。
template <typename DepthFn, typename FacingFn = AcceptAllFacing>
float MarchSegment(const ScreenRay& ray, float sigma_start, float sigma_end, int max_cells,
                   bool skip_first_cell, float thickness_coefficient, float min_thickness_m,
                   DepthFn&& depth_at, int width, int height, int* hit_x = nullptr,
                   int* hit_y = nullptr, FacingFn facing = FacingFn{}) {
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
        const float z_cell = depth_at(cx, cy);
        if (!(z_cell > 0.0f)) {
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
        const float thickness = CellThickness(z_cell, thickness_coefficient, min_thickness_m);
        if (CellHit(WAt(ray, sigma), WAt(ray, sigma_out), z_cell, thickness) &&
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
float MarchWholeRay(const ScreenRay& ray, float thickness_coefficient, float min_thickness_m,
                    int max_cells, DepthFn&& depth_at, int width, int height,
                    int* hit_x = nullptr, int* hit_y = nullptr,
                    FacingFn facing = FacingFn{}) {
    const bool skip = ray.sigma_enter <= 0.0f;
    return MarchSegment(ray, ray.sigma_enter, ray.sigma_exit, max_cells, skip,
                        thickness_coefficient, min_thickness_m, depth_at, width, height,
                        hit_x, hit_y, facing);
}

// 分段并行版（CPU 模拟 GPU 的 T² 个 lane）：每段等长 L，各自报本段最早命中，取 min。
// **必须与 MarchWholeRay 给出同一个 s** —— 这是 GPU 侧"分段 + min"能成立的全部依据。
template <typename DepthFn, typename FacingFn = AcceptAllFacing>
float MarchSlabs(const ScreenRay& ray, const DerivedParams& d, float min_thickness_m,
                 DepthFn&& depth_at, int width, int height, int* hit_x = nullptr,
                 int* hit_y = nullptr, FacingFn facing = FacingFn{}) {
    float best = kNoHit;
    int best_x = -1;
    int best_y = -1;
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
                                     d.thickness_coefficient, min_thickness_m, depth_at,
                                     width, height, &cx, &cy, facing);
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
