# opencv_util - OpenCV 绘图工具

基于 OpenCV 3.4.1 的轻量 2D 图形绘制工具，提供坐标系到像素的映射、网格、线条、文字等绘图功能。

## 目录结构

```
tools/opencv_util/
├── BUILD           # Bazel 构建配置
├── README.md       # 本文档
├── opencv_util.h   # 头文件
├── opencv_util.cpp # 实现
└── opencv_util_test.cpp  # GTest 测试
```

## 依赖

- **OpenCV 3.4.1** — 静态链接，位于 `third_party/opencv-static/`
- **geom::Vec2d** — 二维向量，位于 `geom/common/vec.h`
- **glog** — 日志库

## 核心类

### `opencv_util::Color`

颜色表示，RGB 各分量取值范围 0.0 ~ 1.0。

```cpp
// 创建颜色
auto c1 = opencv_util::Color::RGB(1.0, 0.0, 0.0);        // 红色
auto c2 = opencv_util::Color::RGBA(0.0, 1.0, 0.0, 0.5);  // 半透明绿色
auto c3 = opencv_util::Color(0.5, 0.5, 0.5, 1.0);        // 灰色
```

### `opencv_util::Figure`

画布。构造时指定世界坐标系范围（XY 区域）和像素尺寸。

```cpp
// 创建画布：X轴范围 [0, 19.5], Y轴范围 [0, 10.0], 1280x720 像素
opencv_util::Figure fig({0.0, 0.0}, {19.5, 10.0});
```

也支持嵌套子图：

```cpp
opencv_util::Figure sub({0.0, 0.0}, {10.0, 10.0});
sub.DrawGrid(1.0);
fig.FillFigure(sub, {-3.0, 3.0}, {8.0, 8.0});
```

#### 主要方法

| 方法 | 说明 |
|------|------|
| `DrawGrid(grid_size, origin, color, label_interval, label_color)` | 绘制网格，可标注坐标数值 |
| `DrawLine(p1, p2, color, width)` | 画线段 |
| `DrawArc(center, radius, width, start_angle, end_angle, samples, color)` | 画弧 |
| `FillRect(p1, p2, color)` | 填充矩形 |
| `FillMat(mat, p1, p2)` | 将 cv::Mat 图像填充到指定区域 |
| `FillFigure(other_fig, p1, p2)` | 将另一个 Figure 的内容作为图像填充 |
| `DrawText(p1, p2, text, color, thickness_ratio)` | 在矩形框内绘制单行文字 |
| `SaveAsPng(path, create_dir)` | 保存为 PNG 图片 |
| `ShowAndWait(window_name)` | 弹窗显示（需图形环境） |

### `opencv_util::MakeVideo`

将一组 Figure 帧合成为 AVI 视频。

```cpp
std::vector<opencv_util::Figure> frames;
// ... 构造帧
opencv_util::MakeVideo(frames, 60.0, "output.avi");
```

## 基本用法示例

```cpp
#include "tools/opencv_util/opencv_util.h"
#include "geom/common/vec.h"

using namespace geom;

// 创建画布
opencv_util::Figure fig({0.0, 0.0}, {16.0, 9.0});

// 画网格，每隔 2 格标注数值
fig.DrawGrid(1.0, Vec2d{0.0, 0.0}, 
    opencv_util::Color(0.5, 0.5, 0.5, 1.0), 2);

// 画线
fig.DrawLine(Vec2d{0.0, 0.0}, Vec2d{10.0, 5.0}, 
    opencv_util::Color::RGB(1.0, 0.0, 0.0), 0.1);

// 填充矩形
fig.FillRect(Vec2d{2.0, 2.0}, Vec2d{6.0, 6.0}, 
    opencv_util::Color(0.6, 0.0, 0.0));

// 保存
fig.SaveAsPng("output.png");
```

## 构建与测试

```bash
# 编译库
bazel build //tools/opencv_util:opencv_util

# 运行测试（输出到 /james/ai_infra/output/）
bazel test //tools/opencv_util:opencv_util_test
```

## 移植说明

本模块从 AutoDrive 项目的 `geom/opencv2_util` 移植而来：

| 原项目 | ai_infra |
|--------|----------|
| `opencv2_util` 命名空间 | `opencv_util` 命名空间 |
| `geom/opencv2_util.h` | `tools/opencv_util/opencv_util.h` |
| `geom/opencv2_util.cpp` | `tools/opencv_util/opencv_util.cpp` |

OpenCV 以静态库形式集成在 `third_party/opencv-static/`，版本 3.4.1，编译时开启 `BUILD_opencv_world=ON`，包含模块：core, imgproc, imgcodecs, highgui, videoio。
