// JPOV WindowInfo — 帧级窗口信息
//
// WindowInfo 在每帧开始时由窗口层填充，作为 OneIteration::Step() 的输入之一。
// OneIteration 通过 WindowInfo 获取窗口的像素尺寸，用于 2D 绘制的坐标计算
// 和 3D 投影的宽高比计算。
//
// 设计原则：
// - 只包含窗口尺寸信息，不含输入事件（输入事件走 InputSnapshot）
// - 值为当前帧窗口的实际像素尺寸（float 类型，与 InputSnapshot 的鼠标坐标类型一致，
//   允许子像素精度，但实际窗口分辨率应为整数，仍允许非整数值存在但不保证有意义）
// - 分辨率变化时下一帧自动更新

#ifndef JPOV_WINDOW_INFO_H_
#define JPOV_WINDOW_INFO_H_

#include <cmath>

#include <glog/logging.h>

namespace jpov {

// 帧级窗口信息
//
// width  — 窗口像素宽度（> 0）
// height — 窗口像素高度（> 0）
//
// Pre-condition: width > 0 && height > 0
// 窗口最小化或尺寸为零时，框架不调用 OneIteration::Step()
struct WindowInfo {
    float width;
    float height;

    // 宽高比（width / height），用于投影矩阵计算
    float AspectRatio() const {
        CHECK_GT(height, 0.0f);
        return width / height;
    }
};

}  // namespace jpov

#endif  // JPOV_WINDOW_INFO_H_
