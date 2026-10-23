// gen/shape_text.h — Shape 运行时结构 → shape.pbtxt 文本（与资产格式的字段一一对应）。
//
// 为什么自己拼文本而不是用 protobuf 的 TextFormat：
//   * 输出要和 Python 版（export_vegetation_shape.py 手写的那份）**看起来一样**，
//     方便 diff / review / gold test 对照；TextFormat 会把 `position { x: .. }` 拆成
//     多行，布局完全不同。
//   * 字段顺序、缩进、`%.6g` 的浮点格式都是资产格式约定的一部分（人要读它）。

#pragma once

#include <string>

#include "tools/jpov/tools/vegetation_tool_v2/shape.h"

namespace jpov {
namespace vegetation {
namespace gen {

// 把 Shape 序列化成文本 proto。永远成功（数据非法应由 BuildShape/Validate 拦）。
std::string FormatShapeText(const Shape& shape);

}  // namespace gen
}  // namespace vegetation
}  // namespace jpov
