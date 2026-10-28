#include "tools/jpov/tools/vegetation_tool_v2/gen/shape_text.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace jpov {
namespace vegetation {
namespace gen {
namespace {

// 数字 → 文本：**最短的可逆表示**（round-trip exact）。
//
// 为什么不能用 %.6g：文本是资产格式的另一半（`shape_convert` 把 `.vegshape` 转出来给人改，
// 改完再转回去），6 位有效数字会把 double 砍掉 —— 二进制 → 文本 → 二进制就不再逐字节相同
// （实测 0.25691019529015585 被写成 0.256910，读回来是另一个 double）。
// 「文本只用来查看」不等于「文本可以丢精度」：丢精度以后 diff 里看到的数和资产里的数不是
// 同一个数，review 改参数都会被骗。
//
// 做法：从 1 位有效数字往上试，第一个「读回来仍是同一个 double」的写法就是最短可逆表示。
// 好处是常见的短数字（0.25691 / 1.5 / -0.08）仍然又短又好读，只有真需要更多位才变长；
// 代价是每个数最多 17 次 snprintf+strtod（转换工具本来就是离线一次性跑）。
// 注意 -0 会写成 "-0"（负零的符号对法线/手性判定有意义，不能丢）。
std::string FormatNumber(double value) {
  char buffer[64];
  if (!std::isfinite(value)) {
    // inf / -inf / nan：protobuf 文本格式要的就是这三个小写拼写。
    std::snprintf(buffer, sizeof(buffer), "%.17g", value);
    return buffer;
  }
  for (int precision = 1; precision <= 17; ++precision) {
    std::snprintf(buffer, sizeof(buffer), "%.*g", precision, value);
    if (std::strtod(buffer, nullptr) == value) {
      return buffer;
    }
  }
  // 理论上到不了这里（%.17g 对 IEEE double 一定可逆），留作兜底。
  std::snprintf(buffer, sizeof(buffer), "%.17g", value);
  return buffer;
}

// 文本 proto 的字符串字面量：只转义反斜杠和双引号（与手写产物保持一致）。
std::string QuoteString(const std::string& text) {
  std::string quoted = "\"";
  for (const char character : text) {
    if (character == '\\' || character == '"') {
      quoted.push_back('\\');
    }
    quoted.push_back(character);
  }
  quoted.push_back('"');
  return quoted;
}

void AppendPoint(std::string* out, const char* indent, const char* field, const Vec3d& point) {
  *out += indent;
  *out += field;
  *out += " { x: ";
  *out += FormatNumber(point.x);
  *out += " y: ";
  *out += FormatNumber(point.y);
  *out += " z: ";
  *out += FormatNumber(point.z);
  *out += " }\n";
}

}  // namespace

std::string FormatShapeText(const Shape& shape) {
  std::string out;
  out += "version: " + std::to_string(shape.version) + "\n";
  out += "description: " + QuoteString(shape.description) + "\n";

  for (const Branch& branch : shape.branches) {
    out += "branches {\n";
    out += "  id: " + std::to_string(branch.id) + "\n";
    out += "  parent: " + std::to_string(branch.parent) + "\n";
    out += "  level: " + std::to_string(branch.level) + "\n";
    out += "  ring_count: " + std::to_string(branch.ring_count) + "\n";
    out += "  seam_index: " + std::to_string(branch.seam_index) + "\n";
    for (const BranchSample& sample : branch.samples) {
      out += "  samples {\n";
      AppendPoint(&out, "    ", "position", sample.position);
      out += "    radius: " + FormatNumber(sample.radius) + "\n";
      out += "    tilt: " + FormatNumber(sample.tilt) + "\n";
      out += "  }\n";
    }
    out += "}\n";
  }

  for (const LeafType& type : shape.leaf_types) {
    out += "leaf_types {\n";
    out += "  type: " + std::to_string(type.type) + "\n";
    out += "  description: " + QuoteString(type.description) + "\n";
    out += "  anchor_mode: " + QuoteString(type.anchor_mode) + "\n";
    // 全 0（平面）就不写：proto 的「没写 = 0」在这里是**语义**（旧资产就是这个样），
    // 写一堆 0 只会让人以为弯曲被显式关掉了。
    if (!type.bend.IsFlat()) {
      out += "  bend {\n";
      out += "    along: " + FormatNumber(type.bend.along) + "\n";
      out += "    across: " + FormatNumber(type.bend.across) + "\n";
      out += "    crease: " + FormatNumber(type.bend.crease) + "\n";
      out += "  }\n";
    }
    out += "}\n";
  }

  for (const LeafCard& card : shape.cards) {
    out += "cards {\n";
    AppendPoint(&out, "  ", "anchor", card.anchor);
    AppendPoint(&out, "  ", "up", card.up);
    AppendPoint(&out, "  ", "normal", card.normal);
    out += "  height: " + FormatNumber(card.height) + "\n";
    out += "  width: " + FormatNumber(card.width) + "\n";
    out += "  type: " + std::to_string(card.type) + "\n";
    out += "  branch: " + std::to_string(card.branch) + "\n";
    out += "  s: " + FormatNumber(card.s) + "\n";
    out += "}\n";
  }
  return out;
}

}  // namespace gen
}  // namespace vegetation
}  // namespace jpov
