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
//   * 截面 profile：**归一化**（半径的倍数）、逆时针、闭合且首尾不重复、星形

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

// 截面轮廓上的点（**归一化**，单位是「半径的倍数」，不是米）。
//
// 平面：过采样点、垂直于中心线切线 T；原点：该采样点的中心线位置。
// x 沿 U，y 沿 V，(U, V, T) 为右手系。
// 世界坐标 = C + radius · ( x·U + y·V )  ⇒ radius 管粗细，profile 管形状。
struct ProfilePoint {
  double x = 0.0;
  double y = 0.0;
};

// 枝条中心线上的采样点。
struct BranchSample {
  Vec3d position;       // 米
  double radius = 0.0;  // 米，> 0；同时是 profile 的缩放基准
  double tilt = 0.0;    // 弧度，profile 的 (U, V) 基绕切线自转

  // 该点的截面轮廓。**逐点可变**（塑形噪音靠它）。
  // 空 = 未指定 = 正圆（点数由 Branch::ring_count 决定）。
  std::vector<ProfilePoint> profile;
};

// 一根枝（主干或分叉之一）。
struct Branch {
  uint32_t id = 0;
  int32_t parent = -1;  // -1 = 根枝
  uint32_t level = 0;
  std::vector<BranchSample> samples;
  uint32_t ring_count = 0;   // 0 = 用 profile 点数（正圆时也由它决定）
  uint32_t seam_index = 0;   // UV 接缝落在环上第几个顶点（branch 级：沿枝必须稳定）

  // 实际环顶点数：显式 ring_count 优先；否则取第一个非空 profile 的点数。
  // 两者都没有时返回 0，由 Validate 判非法。
  uint32_t EffectiveRingCount() const;
};

// 一张叶卡：anchor + up + normal + height + width 唯一确定 quad。
struct LeafCard {
  Vec3d anchor;         // 贴枝点，语义由 LeafType::anchor_mode 决定
  Vec3d up;             // 长轴，从根指向梢（单位向量）
  Vec3d normal;         // 卡面法线（单位向量，与 up 正交）
  double height = 0.0;  // 沿 up 的长度（米）
  double width = 0.0;   // 沿 right(width = cross(up, normal)) 的宽度（米），必须 > 0
  uint32_t type = 0;    // 引用 LeafType::type
  uint32_t branch = 0;  // 溯源：所属枝 id（渲染不依赖）
  double s = 0.0;       // 溯源：枝上参数位置 [0,1]
};

// anchor_mode → 根部在图（格子）里的位置，格内归一化：u 向右、v 向上。
//   合法值（4 个，全是「根在哪个角/边」，用户造图时看一眼就懂）：
//     bottom_left / bottom_center / bottom_right  —— 叶、叶簇
//     top_center                                  —— 柔荑花序、垂花（根在上）
// 只有「根部位置」，没有「卡片往哪长」：卡片永远沿 up 从根到梢。
// 返回 false = 非法字符串。
bool AnchorModeToUv(const std::string& mode, double* u, double* v);

// 只判合法、不取值（Validate 用）。
inline bool IsValidAnchorMode(const std::string& mode) {
  double u = 0.0;
  double v = 0.0;
  return AnchorModeToUv(mode, &u, &v);
}

// 叶卡类型。
struct LeafType {
  uint32_t type = 0;
  std::string description;        // 自然语言，永不参与逻辑判断
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

  std::vector<Branch> branches;
  std::vector<LeafType> leaf_types;
  std::vector<LeafCard> cards;
};

// 生成 n 点的单位正圆轮廓（n >= 3）。从 +U 方向起、逆时针。
std::vector<ProfilePoint> UnitCircleProfile(uint32_t n);

// 该采样点截面的**真实周长**（已乘 radius）：tile 尺寸就用它。
// profile 为空时按 ring_count 点的正圆计算。
double SamplePerimeter(const BranchSample& sample, uint32_t ring_count);

// proto -> 运行时结构。不做校验（校验请显式调用 Validate）。
Shape FromProto(const ShapeProto& proto);

// 校验资产合法性。返回 false 时 *error 填具体原因（含定位信息）。
// 校验项见 docs/vegetation_asset_format.md §6；任何一条不过都算失败，不做兜底。
bool Validate(const Shape& shape, std::string* error);

}  // namespace vegetation
}  // namespace jpov

#endif  // JPOV_TOOLS_VEGETATION_TOOL_V2_SHAPE_H_
