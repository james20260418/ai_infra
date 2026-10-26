// gen/shape_gen.cc — 生成配置 pbtxt → shape.vegshape（默认二进制；--text 出文本）。
//
//     configs/pine.pbtxt ──(本程序)──> blender -b --python dump_sapling_curve.py ──> .dump
//                                                                                    │
//                     shape.vegshape  <── 序列化 ◄──── BuildShape ◄─────────────────────┘
//
// 与 Python 版（generate_shape.py + export_vegetation_shape.py）的区别：
//   * 采样 / 父枝 / 叶卡姿态 / 拓扑序全在 C++ 里，Blender 只做「把曲线和叶卡网格倒出来」；
//   * 父枝查询从 O(枝数²) 的三重循环换成稀疏栅格最近邻（fir 那棵树 119s → 十几毫秒）。
//
// 用法：
//   shape_gen --config configs/pine.pbtxt --out output/veg_shapes/pine.vegshape
//             [--blender /usr/bin/blender] [--keep-dump]

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "google/protobuf/text_format.h"
#include "tools/jpov/tools/vegetation_tool_v2/gen/sapling_dump.h"
#include "tools/jpov/tools/vegetation_tool_v2/gen/shape_builder.h"
#include "tools/jpov/tools/vegetation_tool_v2/gen/shape_text.h"
#include "tools/jpov/tools/vegetation_tool_v2/proto/generate.pb.h"
#include "tools/jpov/tools/vegetation_tool_v2/shape.h"

namespace {

using jpov::vegetation::gen::BuildOptions;
using jpov::vegetation::gen::BuildShape;
using jpov::vegetation::gen::BuildStats;
using jpov::vegetation::gen::FormatShapeText;
using jpov::vegetation::gen::LoadSaplingDump;
using jpov::vegetation::gen::SaplingDump;

// dump 脚本路径：默认相对仓库根（在仓库根跑即可）；--dump-script 可覆盖。
const char kDumpScript[] = "tools/jpov/tools/vegetation_tool_v2/dump_sapling_curve.py";

// 单引号包裹，内部单引号按 shell 规则转义。
std::string ShellQuote(const std::string& text) {
  std::string quoted = "'";
  for (const char character : text) {
    if (character == '\'') {
      quoted += "'\\''";
    } else {
      quoted.push_back(character);
    }
  }
  quoted.push_back('\'');
  return quoted;
}

// 最短可往返的 float 文本（Sapling 的属性是 float32：打印成能被解析回同一个值的
// 最短形式，既干净又不丢精度）。
std::string FormatFloat(float value) {
  for (int precision = 6; precision <= 9; ++precision) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*g", precision, static_cast<double>(value));
    if (static_cast<float>(std::strtod(buffer, nullptr)) == value) {
      return buffer;
    }
  }
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%.9g", static_cast<double>(value));
  return buffer;
}

std::string FormatSingular(const google::protobuf::Message& message,
                           const google::protobuf::FieldDescriptor* field) {
  const google::protobuf::Reflection* reflection = message.GetReflection();
  switch (field->cpp_type()) {
    case google::protobuf::FieldDescriptor::CPPTYPE_BOOL:
      return reflection->GetBool(message, field) ? "true" : "false";
    case google::protobuf::FieldDescriptor::CPPTYPE_INT32:
      return std::to_string(reflection->GetInt32(message, field));
    case google::protobuf::FieldDescriptor::CPPTYPE_INT64:
      return std::to_string(reflection->GetInt64(message, field));
    case google::protobuf::FieldDescriptor::CPPTYPE_UINT32:
      return std::to_string(reflection->GetUInt32(message, field));
    case google::protobuf::FieldDescriptor::CPPTYPE_FLOAT:
      return FormatFloat(reflection->GetFloat(message, field));
    case google::protobuf::FieldDescriptor::CPPTYPE_DOUBLE:
      return FormatFloat(static_cast<float>(reflection->GetDouble(message, field)));
    case google::protobuf::FieldDescriptor::CPPTYPE_STRING:
      return reflection->GetString(message, field);
    case google::protobuf::FieldDescriptor::CPPTYPE_ENUM:
      return reflection->GetEnum(message, field)->name();
    case google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE:
      break;  // 本 schema 的 tree 里没有嵌套消息
  }
  return std::string();
}

std::string FormatRepeated(const google::protobuf::Message& message,
                           const google::protobuf::FieldDescriptor* field, int index) {
  const google::protobuf::Reflection* reflection = message.GetReflection();
  switch (field->cpp_type()) {
    case google::protobuf::FieldDescriptor::CPPTYPE_BOOL:
      return reflection->GetRepeatedBool(message, field, index) ? "true" : "false";
    case google::protobuf::FieldDescriptor::CPPTYPE_INT32:
      return std::to_string(reflection->GetRepeatedInt32(message, field, index));
    case google::protobuf::FieldDescriptor::CPPTYPE_INT64:
      return std::to_string(reflection->GetRepeatedInt64(message, field, index));
    case google::protobuf::FieldDescriptor::CPPTYPE_UINT32:
      return std::to_string(reflection->GetRepeatedUInt32(message, field, index));
    case google::protobuf::FieldDescriptor::CPPTYPE_FLOAT:
      return FormatFloat(reflection->GetRepeatedFloat(message, field, index));
    case google::protobuf::FieldDescriptor::CPPTYPE_DOUBLE:
      return FormatFloat(static_cast<float>(reflection->GetRepeatedDouble(message, field, index)));
    case google::protobuf::FieldDescriptor::CPPTYPE_STRING:
      return reflection->GetRepeatedString(message, field, index);
    case google::protobuf::FieldDescriptor::CPPTYPE_ENUM:
      return reflection->GetRepeatedEnum(message, field, index)->name();
    case google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE:
      break;
  }
  return std::string();
}

// SaplingTree 里「写了就传给 Blender」的字段 → `k=v` 列表（顺序 = proto 字段顺序）。
std::vector<std::string> BuildOverrides(const jpov::vegetation::SaplingTree& tree) {
  std::vector<std::string> overrides;
  const google::protobuf::Descriptor* descriptor = tree.GetDescriptor();
  const google::protobuf::Reflection* reflection = tree.GetReflection();
  for (int i = 0; i < descriptor->field_count(); ++i) {
    const google::protobuf::FieldDescriptor* field = descriptor->field(i);
    if (field->name() == "preset" || field->name() == "extra_set") {
      continue;
    }
    if (field->is_repeated()) {
      const int size = reflection->FieldSize(tree, field);
      if (size == 0) {
        continue;
      }
      std::string joined;
      for (int k = 0; k < size; ++k) {
        if (k > 0) {
          joined += ",";
        }
        joined += FormatRepeated(tree, field, k);
      }
      overrides.push_back(field->name() + "=" + joined);
      continue;
    }
    if (!reflection->HasField(tree, field)) {
      continue;  // 没写 = 用预设自带值（proto2 optional 的存在性就是干这个的）
    }
    overrides.push_back(field->name() + "=" + FormatSingular(tree, field));
  }
  // 逃生口排最后：同名 key 覆盖上面的强类型字段。
  for (int i = 0; i < tree.extra_set_size(); ++i) {
    overrides.push_back(tree.extra_set(i));
  }
  return overrides;
}

bool RunCommand(const std::string& command) {
  const int status = std::system(command.c_str());
  if (status != 0) {
    std::cerr << "命令失败（exit=" << status << "）：\n" << command << "\n";
    return false;
  }
  return true;
}

int Fail(const std::string& message) {
  std::cerr << "错误: " << message << "\n";
  return 1;
}

}  // namespace

// 生成配置 → 叶面弯曲系数。没写 = 0 = 平面卡（proto2 的字段存在性在这里不用查：
// 三个系数本来就是「0 = 不弯」的合法值，不是「没填」的哨兵）。
jpov::vegetation::LeafBend MakeLeafBend(const jpov::vegetation::ShapeExport& export_config) {
  jpov::vegetation::LeafBend bend;
  bend.along = export_config.leaf_bend_along();
  bend.across = export_config.leaf_bend_across();
  bend.crease = export_config.leaf_bend_crease();
  return bend;
}

// 生成配置 → 枝条噪声参数。整块没写 = 全关（与没有这一层时逐字节一致）。
jpov::vegetation::gen::NoiseOptions MakeNoiseOptions(
    const jpov::vegetation::VegetationGenerateConfig& config) {
  jpov::vegetation::gen::NoiseOptions noise;
  if (!config.has_noise()) {
    return noise;
  }
  const jpov::vegetation::ShapeNoise& source = config.noise();
  noise.size_jitter.assign(source.size_jitter().begin(), source.size_jitter().end());
  noise.bend_longitudinal.assign(source.bend_longitudinal().begin(),
                                 source.bend_longitudinal().end());
  noise.bend_lateral.assign(source.bend_lateral().begin(), source.bend_lateral().end());
  noise.seed = config.has_tree() ? config.tree().seed() : 0;
  return noise;
}

// 噪声幅度只接受 0..1：它们是「倍数波动」与「枝长比例」，量级写错（把厘米当比例）
// 会得到一棵扭成麻花的树，而且在很远的地方才看得出来。
bool ValidateNoise(const jpov::vegetation::ShapeNoise& noise, std::string* error) {
  const std::pair<const char*, const google::protobuf::RepeatedField<float>*> tables[] = {
    { "noise.size_jitter", &noise.size_jitter() },
    { "noise.bend_longitudinal", &noise.bend_longitudinal() },
    { "noise.bend_lateral", &noise.bend_lateral() },
  };
  for (const auto& [name, values] : tables) {
    for (int i = 0; i < values->size(); ++i) {
      const float value = values->Get(i);
      if (!(value >= 0.0f) || value > 1.0f) {
        *error = std::string(name) + "[" + std::to_string(i) + "] = " + std::to_string(value) +
                 "，必须在 [0, 1]（0 = 关；1 = 极端）";
        return false;
      }
    }
  }
  return true;
}

int main(int argc, char** argv) {
  std::string config_path;
  std::string out_path;
  std::string blender = "blender";
  std::string dump_path;
  std::string dump_script = kDumpScript;
  bool keep_dump = false;
  bool text_output = false;   // --text：写文本（默认按扩展名；资产标准是 .vegshape 二进制）
  std::vector<uint32_t> ring_counts;   // 空 = 用 BuildOptions 的默认（16/12/8）
  uint32_t per_seg_override = 0;       // 0 = 用配置里的 export.per_seg

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--config" && i + 1 < argc) {
      config_path = argv[++i];
    } else if (arg == "--out" && i + 1 < argc) {
      out_path = argv[++i];
    } else if (arg == "--blender" && i + 1 < argc) {
      blender = argv[++i];
    } else if (arg == "--dump" && i + 1 < argc) {
      dump_path = argv[++i];
    } else if (arg == "--dump-script" && i + 1 < argc) {
      dump_script = argv[++i];
    } else if (arg == "--keep-dump") {
      keep_dump = true;
    } else if (arg == "--text") {
      text_output = true;   // 人工查看/diff 用；正式的资产一律 .vegshape
    } else if (arg == "--per-seg" && i + 1 < argc) {
      per_seg_override = static_cast<uint32_t>(std::atoi(argv[++i]));
      if (per_seg_override == 0) {
        return Fail("--per-seg 必须 > 0");
      }
    } else if (arg == "--ring-count" && i + 1 < argc) {
      // 形如 "8,6,4"：下标 = 枝级，最后一项重复用到更深的级。
      const std::string spec = argv[++i];
      size_t begin = 0;
      while (begin <= spec.size()) {
        const size_t comma = spec.find(',', begin);
        const std::string piece =
            spec.substr(begin, comma == std::string::npos ? std::string::npos : comma - begin);
        if (piece.empty()) {
          return Fail("--ring-count 里不能有空项：" + spec);
        }
        const int value = std::atoi(piece.c_str());
        if (value < 3 || value > 64) {
          return Fail("--ring-count 的每一项必须在 [3, 64]：" + piece);
        }
        ring_counts.push_back(static_cast<uint32_t>(value));
        if (comma == std::string::npos) {
          break;
        }
        begin = comma + 1;
      }
    } else {
      return Fail("未知参数: " + arg +
                  "\n用法: shape_gen --config <config.pbtxt> --out <shape.vegshape> [--text] "
                  "[--blender <path>] [--dump <path>] [--dump-script <path>] [--keep-dump]");
    }
  }
  if (config_path.empty() || out_path.empty()) {
    return Fail("必须给 --config 和 --out");
  }

  // ---- 读配置 ----
  std::string config_text;
  {
    std::ifstream stream(config_path);
    if (!stream) {
      return Fail("打不开配置：" + config_path);
    }
    config_text.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
  }
  jpov::vegetation::VegetationGenerateConfig config;
  if (!google::protobuf::TextFormat::ParseFromString(config_text, &config)) {
    return Fail("配置解析失败（字段名/类型对不上 proto/generate.proto）：" + config_path);
  }
  if (!config.has_version() || config.version() != 1) {
    return Fail("配置 version 必须显式写成 1");
  }
  if (!config.has_tree() || config.tree().preset().empty()) {
    return Fail("配置缺少 tree.preset");
  }
  if (!config.has_export_()) {
    return Fail("配置缺少 export 块");
  }
  // 注意：proto 里这个字段就叫 `export`（C++ 关键字），protoc 生成的是 export_()/has_export_()。
  const jpov::vegetation::ShapeExport& export_config = config.export_();
  if (!export_config.has_per_seg() || export_config.per_seg() == 0) {
    return Fail("export.per_seg 必须是正整数");
  }
  if (!export_config.has_leaf_aspect() || export_config.leaf_aspect() <= 0.0) {
    return Fail("export.leaf_aspect 必须 > 0");
  }
  if (!jpov::vegetation::IsValidAnchorMode(export_config.anchor_mode())) {
    return Fail("export.anchor_mode 只能是 bottom_left/bottom_center/bottom_right/top_center");
  }
  if (export_config.leaf_type_desc().empty()) {
    return Fail("export.leaf_type_desc 必填");
  }
  // 弯曲系数是「卡的 height 倍数」，Validate 的上限也就是 1.0；写错量级（把厘米当比例）
  // 会在很长一段时间里表现为「叶子被弯成麻花」，所以在这里先挡一次。
  const jpov::vegetation::LeafBend bend = MakeLeafBend(export_config);
  if (!(std::fabs(bend.along) <= 1.0) || !(std::fabs(bend.across) <= 1.0) ||
      !(std::fabs(bend.crease) <= 1.0)) {
    return Fail("export.leaf_bend_* 必须在 [-1, 1]（单位是卡的 height）");
  }

  // 环顶点数：3 = 三角管（细枝够用）、16 = 正圆。超出范围一定是把"半径"或"环数"填反了。
  for (int i = 0; i < export_config.ring_count_size(); ++i) {
    const uint32_t ring = export_config.ring_count(i);
    if (ring < 3 || ring > 64) {
      return Fail("export.ring_count[" + std::to_string(i) + "] = " + std::to_string(ring) +
                  "，必须在 [3, 64]（一圈至少 3 个顶点；64 以上纯属浪费）");
    }
  }

  if (config.has_noise()) {
    std::string noise_error;
    if (!ValidateNoise(config.noise(), &noise_error)) {
      return Fail(noise_error);
    }
  }

  const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
  const std::vector<std::string> overrides = BuildOverrides(config.tree());
  std::cout << "[gen] " << config.name() << "（preset=" << config.tree().preset() << "，"
            << overrides.size() << " 个参数覆盖）\n";
  if (!config.description().empty()) {
    std::cout << "[gen] " << config.description() << "\n";
  }

  // ---- 让 Blender 倒数据 ----
  const std::string effective_dump = dump_path.empty() ? out_path + ".dump" : dump_path;
  std::string command = ShellQuote(blender) + " -b --python " + ShellQuote(dump_script) + " -- " +
                        ShellQuote(config.tree().preset()) + " --out " +
                        ShellQuote(effective_dump);
  for (const std::string& item : overrides) {
    command += " --set " + ShellQuote(item);
  }
  if (!RunCommand(command)) {
    return Fail("Blender 执行失败（--dump-script 指对了吗？当前：" + dump_script + "）");
  }

  // ---- dump → shape ----
  SaplingDump dump;
  std::string error;
  if (!LoadSaplingDump(effective_dump, &dump, &error)) {
    return Fail("解析 dump 失败：" + error);
  }
  BuildOptions options;
  options.per_seg = per_seg_override != 0 ? per_seg_override : export_config.per_seg();
  options.ring_counts.assign(export_config.ring_count().begin(),
                             export_config.ring_count().end());
  if (!ring_counts.empty()) {
    options.ring_counts = ring_counts;   // 命令行优先级最高（临时试参数用）
  }
  if (options.ring_counts.empty()) {
    options.ring_counts = { 16u, 12u, 8u };  // 与历史行为一致
  }
  options.leaf_aspect = export_config.leaf_aspect();
  options.anchor_mode = export_config.anchor_mode();
  options.leaf_type_desc = export_config.leaf_type_desc();
  options.species = config.tree().preset();
  options.leaf_bend = bend;
  options.noise = MakeNoiseOptions(config);

  jpov::vegetation::Shape shape;
  BuildStats stats;
  if (!BuildShape(dump, options, &shape, &stats, &error)) {
    return Fail("构建 shape 失败：" + error);
  }

  // 自检：拓扑序与唯一根（Validate 的两条硬约束，坏数据不落盘）。
  int roots = 0;
  std::vector<int> position_of(shape.branches.size(), -1);
  for (std::size_t i = 0; i < shape.branches.size(); ++i) {
    position_of[shape.branches[i].id] = static_cast<int>(i);
  }
  for (std::size_t i = 0; i < shape.branches.size(); ++i) {
    const jpov::vegetation::Branch& branch = shape.branches[i];
    if (branch.parent < 0) {
      ++roots;
    } else if (position_of[static_cast<std::size_t>(branch.parent)] >= static_cast<int>(i)) {
      return Fail("产物的拓扑序不合法：枝 " + std::to_string(branch.id) + " 的父枝排在它后面");
    }
  }
  if (roots != 1) {
    return Fail("产物必须恰好有一个根枝，实际 " + std::to_string(roots) + " 个");
  }

  // 资产标准格式 = `.vegshape`（二进制）；文本只用于人工查看/diff，--text 显式要才写。
  const bool write_binary =
      !text_output && jpov::vegetation::ShapeFileFormatFromPath(out_path) ==
                          jpov::vegetation::ShapeFileFormat::kBinary;
  if (write_binary) {
    std::string write_error;
    if (!jpov::vegetation::SaveShapeBinary(shape, out_path, &write_error)) {
      return Fail(write_error);
    }
  } else {
    const std::string text = FormatShapeText(shape);
    std::ofstream stream(out_path);
    if (!stream) {
      return Fail("写不了产物：" + out_path);
    }
    stream << text;
  }
  const std::uintmax_t out_bytes = [&out_path] {
    std::ifstream stream(out_path, std::ios::binary | std::ios::ate);
    return stream ? static_cast<std::uintmax_t>(stream.tellg()) : 0u;
  }();
  if (!keep_dump && dump_path.empty()) {
    std::remove(effective_dump.c_str());
  }

  const std::chrono::duration<double, std::milli> elapsed =
      std::chrono::steady_clock::now() - start;
  std::cout << "[gen] OK  " << stats.branches << " 枝 / " << stats.cards << " 卡 / "
            << (stats.mirrored_cards * 100 / std::max(1, stats.cards)) << "% 镜像 / "
            << out_bytes / 1000000.0 << " MB / " << elapsed.count() / 1000.0 << " s → "
            << out_path << "\n";
  return 0;
}
