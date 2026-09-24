// JPOV 软体仿真器 — 世界坐标 → 屏幕像素投影（纯 CPU、GL-free、可单测）
//
// 用途：把世界空间里的一个点投到屏幕像素，供查看器「把仿真点画成 2D 圆点」用
// （圆心需要屏幕坐标）。
//
// ⚠️ 本文件**必须与渲染层的 MVP 约定逐字一致**（`Primitives3DRenderer::BuildMVP`
//    + `BuildPerspProj`），否则圆圈会和 3D 网格错位。约定：
//     - 列主序矩阵、OpenGL 右手系
//     - lookAt 用「行主序书写、列主序存储」的转置写法（见彼处的重要坑注释）
//     - NDC 的 y 已由投影矩阵翻正（+y 向上），屏幕像素 +y 向下 → 输出时翻一次
//
// 之所以自实现而不复用渲染层：渲染层那套是 `src/` 的实现细节（含 GL 依赖），
// 仿真器包保持「零 GL」边界；这层薄数学放这里可被单测直接验证，是划算的。

#ifndef JPOV_SOFT_MESH_SIMULATOR_SCREEN_PROJECTION_H_
#define JPOV_SOFT_MESH_SIMULATOR_SCREEN_PROJECTION_H_

#include <cmath>

#include <glog/logging.h>

#include "geom/common/vec.h"

namespace jpov {
namespace soft_mesh_simulator {

// 透视投影所需的相机参数（裁剪空间前的完备信息）。
//
// 与本包的边界一致：不 include 渲染层的 Camera（避免依赖倒灌），只取需要的字段。
struct ProjectionCamera {
    geom::Vec3<float> position = geom::Vec3<float>(0.0f, 0.0f, 10.0f);
    geom::Vec3<float> target   = geom::Vec3<float>(0.0f, 0.0f, 0.0f);
    geom::Vec3<float> up       = geom::Vec3<float>(0.0f, 1.0f, 0.0f);
    float fov_deg = 60.0f;   // 垂直视野（度）
    float near    = 0.1f;
    float far     = 1000.0f;
    int   fbo_w   = 1280;    // 渲染目标宽（像素）
    int   fbo_h   = 720;     // 渲染目标高（像素）
};

// 投影结果。
//
//   visible == false → 点在相机背后/太近/超出 far，屏幕坐标无意义（**不要画**）
//   visible == true  → screen_x/screen_y 是像素坐标（左上角为原点，+y 向下），
//                      与 `RenderCommandList::DrawCircle` 的坐标空间一致
struct ScreenPoint {
    float x = 0.0f;
    float y = 0.0f;
    bool  visible = false;
};

// 把世界点投到屏幕像素。
//
// 实现步骤（与渲染层同源）：
//   1. view = lookAt(position, target, up)（列主序、转置写法）
//   2. proj = perspective(fov, aspect, near, far)（列主序）
//   3. clip = proj * view * (p, 1)
//   4. ndc = clip.xyz / clip.w；若 clip.w <= 0 → 相机背后 → visible=false
//   5. screen_x = (ndc.x*0.5 + 0.5) * fbo_w
//      screen_y = (1 - (ndc.y*0.5 + 0.5)) * fbo_h     ← y 翻一次（NDC +y 向上，
//                                                        屏幕 +y 向下）
//
// Pre-condition: fbo_w > 0, fbo_h > 0, near > 0, far > near, fov in (0,180)
inline ScreenPoint ProjectToScreen(const geom::Vec3<float>& p,
                                   const ProjectionCamera& cam) {
    CHECK_GT(cam.fbo_w, 0);
    CHECK_GT(cam.fbo_h, 0);
    CHECK_GT(cam.near, 0.0f);
    CHECK_GT(cam.far, cam.near);

    const float kPi = 3.14159265358979323846f;

    // ---- lookAt（与 Primitives3DRenderer::BuildMVP 逐字一致）----
    geom::Vec3<float> fwd = cam.target - cam.position;
    float f_len = std::sqrt(fwd.x() * fwd.x() + fwd.y() * fwd.y() + fwd.z() * fwd.z());
    if (f_len < 1e-8f) {
        fwd = geom::Vec3<float>(0.0f, 0.0f, -1.0f);
    } else {
        fwd = geom::Vec3<float>(fwd.x() / f_len, fwd.y() / f_len, fwd.z() / f_len);
    }

    geom::Vec3<float> side{
        fwd.y() * cam.up.z() - fwd.z() * cam.up.y(),
        fwd.z() * cam.up.x() - fwd.x() * cam.up.z(),
        fwd.x() * cam.up.y() - fwd.y() * cam.up.x()};
    float s_len = std::sqrt(side.x() * side.x() + side.y() * side.y() +
                            side.z() * side.z());
    if (s_len < 1e-8f) {
        side = geom::Vec3<float>(1.0f, 0.0f, 0.0f);
    } else {
        side = geom::Vec3<float>(side.x() / s_len, side.y() / s_len,
                                 side.z() / s_len);
    }

    geom::Vec3<float> upv{
        side.y() * fwd.z() - side.z() * fwd.y(),
        side.z() * fwd.x() - side.x() * fwd.z(),
        side.x() * fwd.y() - side.y() * fwd.x()};

    // view：列主序（view[0..2] = side，view[4..6] = upv，view[8..10] = -fwd）
    const float view[16] = {
        side.x(), upv.x(), -fwd.x(), 0.0f,
        side.y(), upv.y(), -fwd.y(), 0.0f,
        side.z(), upv.z(), -fwd.z(), 0.0f,
        -(side.x() * cam.position.x() + side.y() * cam.position.y() +
          side.z() * cam.position.z()),
        -(upv.x() * cam.position.x() + upv.y() * cam.position.y() +
          upv.z() * cam.position.z()),
        (fwd.x() * cam.position.x() + fwd.y() * cam.position.y() +
         fwd.z() * cam.position.z()),
        1.0f};

    // ---- 透视投影（与 Primitives3DRenderer::BuildPerspProj 逐字一致）----
    const float aspect = static_cast<float>(cam.fbo_w) / static_cast<float>(cam.fbo_h);
    const float fov_rad = cam.fov_deg * kPi / 180.0f;
    const float f = 1.0f / std::tan(fov_rad * 0.5f);
    const float range_inv = 1.0f / (cam.near - cam.far);
    const float proj[16] = {
        f / aspect, 0.0f, 0.0f, 0.0f,
        0.0f, f, 0.0f, 0.0f,
        0.0f, 0.0f, (cam.near + cam.far) * range_inv, -1.0f,
        0.0f, 0.0f, 2.0f * cam.near * cam.far * range_inv, 0.0f};

    // ---- MVP = proj * view（列主序）----
    float mvp[16];
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                sum += proj[k * 4 + r] * view[c * 4 + k];
            }
            mvp[c * 4 + r] = sum;
        }
    }

    // ---- 变换点 ----
    const float px = p.x(), py = p.y(), pz = p.z();
    const float clip_x = mvp[0] * px + mvp[4] * py + mvp[8] * pz + mvp[12];
    const float clip_y = mvp[1] * px + mvp[5] * py + mvp[9] * pz + mvp[13];
    const float clip_w = mvp[3] * px + mvp[7] * py + mvp[11] * pz + mvp[15];

    ScreenPoint out;
    if (clip_w <= 1e-8f) {
        return out;  // 相机背后（或退化）→ 不可见
    }
    const float ndc_x = clip_x / clip_w;
    const float ndc_y = clip_y / clip_w;
    out.x = (ndc_x * 0.5f + 0.5f) * static_cast<float>(cam.fbo_w);
    out.y = (1.0f - (ndc_y * 0.5f + 0.5f)) * static_cast<float>(cam.fbo_h);
    out.visible = true;
    return out;
}

}  // namespace soft_mesh_simulator
}  // namespace jpov

#endif  // JPOV_SOFT_MESH_SIMULATOR_SCREEN_PROJECTION_H_
