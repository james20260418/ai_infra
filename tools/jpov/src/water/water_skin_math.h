// JPOV 水皮（water skin）—— GL-free 的 CPU 参考模型 + 常量
//
// 为什么单独一个头（与 SSR 的 ssr_math.h 同一套做法）：
//   水皮的全部判据只有四件事 —— 「视线是否打中 y=level 的平面」「水面是否被场景挡住」
//   「菲涅尔反射率」「反射方向要不要夹到地平线以上」。
//   GLSL 里必须再写一遍（着色器不能 include C++），所以这里放**CPU 参考实现**：
//   · 单测（test/water/water_skin_math_test.cc）钉住物理量与边界；
//   · GPU 像素测试用同一份模型和着色器逐像素对拍 —— 任何一边改歪都会红。
//   常量（地平线夹、深度容差）也只有这一份，渲染器把它作为 uniform 传给着色器。
//
// ⚠️ 改这里的公式必须同步改 water_skin_shader.h（两处都标了「同源」），
//    并由 test/water:water_skin_gpu_test 的逐像素对拍兜底。

#ifndef JPOV_SRC_WATER_WATER_SKIN_MATH_H_
#define JPOV_SRC_WATER_WATER_SKIN_MATH_H_

#include <cmath>

#include "tools/jpov/interface/camera.h"
#include "tools/jpov/src/lighting/ggx_specular.h"   // E_ss 表 / 多散射补偿（M2）
#include "tools/jpov/src/water/water_optics.h"   // 菲涅尔 / 临界角 / ior（唯一出处）   // Vec3f（header-only，无 GL）
#include "tools/jpov/src/water/water_wave_math.h"  // 波浪高度场求交（WaterWaves / IntersectWaveSurface）

namespace jpov {
namespace water {

// 水面法线恒为世界 +Y（本版水皮是**纯平面**：没有波浪法线，也没有几何位移）。
// 实现上直接写进公式（翻转 y / 用 N=(0,1,0)），不需要单独常量。

// 反射方向夹到地平线以上的最小 y（= sin 1°）。
// 探针下半球是「地色」语义（见 SkyOutput::kEnvProbe）：掠射反射不夹的话会采到地色，
// 在水平线上画出一条黑边 —— 2026-10-28 实测过，别再重新发现一次。
inline constexpr float kHorizonClampSin = 0.0174524064f;

// 命中点与场景深度的比较容差（米）：水面比不透明场景**更远**超过它 ⇒ 被挡住、不画。
// 贴着水面的几何（岛屿侧面、水下物体）会在水线处与水面深度几乎相等，容差太小会打架。
inline constexpr float kDepthBiasM = 0.02f;

// 「场景深度 = 背景（远平面）」的判定阈值。MRT#1 里背景写 1.0。
inline constexpr float kBackgroundDepthNdc = 0.999999f;

// 视线与 y = level 平面的交点参数 t（米，沿单位方向向量）。
// 返回 false = 视线与平面平行（|dir_y| 过小）或交点在相机后方（t <= 0）。
// Pre-condition: dir 是单位向量。
inline bool PlaneHitDistance(float cam_y, float level_y, float dir_y, float* t /*output*/) {
    if (t == nullptr) {
        return false;
    }
    if (std::fabs(dir_y) < 1.0e-6f) {
        return false;
    }
    const float hit_t = (level_y - cam_y) / dir_y;
    if (hit_t <= 0.0f) {
        return false;
    }
    *t = hit_t;
    return true;
}

// 纯平面反射方向：incident 是**指向水面**的视线方向，法线恒 (0,1,0) ⇒ 翻转 y。
inline Vec3f ReflectDirection(const Vec3f& incident) {
    return Vec3f{incident.x(), -incident.y(), incident.z()};
}

// 绕任意单位法线 N 的反射：incident 指向水面，N 朝上（可为波浪法线）。
// N = (0,1,0) 时与 ReflectDirection 逐位一致。
// Pre-condition: N 为单位向量。
inline Vec3f ReflectDirectionAbout(const Vec3f& incident, const Vec3f& normal) {
    const float d = incident.x() * normal.x() + incident.y() * normal.y() +
                    incident.z() * normal.z();
    return Vec3f{incident.x() - 2.0f * d * normal.x(),
                 incident.y() - 2.0f * d * normal.y(),
                 incident.z() - 2.0f * d * normal.z()};
}

// 把反射方向夹到地平线以上并重新归一化。
//
// ⚠️ 「先夹 y、再归一化」会把 y 拉回夹值以下（缩放因子 < 1）—— 这里改成
//    「夹住 y，再把 xz 缩放到 sqrt(1−y²)」，使结果**精确**在单位球上且 y ≥ 夹值。
// Pre-condition: dir 非零。
inline Vec3f ClampReflectionAboveHorizon(const Vec3f& dir) {
    const float xz = std::sqrt(dir.x() * dir.x() + dir.z() * dir.z());
    if (dir.y() >= kHorizonClampSin) {
        // 已经在地平线以上：只做归一化。
        const float len = std::sqrt(xz * xz + dir.y() * dir.y());
        if (len < 1.0e-8f) {
            return Vec3f{0.0f, 1.0f, 0.0f};
        }
        return Vec3f{dir.x() / len, dir.y() / len, dir.z() / len};
    }
    if (xz < 1.0e-6f) {
        // 正下方（反射方向上不会出现；防御）：退化成正上方。
        return Vec3f{0.0f, 1.0f, 0.0f};
    }
    const float scale = std::sqrt(1.0f - kHorizonClampSin * kHorizonClampSin) / xz;
    return Vec3f{dir.x() * scale, kHorizonClampSin, dir.z() * scale};
}

// 由 NDC 深度（= gl_FragCoord.z ∈ [0,1]，也是 MRT#1 场景深度的编码）
// 反算**视轴深度**（米，正数）。与 strip/fog 里的线性化公式同一份。
inline float LinearizeDepthNdc(float ndc_z, float near_plane, float far_plane) {
    const float ndc = ndc_z * 2.0f - 1.0f;
    const float denom = far_plane + near_plane - ndc * (far_plane - near_plane);
    if (std::fabs(denom) < 1.0e-8f) {
        return far_plane;
    }
    return 2.0f * near_plane * far_plane / denom;
}

// 一个像素的水面判定结果（= 着色器里那段几何/物理的 CPU 版）。
struct WaterPixel {
    bool hit = false;          // 打中水面且没被场景挡住
    float view_z = 0.0f;       // 命中点的视轴深度（米）
    float cos_i = 0.0f;        // 入射角余弦（|dot(rd, N)|）
    float reflectance = 0.0f;  // 界面反射率（= WBO 的 α）
    Vec3f normal = {0.0f, 1.0f, 0.0f};        // 交点法线（波浪时为扰动法线）
    Vec3f reflect_dir = {0.0f, 1.0f, 0.0f};   // 反射方向（已夹地平线；水下不使用）
};

// 逐像素判定：视线 ∩ 水面 → 场景深度拒绝 → 菲涅尔。
//   cam_pos / cam_fwd  相机位置与朝向（cam_fwd 用来把 t 换成视轴深度，与 MRT#1 同口径）
//   ray_dir            该像素的世界射线方向（单位向量）
//   scene_ndc_z        MRT#1 场景深度（1.0 = 背景/远平面）
//   waves              波浪谱（nullptr / !enabled / num_waves=0 ⇒ 纯平面，与旧版逐字节一致）
//   flow_x / flow_z    waves 非空时的 flow 向量（CPU 参考在整条射线取同一值）
// Pre-condition: ray_dir 与 cam_fwd 单位化且 dot > 0；0 < near < far。
inline bool EvaluateWaterPixel(const Vec3f& cam_pos, const Vec3f& cam_fwd,
                               const Vec3f& ray_dir, float level_y, float ior,
                               float scene_ndc_z, float near_plane, float far_plane,
                               WaterPixel* out /*output*/,
                               const WaterWaves* waves = nullptr,
                               float flow_x = 0.0f, float flow_z = 0.0f,
                               float pixel_angle_rad = 0.0f,
                               float time_s = 0.0f,
                               float depth_m = 300.0f,
                               bool cull_subpixel_normals = true,
                               bool multiscatter = true,
                               float lobe_var_scale = 1.0f,
                               float lod_px_min = kWaveLodPxMin,
                               float lod_px_max = kWaveLodPxMax) {
    if (out == nullptr) {
        return false;
    }
    *out = WaterPixel{};

    float t = 0.0f;
    Vec3f normal{0.0f, 1.0f, 0.0f};
    float footprint = 0.0f;   // 像素足迹（供 F_eff 用；着色器同一公式）
    if (waves != nullptr && waves->enabled && waves->num_waves > 0) {
        // 像素在水面上覆盖的世界尺寸：**调同一个口径函数**（WaveFootprintM），
        // 不在这里再写一遍公式 —— 抄两份就会漂（掠射护栏改变时只改到一边）。
        const float t_plane = (level_y - cam_pos.y()) / ray_dir.y();
        footprint = WaveFootprintM(*waves, t_plane, ray_dir.y(), pixel_angle_rad);
        WaveSurfaceHit wh;
        if (!IntersectWaveSurface(cam_pos, ray_dir, level_y, *waves, flow_x, flow_z,
                                  time_s, footprint, &wh, depth_m, cull_subpixel_normals,
                                  lod_px_min, lod_px_max)) {
            return false;
        }
        t = wh.t;
        normal = wh.normal;
    } else {
        if (!PlaneHitDistance(cam_pos.y(), level_y, ray_dir.y(), &t)) {
            return false;
        }
    }

    // 视轴深度：t × (射线在相机前方向上的投影)。MRT#1 存的是 gl_FragCoord.z，
    // 它编码的是视轴深度（不是欧氏距离），所以两者必须同口径比较。
    const float dir_dot_fwd = ray_dir.x() * cam_fwd.x() +
                              ray_dir.y() * cam_fwd.y() +
                              ray_dir.z() * cam_fwd.z();
    if (dir_dot_fwd <= 1.0e-6f) {
        return false;   // 射线在相机侧后方（正常透视不会出现，防御）
    }
    const float view_z = t * dir_dot_fwd;

    // 场景遮挡：只有「有几何」时才比较；背景（远平面）恒被水覆盖 ——
    // 这正是「水面盖住天球下半球地色」那条（2026-10-28 修的地色带）。
    if (scene_ndc_z < kBackgroundDepthNdc) {
        const float scene_z = LinearizeDepthNdc(scene_ndc_z, near_plane, far_plane);
        if (view_z > scene_z + kDepthBiasM) {
            return false;
        }
    }

    out->hit = true;
    out->view_z = view_z;
    out->normal = normal;
    // 入射角余弦：与法线夹角（波浪下不能再写 |rd.y|）。
    out->cos_i = std::fabs(ray_dir.x() * normal.x() + ray_dir.y() * normal.y() +
                           ray_dir.z() * normal.z());
    out->reflect_dir = ClampReflectionAboveHorizon(ReflectDirectionAbout(ray_dir, normal));
    if (cam_pos.y() < level_y) {
        out->reflectance = FresnelDielectric(out->cos_i, ior, 1.0f);   // 水→空气（含 TIR）
    } else {
        out->reflectance = FresnelDielectric(out->cos_i, 1.0f, ior);   // 空气→水
        // ③ 反射率/透射率随粗糙度重分配（与着色器同一条）：投影到入射面后取角度平均菲涅尔。
        if (waves != nullptr && waves->enabled && waves->num_waves > 0 && t > 0.0f) {
            const float dx = ray_dir.x() * t;
            const float dz = ray_dir.z() * t;
            const float hdist = std::sqrt(dx * dx + dz * dz);
            const SlopeVariance v =
                WaveFilteredVariance(*waves, footprint, WaveFadeFactor(*waves, hdist),
                                     cull_subpixel_normals, lod_px_min, lod_px_max);
            const float rl = std::sqrt(ray_dir.x() * ray_dir.x() + ray_dir.z() * ray_dir.z());
            const float tx = (rl > 1.0e-6f) ? ray_dir.x() / rl : 1.0f;
            const float tz = (rl > 1.0e-6f) ? ray_dir.z() / rl : 0.0f;
            const float var_p = tx * tx * v.xx + 2.0f * tx * tz * v.xz + tz * tz * v.zz;
            // ③ 单次散射反射率 → ④ 多散射补偿（M2，与着色器同一条）：
            //    粗糙面 Smith 遮蔽吃掉的那部分能量 (1−E_ss) 按 Kulla–Conty 补回。
            //    R 与 T 一起动（`read->reflectance` 是唯一的 R 出口），保证 R+T=1。
            const float f_ss = RoughFresnelAlbedo(out->cos_i, var_p, ior);
            (void)lobe_var_scale;   // 只影响太阳的像（不参与水皮的 C/α）
            out->reflectance =
                multiscatter
                    ? std::min(f_ss * lighting::GgxMultiscatterScale(
                                           out->cos_i, var_p, FresnelAvgDielectric(ior)),
                               1.0f)
                    : f_ss;
        }
    }
    return true;
}

}  // namespace water
}  // namespace jpov

#endif  // JPOV_SRC_WATER_WATER_SKIN_MATH_H_
