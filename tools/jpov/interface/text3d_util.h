// JPOV 3D 文本工具 —— 像素/米 换算
//
// 背景：Text3DCommand 的尺寸单位是**世界米**（font_height_world），因为 3D 空间里
// 「多大」的自然单位是米。但用户/设计者在配字号时习惯用**像素**（"我要 24px 的字"）。
//
// 本文件提供二者之间的换算：给定世界位置与相机，算出该处「1 米 = 多少像素」。
//
// ---- 为什么不做成 JPOV 上的成员函数 ----
// 换算只依赖 {anchor, camera, 渲染分辨率}，全是调用方在 OneIteration 里已经
// 持有的量。做成纯函数（无隐藏状态、不读上一帧相机）符合 minimal surprise：
//   - 不引入「首帧不准 / 相机突变滞后一帧」这类隐式时滞；
//   - 可在任何地方调用（含测试），不需要 JPOV 实例。
//
// ---- 推导（透视投影） ----
// 设相机到 anchor 的视线方向为 d = anchor - position，视线距离
//   z = dot(d, forward)     （forward = normalize(target - position)）
//
// 这是 anchor 在相机空间中的**深度**（沿光轴的距离，不是欧氏距离）。
//
// 竖直方向：投影矩阵中 proj[5] = f = 1/tan(fov_y/2)，即
//   y_ndc = f * y_view / (-z_view)，而 z_view = -z（右手系相机看向 -z）
//   ⇒ y_ndc = f * y_view / z
// NDC 的 y 范围是 [-1, 1]（跨度 2），映射到 fbo_h 像素（跨度 fbo_h）：
//   px_per_view_unit = (fbo_h / 2) * f / z = fbo_h * f / (2 * z)
//
// 而「1 米的世界长度」在**垂直于视线**的方向上，恰是 1 个 view-space 单位
// （相机仅旋转不平移缩放，保持长度）。所以：
//   px_per_meter = fbo_h * f / (2 * z) = fbo_h / (2 * z * tan(fov_y / 2))
//
// 这给出的是 **anchor 处的换算比**（局部线性化）。注意它随深度衰减（近大远小），
// 因此对**一个横跨较大深度的文本**，用 anchor 处的比值只是近似 —— 若需精确，
// 用户可自行对首尾两点各算一次再取平均（本接口只提供单点原语）。
//
// ---- 使用示例 ----
//   // 想让这段标签在屏幕上看起来是 24px 高：
//   float ppm = jpov::PixelsPerMeterAt(anchor, cmds->camera, kFboH);
//   float h_world = 24.0f / ppm;
//   cmds->DrawText3D("Hello", anchor, face, up, h_world, color, "default");

#ifndef JPOV_INTERFACE_TEXT3D_UTIL_H_
#define JPOV_INTERFACE_TEXT3D_UTIL_H_

#include <cmath>

#include <glog/logging.h>

#include "tools/jpov/interface/camera.h"

namespace jpov {

// PixelsPerMeterAt: 返回世界位置 anchor 处「1 米 = 多少屏幕像素」的换算比。
//
// 参数：
//   anchor:        世界空间位置（通常用 Text3DCommand::anchor）
//   cam:           当前帧的相机（含 fov / position / target / up）
//   fbo_height_px: 3D 渲染目标的高度（像素），= Camera::fbo_3d_height_
//
// 返回：像素/米（> 0）。
//
// Pre-condition: fbo_height_px > 0
// Pre-condition: cam.fov in (0, 180)
// Pre-condition: anchor 在相机前方（depth > 0）；anchor 落在相机平面/后方时
//                换算无意义 → LOG(FATAL) crash（不返回 inf/负值静默传播）。
//
// 说明：fov 是**竖直**视野（与 BuildPerspProj 一致），故用 fbo 高度而非宽度。
inline float PixelsPerMeterAt(const Vec3f& anchor, const Camera& cam,
                              int fbo_height_px) {
    CHECK_GT(fbo_height_px, 0) << "PixelsPerMeterAt: fbo_height_px 必须 > 0";
    CHECK_GT(cam.fov, 0.0f) << "PixelsPerMeterAt: fov 必须 > 0";
    CHECK_LT(cam.fov, 180.0f) << "PixelsPerMeterAt: fov 必须 < 180";

    // forward = normalize(target - position)
    const float fx = cam.target.x() - cam.position.x();
    const float fy = cam.target.y() - cam.position.y();
    const float fz = cam.target.z() - cam.position.z();
    const float fl = std::sqrt(fx*fx + fy*fy + fz*fz);
    CHECK_GT(fl, 1e-8f)
        << "PixelsPerMeterAt: camera.position 与 target 重合，无法定义视线方向";
    const float inv_fl = 1.0f / fl;

    // depth = dot(anchor - position, forward)
    const float dx = anchor.x() - cam.position.x();
    const float dy = anchor.y() - cam.position.y();
    const float dz = anchor.z() - cam.position.z();
    const float depth = (dx * fx + dy * fy + dz * fz) * inv_fl;

    CHECK_GT(depth, 1e-6f)
        << "PixelsPerMeterAt: anchor 不在相机前方（depth=" << depth
        << " ≤ 0），换算比无定义（该处不会出现在画面上）";

    // f = 1 / tan(fov_y / 2)
    constexpr float kPi = 3.14159265358979323846f;
    const float half_fov = cam.fov * 0.5f * (kPi / 180.0f);
    const float f = 1.0f / std::tan(half_fov);

    // px_per_meter = fbo_h * f / (2 * depth)
    const float ppm = static_cast<float>(fbo_height_px) * f / (2.0f * depth);
    CHECK_GT(ppm, 0.0f) << "PixelsPerMeterAt: 换算比非正（内部计算异常）";
    return ppm;
}

}  // namespace jpov

#endif  // JPOV_INTERFACE_TEXT3D_UTIL_H_
