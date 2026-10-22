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

#include "tools/jpov/tools/vegetation_tool_v2/bake/bake_options.h"
#include "tools/jpov/tools/vegetation_tool_v2/shape.h"

namespace jpov {
namespace vegetation {

// 烘焙并写出 GLB。失败返回 false，*error 填原因（读不到贴图 / 没有可烘的几何 / 写盘失败）。
bool BakeVegetationGlb(const Shape& shape, const std::string& out_path,
                       const GlbBakeOptions& opts, GlbBakeStats* stats,
                       std::string* error);

}  // namespace vegetation
}  // namespace jpov

#endif  // JPOV_TOOLS_VEGETATION_TOOL_V2_BAKE_VEGETATION_GLB_BAKER_H_
