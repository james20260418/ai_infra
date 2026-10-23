#include "tools/jpov/tools/vegetation_tool_v2/gen/sapling_dump.h"

#include <cstdlib>
#include <fstream>
#include <sstream>

namespace jpov {
namespace vegetation {
namespace gen {
namespace {

// 读一行并切成 token；空行直接跳过（返回空 vector）。
std::vector<std::string> ReadTokens(std::istream* stream) {
  std::string line;
  while (std::getline(*stream, line)) {
    std::istringstream line_stream(line);
    std::vector<std::string> tokens;
    std::string token;
    while (line_stream >> token) {
      tokens.push_back(token);
    }
    if (!tokens.empty()) {
      return tokens;
    }
  }
  return {};
}

bool ParseDouble(const std::string& token, double* out) {
  char* end = nullptr;
  const double value = std::strtod(token.c_str(), &end);
  if (end == token.c_str() || *end != '\0') {
    return false;
  }
  *out = value;
  return true;
}

bool ParseInt(const std::string& token, int* out) {
  char* end = nullptr;
  const long value = std::strtol(token.c_str(), &end, 10);
  if (end == token.c_str() || *end != '\0') {
    return false;
  }
  *out = static_cast<int>(value);
  return true;
}

std::string LineError(int line_number, const std::string& message) {
  return "第 " + std::to_string(line_number) + " 行：" + message;
}

}  // namespace

bool LoadSaplingDump(const std::string& path, SaplingDump* out /*output*/,
                     std::string* error /*output*/) {
  std::ifstream stream(path);
  if (!stream) {
    *error = "打不开文件：" + path;
    return false;
  }
  *out = SaplingDump();

  int line_number = 0;
  std::string line;
  // 头一行：魔数 + 版本。
  if (!std::getline(stream, line)) {
    *error = "空文件：" + path;
    return false;
  }
  ++line_number;
  if (line != "jpov_sapling_dump 1") {
    *error = LineError(line_number, "文件头应为 'jpov_sapling_dump 1'，实际是 '" + line + "'");
    return false;
  }

  while (std::getline(stream, line)) {
    ++line_number;
    std::istringstream line_stream(line);
    std::vector<std::string> tokens;
    std::string token;
    while (line_stream >> token) {
      tokens.push_back(token);
    }
    if (tokens.empty()) {
      continue;
    }
    const std::string& tag = tokens[0];

    if (tag == "bevel_depth") {
      if (tokens.size() != 2 || !ParseDouble(tokens[1], &out->bevel_depth)) {
        *error = LineError(line_number, "bevel_depth 需要一个数字");
        return false;
      }
      continue;
    }

    if (tag == "spline") {
      if (tokens.size() != 3) {
        *error = LineError(line_number, "spline 应为 'spline bezier|poly <n>'");
        return false;
      }
      DumpSpline spline;
      if (tokens[1] == "bezier") {
        spline.is_bezier = true;
      } else if (tokens[1] == "poly") {
        spline.is_bezier = false;
      } else {
        *error = LineError(line_number, "未知的 spline 类型 '" + tokens[1] + "'");
        return false;
      }
      int count = 0;
      if (!ParseInt(tokens[2], &count) || count < 1) {
        *error = LineError(line_number, "spline 点数必须是正整数");
        return false;
      }
      spline.points.resize(static_cast<std::size_t>(count));
      for (int i = 0; i < count; ++i) {
        const std::vector<std::string> fields = ReadTokens(&stream);
        if (fields.empty()) {
          *error = LineError(line_number, "spline 的点不足（少 " + std::to_string(count - i) + " 个）");
          return false;
        }
        ++line_number;
        if (fields.size() != 11 || fields[0] != "p") {
          *error = LineError(line_number, "点应为 'p <co xyz> <hl xyz> <hr xyz> <radius>'");
          return false;
        }
        double values[10] = { 0.0 };
        for (int k = 0; k < 10; ++k) {
          if (!ParseDouble(fields[static_cast<std::size_t>(k) + 1], &values[k])) {
            *error = LineError(line_number, "点的第 " + std::to_string(k + 1) + " 个数不是数字");
            return false;
          }
        }
        DumpSplinePoint& point = spline.points[static_cast<std::size_t>(i)];
        point.co = Vec3d{ values[0], values[1], values[2] };
        point.handle_left = Vec3d{ values[3], values[4], values[5] };
        point.handle_right = Vec3d{ values[6], values[7], values[8] };
        point.radius = values[9];
      }
      out->splines.push_back(std::move(spline));
      continue;
    }

    if (tag == "leaves") {
      if (tokens.size() != 2) {
        *error = LineError(line_number, "leaves 应为 'leaves <n>'");
        return false;
      }
      int count = 0;
      if (!ParseInt(tokens[1], &count) || count < 0) {
        *error = LineError(line_number, "叶卡顶点数必须是非负整数");
        return false;
      }
      out->leaf_vertices.resize(static_cast<std::size_t>(count));
      for (int i = 0; i < count; ++i) {
        const std::vector<std::string> fields = ReadTokens(&stream);
        if (fields.empty()) {
          *error = LineError(line_number, "叶卡顶点不足（少 " + std::to_string(count - i) + " 个）");
          return false;
        }
        ++line_number;
        if (fields.size() != 6 || fields[0] != "v") {
          *error = LineError(line_number, "顶点应为 'v <xyz> <u> <v>'");
          return false;
        }
        double values[5] = { 0.0 };
        for (int k = 0; k < 5; ++k) {
          if (!ParseDouble(fields[static_cast<std::size_t>(k) + 1], &values[k])) {
            *error = LineError(line_number, "顶点的第 " + std::to_string(k + 1) + " 个数不是数字");
            return false;
          }
        }
        DumpLeafVertex& vertex = out->leaf_vertices[static_cast<std::size_t>(i)];
        vertex.position = Vec3d{ values[0], values[1], values[2] };
        vertex.u = values[3];
        vertex.v = values[4];
      }
      continue;
    }

    *error = LineError(line_number, "未知记录 '" + tag + "'");
    return false;
  }

  if (out->splines.empty()) {
    *error = "dump 里没有任何 spline";
    return false;
  }
  return true;
}

}  // namespace gen
}  // namespace vegetation
}  // namespace jpov
