// 植被 GLB 烘焙的输入/输出参数（baker 与关节构造单元共用）。
//
// 单独一个头文件的原因：关节构造（`vegetation_glb_joints.h`）需要读
// `GlbBakeOptions` 里的密度/软硬度字段，而 baker 又需要关节构造的结果。
// 把参数放在这里避免两者互相 include（循环依赖）。
//
// 字段语义与默认值的来历见 docs/vegetation_asset_format.md 与
// docs/jpov_vegetation_wind_design.md §2.2。

#ifndef JPOV_TOOLS_VEGETATION_TOOL_V2_BAKE_BAKE_OPTIONS_H_
#define JPOV_TOOLS_VEGETATION_TOOL_V2_BAKE_BAKE_OPTIONS_H_

#include <string>

namespace jpov {
namespace vegetation {

// 烘焙参数（面板上全部可调；默认值 = 柳树实测可用的一组）。
struct GlbBakeOptions {
  // ── 关节密度 ──
  // 粗枝（level <= thick_level_max）关节少而长（链短、残余误差不放大）；
  // 细而下垂的枝关节密（多环才弯得动，而不是整根硬棍）。
  double thick_spacing = 1.5;   // 米/关节（粗枝）
  double thin_spacing = 0.5;    // 米/关节（细枝）
  int thick_level_max = 1;      // level <= 这个值算"粗枝"
  int max_joints_per_branch = 8;  // 每枝至少 2 个关节（首尾），至多本值 + 1 个

  // ── 朝向继承度 soft（0 = 硬/全继承父朝向，1 = 软/只随自己的 θ）──
  // 判定「下垂细枝」：枝首比枝尾高 droop_min 米以上、且平均半径 < strand_radius_max，
  // 该枝 soft 随弧长从 0 渐变到 droop/droop_scale（v1 的判据，实测对柳条有效）。
  double droop_min = 0.15;
  double strand_radius_max = 0.01;
  double droop_scale = 0.5;

  // ── 相位场 ──
  // 逐顶点/逐关节的连续相位（平动湍流用）。phase_scale 缩放空间频率：
  // 1.0 = v1 波长（树 ~6m 时树冠去同步、单张卡内几乎不变）。
  double phase_scale = 1.0;

  // ── 材质 ──
  std::string bark_dir;      // 含 albedo.png/normal.png/roughness.png；空 = 纯色树干
  std::string leaf_png;      // 黑底叶卡图（type 0）；空 = 不烘叶子
  std::string leaf2_png;     // type 1（镜像手性）素材；空 = 回退 leaf_png
  double leaf_black = 30.0;  // 黑底阈值（软过档 [0.5t, 1.5t]）
  double leaf_normal = 1.0;  // 由 alpha+luma 生成的法线强度
  double normal_scale = 1.5;  // 树皮法线强度
  double uv_v_scale = 0.0;   // 0 = 按树皮 albedo 像素尺寸自动（宽/高）
};

// 一次烘焙的产出统计（面板显示 / 命令行日志）。
//
// 口径说明（别混）：`trunk_vertices` / `leaf_vertices` 是**顶点数**，
// `triangles` 是**整份 GLB 的三角形总数**（树干 + 所有叶卡 primitive）。
struct GlbBakeStats {
  int joints = 0;
  int trunk_vertices = 0;
  int leaf_vertices = 0;
  int triangles = 0;   // 全部 primitive 之和
  double milliseconds = 0.0;
  std::string path;
};

}  // namespace vegetation
}  // namespace jpov

#endif  // JPOV_TOOLS_VEGETATION_TOOL_V2_BAKE_BAKE_OPTIONS_H_
