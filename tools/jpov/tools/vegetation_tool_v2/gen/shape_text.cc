#include "tools/jpov/tools/vegetation_tool_v2/gen/shape_text.h"

#include <cstdio>
#include <string>

namespace jpov {
namespace vegetation {
namespace gen {
namespace {

// 与 Python 版一致：6 位有效数字。
std::string FormatNumber(double value) {
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%.6g", value);
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
