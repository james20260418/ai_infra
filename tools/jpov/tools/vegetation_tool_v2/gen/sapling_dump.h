// gen/sapling_dump.h — 读 Blender 侧倒出来的原始树数据（见 dump_sapling_curve.py）。
//
// 这个文件只负责**解析**：把一段没有分支的文本变成结构体。任何几何决策
// （采样、父枝、叶卡姿态、拓扑序）都不在这里，见 shape_builder.h。
//
// 数据是 Blender 原生坐标系：**+Z up**、**世界坐标**（对象矩阵已经乘进去）。
// +Z→+Y 的换算由 shape_builder 在写 shape 时统一做。

#pragma once

#include <string>
#include <vector>

#include "tools/jpov/tools/vegetation_tool_v2/shape.h"

namespace jpov {
namespace vegetation {
namespace gen {

// 一个 Bezier 控制点（poly 样条也用它，没有 handle 时填 0）。
struct DumpSplinePoint {
  Vec3d co;
  Vec3d handle_left;
  Vec3d handle_right;
  double radius = 0.0;
};

// 一条样条。`is_bezier == false` 时 points 是折线顶点，不再求值。
struct DumpSpline {
  bool is_bezier = true;
  std::vector<DumpSplinePoint> points;
};

// 叶卡网格的一个顶点（世界坐标 + UV）。缺 UV 时 u/v 为负。
struct DumpLeafVertex {
  Vec3d position;
  double u = -1.0;
  double v = -1.0;
};

// 一次 Sapling 建树的完整原始数据。
struct SaplingDump {
  double bevel_depth = 0.0;
  std::vector<DumpSpline> splines;
  std::vector<DumpLeafVertex> leaf_vertices;
};

// 解析 dump 文件。
// 失败时返回 false 并把原因写进 *error（含行号）——不要用崩掉来表达「文件写坏了」。
bool LoadSaplingDump(const std::string& path, SaplingDump* out /*output*/,
                     std::string* error /*output*/);

}  // namespace gen
}  // namespace vegetation
}  // namespace jpov
