// 植被 GLB 烘焙器：shape (+ bark 材质 + 叶卡素材) → 自洽 .glb
//
// 产物一份 GLB，内含：
//   * 树干 primitive（扫管网格 + UV/切线）+ 叶卡 primitive（quad 集合）
//   * 逐顶点 `(joint_id, phase)` → glTF `TEXCOORD_1`（= MeshData::wind_link）
//   * 每 mesh 一份关节表 → node extras `jpov_wind_joints`（每关节 7 float：
//     pivot.xyz / parent / level / phase / soft）
//   * 材质贴图**内嵌**（树皮 albedo+normal+ORM、叶卡 albedo+normal），单文件自洽
//
// 与 v1（Blender `wind_bake.py`）的差别：这里**不需要** Voronoi 标签推断 —— 我们已经
// 知道每个顶点属于哪根枝的哪个采样点（网格就是自己扫出来的），所以关节归属是精确的：
//   * 树干：第 i 环 → 覆盖该采样点的关节（按采样下标 floor）
//   * 叶卡：整张卡 → 卡片 anchor 所在枝上最近的保留采样点（4 个顶点共用同一 joint/phase
//     ⇒ 卡片保持刚性，不会被"软度梯度"剪开，也不会脱离挂点）
// v1 那套「测地 Voronoi + 跨枝 tie_eps」的问题（分叉处跨链三角形 15.6%、撕成薄片）在
// 这里从根上不存在。
//
// 关节表的语义与 docs/jpov_vegetation_wind_design.md §2.2 一致，引擎 `MeshManager`
// 载入后直接上传成 RGBA32F 关节纹理，object3d 路径 `DrawObject3D/DrawGltfObject` 加
// `WindCommand` 即生效。

#ifndef JPOV_TOOLS_VEGETATION_TOOL_V2_BAKE_VEGETATION_GLB_BAKER_H_
#define JPOV_TOOLS_VEGETATION_TOOL_V2_BAKE_VEGETATION_GLB_BAKER_H_

#include <string>

#include "tools/jpov/tools/vegetation_tool_v2/shape.h"

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
  int max_joints_per_branch = 8;

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

struct GlbBakeStats {
  int joints = 0;
  int trunk_vertices = 0;
  int leaf_vertices = 0;
  int triangles = 0;
  double milliseconds = 0.0;
  std::string path;
};

// 烘焙并写出 GLB。失败返回 false，*error 填原因（读不到贴图 / 没有可烘的几何 / 写盘失败）。
bool BakeVegetationGlb(const Shape& shape, const std::string& out_path,
                       const GlbBakeOptions& opts, GlbBakeStats* stats,
                       std::string* error);

}  // namespace vegetation
}  // namespace jpov

#endif  // JPOV_TOOLS_VEGETATION_TOOL_V2_BAKE_VEGETATION_GLB_BAKER_H_
