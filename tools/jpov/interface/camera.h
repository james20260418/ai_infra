// JPOV Camera — 透视相机配置
//
// Camera 现在属于 RenderCommandList：用户在 OneIteration 中设置 cmds->camera，
// 框架在 Render() 时自动使用该 Camera 计算 MVP 矩阵。
//
// 一个 RenderCommandList 有且仅有一个 Camera。不要在同一帧内修改或替换它。
// 不支持正交相机或多相机。

#ifndef JPOV_CAMERA_H_
#define JPOV_CAMERA_H_

#include "geom/common/vec.h"

namespace jpov {

// ==================== 类型别名 ====================

// 复用 geom 库的向量类型
using Vec3f = geom::Vec3<float>;

// ==================== Camera 结构体 ====================

// 透视相机
//
// position — 相机在世界空间的位置
// target   — 相机看向的目标点
// up       — 上方向向量（默认 {0, 1, 0}）
// fov      — 垂直视野角度（度），默认 60.0
// near     — 近裁剪面距离，默认 0.1
// far      — 远裁剪面距离，默认 1000.0
//
// viewport — 3D 世界渲染在窗口内的矩形区域
//            x, y: 矩形左上角在窗口中的位置（像素单位）
//            width, height: 矩形宽高（像素单位）
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

    // 3D 离屏 FBO 尺寸（像素单位）
    //
    // 决定 BuildMVP 时投影矩阵的宽高比和视野范围。
    // 所有 3D 命令（Triangle3D/Strip3D/Line3D/Text3D）先渲染到这个离屏 FBO，
    // 然后整个结果按 viewport 描述的屏幕位置 blit 到主 FBO。
    //
    // 与 viewport 的关系：
    //   fbo_3d_width_/height_ 决定 3D 场景的渲染分辨率（用于 MVP 计算），
    //   viewport_x/y/width/height 决定渲染结果在主 FBO 上的屏幕位置。
    //
    // 默认 0 表示与主 FBO 同尺寸，框架会在渲染前自动填入。
    float fbo_3d_width_ = 0.0f;   // 3D 离屏 FBO 宽度（像素）
    float fbo_3d_height_ = 0.0f;  // 3D 离屏 FBO 高度（像素）

    // 3D 视口矩形（窗口坐标，左上角为原点，所有值均为像素单位）
    // 0 宽高表示"全窗口"，框架会在渲染前自动填入实际窗口尺寸。
    float viewport_x = 0.0f;       // 视口左上角 x（像素）
    float viewport_y = 0.0f;       // 视口左上角 y（像素）
    float viewport_width = 0.0f;   // 视口宽度（像素）
    float viewport_height = 0.0f;  // 视口高度（像素）
};

}  // namespace jpov

#endif  // JPOV_CAMERA_H_
