// 植物拓扑的 C++ 运行时结构（与 ShapeProto 一一对应，但不依赖 protobuf 运行时代码）。
//
// 设计意图：装配/渲染代码只 include 本文件，不直接碰 proto。proto 只作为**序列化与校验**
// 的边界，这样：
//   * 下游代码不传播 protobuf 依赖；
//   * proto 改字段时，改动集中在本文件的 FromProto 一处；
//   * 结构体可以带派生计算（周长、环细分等），proto 不能。
//
// 约定（**硬编码，不做成字段**——多一个字段就多一个能填错的地方）：
//   * 单位：**米**（m）；角度用弧度
//   * 上方向：**+Y**（glTF 约定）。Blender 是 +Z up，导出时转 (x,y,z)->(x,z,-y)
//   * profile：归一化单位多边形（半径 1），逆时针，闭合且首尾不重复

#ifndef JPOV_TOOLS_VEGETATION_TOOL_V2_SHAPE_H_
#define JPOV_TOOLS_VEGETATION_TOOL_V2_SHAPE_H_

#include <cstdint>
#include <string>
#include <vector>

#include "tools/jpov/tools/vegetation_tool_v2/proto/shape.pb.h"

namespace jpov {
namespace vegetation {

// 三维点，单位米。
struct Vec3d {
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
};

// 单位化截面上的点（半径 1 时的轮廓），实际尺寸由 BranchSample::radius 缩放。
struct ProfilePoint {
  double x = 0.0;
  double y = 0.0;
};

// 单位化截面轮廓：星形多边形，逆时针，闭合且首尾不重复。
struct CrossSection {
  std::vector<ProfilePoint> points;

  // UV 接缝所在的顶点下标（u = 0 从这里开始绕）。
  uint32_t seam_index = 0;

  // 单位化轮廓的周长（半径 1 时的周长）。UV 的 tile 尺寸 = radius × Perimeter()。
  // 多边形用真实边长累加，不是 2πr —— 低模环（8/12 边）的真实表面比理想圆短，
  // 用 2πr 会让纹理横向被多算约 14%。
  double Perimeter() const;
};

// 枝条中心线上的采样点。
struct BranchSample {
  Vec3d position;     // 米
  double radius = 0.0;  // 米，> 0
  double tilt = 0.0;    // 弧度，截面绕切线自转
};

// 一根枝（主干或分叉之一）。
struct Branch {
  uint32_t id = 0;
  int32_t parent = -1;  // -1 = 根枝
  uint32_t level = 0;
  std::vector<BranchSample> samples;
  uint32_t ring_count = 0;  // 0 = 用截面点数

  // 实际环细分：ring_count 为 0 时回落到截面点数。
  uint32_t EffectiveRingCount(const CrossSection& profile) const;
};

// 一张叶卡。宽度不在数据结构里——由叶材质 atlas 的格子长宽比推导。
struct LeafCard {
  Vec3d anchor;   // 贴枝点，语义由 LeafType::anchor_mode 决定
  Vec3d up;       // 长轴，从根指向梢（单位向量）
  Vec3d normal;   // 卡面法线（单位向量，与 up 正交）
  double height = 0.0;  // 沿 up 的长度（米）
  uint32_t type = 0;    // 引用 LeafType::type
  uint32_t branch = 0;  // 溯源：所属枝 id（渲染不依赖）
  double s = 0.0;       // 溯源：枝上参数位置 [0,1]
};

// 叶卡类型。anchor_mode 取 "bottom_center" / "top_center" / "center"。
struct LeafType {
  uint32_t type = 0;
  std::string description;   // 自然语言，永不参与逻辑判断
  std::string anchor_mode;
  double preferred_aspect = 0.0;  // width/height；0 = 未指定
};

// 一棵植物的完整拓扑。
struct Shape {
  // 格式版本。加字段不升版本；改语义/单位必须升。
  uint32_t version = 1;

  // 万用备注/提示词：这份 shape 的用途、哪类叶卡该配什么素材、哪些素材可互换。
  // 供人 / agent / 下游工具读取，**永不参与逻辑判断**，可为空。
  std::string description;

  CrossSection profile;
  std::vector<Branch> branches;
  std::vector<LeafType> leaf_types;
  std::vector<LeafCard> cards;
};

// proto -> 运行时结构。不做校验（校验请显式调用 Validate）。
Shape FromProto(const ShapeProto& proto);

// 校验资产合法性。返回 false 时 *error 填具体原因（含定位信息）。
// 校验项见 docs/vegetation_asset_format.md §6；任何一条不过都算失败，不做兜底。
bool Validate(const Shape& shape, std::string* error);

}  // namespace vegetation
}  // namespace jpov

#endif  // JPOV_TOOLS_VEGETATION_TOOL_V2_SHAPE_H_
