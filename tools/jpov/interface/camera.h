// JPOV Camera — 透视相机配置
//
// Camera 由外部设置，在 OneIteration::Step() 中传入。
// OneIteration 消费 Camera 来决定 3D 世界空间到屏幕空间的投影。
//
// MVP 阶段只支持单个固定透视相机。不支持正交相机或多相机。
// 用户不直接接触投影/视图矩阵——OneIteration 内部使用 Camera 构建这些矩阵。

#ifndef JPOV_CAMERA_H_
#define JPOV_CAMERA_H_

#include "render_command.h"

namespace jpov {

// 透视相机
//
// position — 相机在世界空间的位置
// target   — 相机看向的目标点
// up       — 上方向向量（默认 {0, 1, 0}，不影响渲染结果的默认值）
// fov      — 垂直视野角度（度），默认 60.0
// near     — 近裁剪面距离，默认 0.1
// far      — 远裁剪面距离，默认 1000.0
//
// Pre-condition:
//   - fov in [1, 179]（有效视野范围）
//   - near > 0 && far > near（裁剪面有效）
//   - up 不是零向量
struct Camera {
    Vec3  position = {0.0f, 0.0f, 10.0f};
    Vec3  target   = {0.0f, 0.0f, 0.0f};
    Vec3  up       = {0.0f, 1.0f, 0.0f};
    float fov      = 60.0f;
    float near     = 0.1f;
    float far      = 1000.0f;
};

}  // namespace jpov

#endif  // JPOV_CAMERA_H_
