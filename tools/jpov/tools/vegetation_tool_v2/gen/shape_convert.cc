// gen/shape_convert.cc — 植被资产格式互转：`*.vegshape`（二进制）⇄ `*.pbtxt`（文本）。
//
// 约定（2026-10-26 Danis 定）：
//   * **资产一律存二进制 `.vegshape`**（ShapeProto 的 protobuf wire format）——
//     三棵树实测文本 1.2~1.4 MB、二进制约 1/3，且加载不用解析文本；
//   * 文本**只用于人工查看 / diff / review**：要看就转出来，改完转回去，仓库不放文本资产。
//
// 用法：
//   shape_convert --in pine.vegshape --out pine.pbtxt            # 转成人能读的文本
//   shape_convert --in pine.pbtxt --out pine.vegshape            # 改完转回二进制
//   shape_convert --in a.vegshape --out b.vegshape --stats        # 只统计（同格式也允许）
//
// 格式由**扩展名**决定（`.vegshape` → 二进制，其余 → 文本），也可以用 --format 强制。
// 转换一定会过一遍 LoadShapeFile 的校验（拓扑 / 半径 / anchor 合同），坏资产转不出去。

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>

#include "tools/jpov/tools/vegetation_tool_v2/gen/shape_text.h"
#include "tools/jpov/tools/vegetation_tool_v2/shape.h"

namespace {

using jpov::vegetation::Shape;
using jpov::vegetation::ShapeFileFormat;
using jpov::vegetation::ShapeFileFormatFromPath;

int Fail(const std::string& message) {
  std::cerr << "错误: " << message << "\n";
  return 1;
}

// 用 ifstream 取大小（不用 <sys/stat.h>）：这个工具要在 Linux 和 Windows 交叉编译都能过。
std::uintmax_t FileSize(const std::string& path) {
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  if (!stream) {
    return 0;
  }
  return static_cast<std::uintmax_t>(stream.tellg());
}

std::string HumanSize(std::uintmax_t bytes) {
  char buffer[64];
  if (bytes >= 1000000) {
    std::snprintf(buffer, sizeof(buffer), "%.2f MB", static_cast<double>(bytes) / 1e6);
  } else {
    std::snprintf(buffer, sizeof(buffer), "%.1f KB", static_cast<double>(bytes) / 1e3);
  }
  return std::string(buffer);
}

// 统计口径与 README 一致：管面顶点 = 采样点 × 环数；
// 叶卡顶点 = 卡数 × (kBentCardSegments+1)² = 卡数 × 9（弯曲卡 2 段）。
// ⚠️ 平面卡（leaf type 的 bend 全 0）实际只占 4 顶点 / 2 三角形，这里按 9 / 8 计
// 会高估；要精确值就看 BuildLeafCards 的实际顶点数（viewer 的日志里有）。
void PrintStats(const Shape& shape) {
  std::uint64_t tube_vertices = 0;
  std::uint64_t tube_triangles = 0;
  for (const jpov::vegetation::Branch& branch : shape.branches) {
    const std::uint64_t ring = branch.EffectiveRingCount();
    tube_vertices += branch.samples.size() * ring;
    tube_triangles += (branch.samples.size() > 1 ? branch.samples.size() - 1 : 0) * ring * 2;
  }
  const std::uint64_t card_vertices = shape.cards.size() * 9;
  const std::uint64_t card_triangles = shape.cards.size() * 8;
  std::cout << "[stats] " << shape.branches.size() << " 枝 / " << shape.cards.size() << " 卡 / "
            << shape.leaf_types.size() << " leaf type | 顶点 " << tube_vertices + card_vertices
            << "（管 " << tube_vertices << " + 叶 " << card_vertices << "）| 三角形 "
            << tube_triangles + card_triangles << "（管 " << tube_triangles << " + 叶 "
            << card_triangles << "）\n";
}

}  // namespace

int main(int argc, char** argv) {
  std::string in_path;
  std::string out_path;
  std::string format_override;
  bool stats = false;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--in" && i + 1 < argc) {
      in_path = argv[++i];
    } else if (arg == "--out" && i + 1 < argc) {
      out_path = argv[++i];
    } else if (arg == "--format" && i + 1 < argc) {
      format_override = argv[++i];
    } else if (arg == "--stats") {
      stats = true;
    } else if (arg == "--help" || arg == "-h") {
      std::cout << "用法: shape_convert --in <file> --out <file> [--format text|binary] [--stats]\n"
                   "  格式默认按扩展名（.vegshape = 二进制，其余 = 文本）；--format 可强制。\n";
      return 0;
    } else {
      return Fail("未知参数: " + arg);
    }
  }
  if (in_path.empty() || out_path.empty()) {
    return Fail("必须给 --in 和 --out（--help 看用法）");
  }
  if (!format_override.empty() && format_override != "text" && format_override != "binary") {
    return Fail("--format 只能是 text 或 binary，实际: " + format_override);
  }

  const ShapeFileFormat in_format = ShapeFileFormatFromPath(in_path);
  const ShapeFileFormat out_format = format_override == "text"
                                         ? ShapeFileFormat::kText
                                         : (format_override == "binary"
                                                ? ShapeFileFormat::kBinary
                                                : ShapeFileFormatFromPath(out_path));

  Shape shape;
  std::string error;
  if (!jpov::vegetation::LoadShapeFile(in_path, &shape, &error)) {
    return Fail(error);
  }

  if (out_format == ShapeFileFormat::kBinary) {
    if (!jpov::vegetation::SaveShapeBinary(shape, out_path, &error)) {
      return Fail(error);
    }
  } else {
    const std::string text = jpov::vegetation::gen::FormatShapeText(shape);
    std::ofstream out(out_path);
    if (!out) {
      return Fail("写不了文件: " + out_path);
    }
    out << text;
  }

  std::cout << "[convert] " << in_path << "（" << (in_format == ShapeFileFormat::kBinary ? "binary" : "text")
            << ", " << HumanSize(FileSize(in_path)) << "）→ " << out_path << "（"
            << (out_format == ShapeFileFormat::kBinary ? "binary" : "text") << ", "
            << HumanSize(FileSize(out_path)) << "）\n";
  if (stats) {
    PrintStats(shape);
  }
  return 0;
}
