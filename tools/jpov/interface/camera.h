// JPOV Camera — 透视相机配置
//
// Camera 由外部设置，在 OneIteration::Step() 中传入。
// OneIteration 消费 Camera 来决定 3D 世界空间到屏幕空间的投影。
//
// MVP 阶段只支持单个固定透视相机。不支持正交相机或多相机。
// 用户不直接接触投影/视图矩阵——OneIteration 内部使用 Camera 构建这些矩阵。

#ifndef JPOV_CAMERA_H_
#define JPOV_CAMERA_H_

#include "geom/common/vec.h"

namespace jpov {

// 透视相机
//
// position — 相机在世界空间的位置
// target   — 相机看向的目标点
// up       — 上方向向量（默认 {0, 1, 0}）
// fov      — 垂直视野角度（度），默认 60.0
// near     — 近裁剪面距离，默认 0.1
// far      — 远裁剪面距离，默认 1000.0
//
// viewport — 3D 世界渲染在窗口内的矩形区域（窗口坐标）
//            x,y: 矩形左上角在窗口中的位置
//            width, height: 矩形宽高
//            默认值：x=y=0, width=height=0（表示全窗口）
//            2D 绘制不受 viewport 影响，始终绘制在全窗口范围。
//
// Pre-condition:
//   - fov in [1, 179]（有效视野范围）
//   - near > 0 && far > near（裁剪面有效）
//   - up 不是零向量
//   - viewport.width >= 0 && viewport.height >= 0
struct Camera {
    Vec3f position = {0.0f, 0.0f, 10.0f};
    Vec3f target   = {0.0f, 0.0f, 0.0f};
    Vec3f up       = {0.0f, 1.0f, 0.0f};
    float fov      = 60.0f;
    float near     = 0.1f;
    float far      = 1000.0f;

    // 3D 视口矩形（窗口坐标，左上角为原点）
    // 用 0 宽高表示"全窗口"，OneIteration 或渲染后端自动填入实际窗口尺寸
    float viewport_x = 0.0f;
    float viewport_y = 0.0f;
    float viewport_width = 0.0f;
    float viewport_height = 0.0f;
};

}  // namespace jpov

#endif  // JPOV_CAMERA_H_
