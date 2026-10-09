// JPOV 穿衣工具 — 相机基 / 屏幕投影（pure header-only，GL-free，可单测）。
//
// 用途：把「鼠标像素」与「世界点」互相换算，供 3D 画笔（射线选点）与顶点标记
// （世界顶点 → 屏幕方块）共用。
//
// ⚠️ 约定必须与渲染器 `Primitives3DRenderer::BuildMVP` 的 lookAt + 透视投影**逐项一致**，
//    否则标记/画笔会和画面错位。相机基（右手系）：
//      forward = normalize(target − eye)         相机前向（看向目标）
//      right   = normalize(cross(forward, up))   屏幕右向（= 渲染器里的 side）
//      up      = cross(right, forward)           屏幕上向
//
// 投影（vertical fov，ndc ∈ [-1,1]）：
//      depth   = dot(p − eye, forward)                 （> 0 在相机前方）
//      ndc.x   = dot(p − eye, right) / (depth · tan_half_fov · aspect)
//      ndc.y   = dot(p − eye, up)    / (depth · tan_half_fov)
//      screen  = ( (ndc.x·0.5 + 0.5)·W , (0.5 − ndc.y·0.5)·H )   （像素，y 向下）
//
// 反解（像素 → 世界射线）：
//      ndc.x = 2·px/W − 1 ; ndc.y = 1 − 2·py/H
//      dir   = normalize(forward + ndc.x·tan_half_fov·aspect·right + ndc.y·tan_half_fov·up)

#ifndef JPOV_CLOTHING_CAMERA_PROJECTION_H_
#define JPOV_CLOTHING_CAMERA_PROJECTION_H_

#include <cmath>

#include <glog/logging.h>

#include "tools/jpov/interface/camera.h"  // jpov::Vec3f

namespace jpov {
namespace clothing {

// 相机基（见文件头）。所有基向量均为单位向量。tan_half_fov = tan(fov_y / 2)。
struct CameraBasis {
    Vec3f  eye          = {0.0f, 0.0f, 0.0f};
    Vec3f  forward      = {0.0f, 0.0f, -1.0f};
    Vec3f  right        = {1.0f, 0.0f, 0.0f};
    Vec3f  up           = {0.0f, 1.0f, 0.0f};
    double tan_half_fov = 1.0;
    double aspect       = 1.0;
    float  screen_w     = 0.0f;
    float  screen_h     = 0.0f;
};

namespace camera_projection_detail {
inline Vec3f Sub(const Vec3f& a, const Vec3f& b) {
    return Vec3f(a.x() - b.x(), a.y() - b.y(), a.z() - b.z());
}
inline double Dot(const Vec3f& a, const Vec3f& b) {
    return static_cast<double>(a.x()) * b.x() + static_cast<double>(a.y()) * b.y() +
           static_cast<double>(a.z()) * b.z();
}
inline Vec3f Cross(const Vec3f& a, const Vec3f& b) {
    return Vec3f(static_cast<float>(static_cast<double>(a.y()) * b.z() -
                                   static_cast<double>(a.z()) * b.y()),
                 static_cast<float>(static_cast<double>(a.z()) * b.x() -
                                   static_cast<double>(a.x()) * b.z()),
                 static_cast<float>(static_cast<double>(a.x()) * b.y() -
                                   static_cast<double>(a.y()) * b.x()));
}
inline double Len(const Vec3f& v) {
    return std::sqrt(Dot(v, v));
}
// 归一化；退化（近零）时返回 fallback。
inline Vec3f Normalize(const Vec3f& v, const Vec3f& fallback) {
    const double n = Len(v);
    if (n < 1.0e-12) {
        return fallback;
    }
    const double inv = 1.0 / n;
    return Vec3f(static_cast<float>(v.x() * inv), static_cast<float>(v.y() * inv),
                 static_cast<float>(v.z() * inv));
}
}  // namespace camera_projection_detail

// 由 eye/target/world_up/fov/screen 构造相机基。
// Pre-condition: fov_deg ∈ (0,180)、screen_w>0、screen_h>0。
inline CameraBasis MakeCameraBasis(const Vec3f& eye, const Vec3f& target,
                                   const Vec3f& world_up, double fov_deg,
                                   float screen_w, float screen_h) {
    using namespace camera_projection_detail;
    CHECK_GT(fov_deg, 0.0);
    CHECK_LT(fov_deg, 180.0);
    CHECK_GT(screen_w, 0.0f);
    CHECK_GT(screen_h, 0.0f);

    CameraBasis c;
    c.eye      = eye;
    c.forward  = Normalize(Sub(target, eye), Vec3f(0.0f, 0.0f, -1.0f));
    c.right    = Normalize(Cross(c.forward, world_up), Vec3f(1.0f, 0.0f, 0.0f));
    c.up       = Cross(c.right, c.forward);  // right、forward 正交且单位 ⇒ up 也单位
    c.tan_half_fov = std::tan(fov_deg * 3.14159265358979323846 / 360.0);
    c.aspect       = static_cast<double>(screen_w) / static_cast<double>(screen_h);
    c.screen_w     = screen_w;
    c.screen_h     = screen_h;
    return c;
}

// 世界射线（origin 在相机；dir 单位）。
struct CameraRay {
    Vec3f origin = {0.0f, 0.0f, 0.0f};
    Vec3f dir    = {0.0f, 0.0f, -1.0f};
};

// 像素 → 世界射线（见文件头反解）。dir 已归一化。
inline CameraRay RayFromPixel(const CameraBasis& c, float px, float py) {
    using namespace camera_projection_detail;
    const double ndc_x = 2.0 * static_cast<double>(px) / c.screen_w - 1.0;
    const double ndc_y = 1.0 - 2.0 * static_cast<double>(py) / c.screen_h;
    const double kx = ndc_x * c.tan_half_fov * c.aspect;
    const double ky = ndc_y * c.tan_half_fov;
    const Vec3f raw(
        static_cast<float>(c.forward.x() + kx * c.right.x() + ky * c.up.x()),
        static_cast<float>(c.forward.y() + kx * c.right.y() + ky * c.up.y()),
        static_cast<float>(c.forward.z() + kx * c.right.z() + ky * c.up.z()));
    CameraRay r;
    r.origin = c.eye;
    r.dir    = Normalize(raw, c.forward);
    return r;
}

// 世界点 → 屏幕像素。返回 false = 在相机后方（depth ≤ 0）或退化；此时输出未定义。
// 成功时 *out_px/*out_py 为像素坐标（y 向下），*out_depth 为相机前向距离（可空）。
inline bool ProjectToScreen(const CameraBasis& c, const Vec3f& world,
                            float* out_px, float* out_py, float* out_depth) {
    using namespace camera_projection_detail;
    CHECK(out_px != nullptr);
    CHECK(out_py != nullptr);
    const Vec3f rel = Sub(world, c.eye);
    const double depth = Dot(rel, c.forward);
    if (depth <= 1.0e-9) {
        return false;
    }
    const double x_view = Dot(rel, c.right);
    const double y_view = Dot(rel, c.up);
    const double ndc_x = x_view / (depth * c.tan_half_fov * c.aspect);
    const double ndc_y = y_view / (depth * c.tan_half_fov);
    *out_px = static_cast<float>((ndc_x * 0.5 + 0.5) * c.screen_w);
    *out_py = static_cast<float>((0.5 - ndc_y * 0.5) * c.screen_h);
    if (out_depth != nullptr) {
        *out_depth = static_cast<float>(depth);
    }
    return true;
}

}  // namespace clothing
}  // namespace jpov

#endif  // JPOV_CLOTHING_CAMERA_PROJECTION_H_
