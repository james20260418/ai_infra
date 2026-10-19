// 植被工具：把「当前风形态」下的网格导出成 OBJ + CSV（供离线对账）
//
// 为什么需要它（2026-10-19）：
//   吹倒位移只在 VS 里算，肉眼只能看到"三角片"，没法判断到底是
//   ① 顶点聚类不一致（同一三角形跨两条枝链）还是 ② 位移场把细长条拉长。
//   这个导出器用 CPU 端**同一份公式**（interface/wind_deform.h）把变形后的顶点写出来，
//   并附上逐顶点 joint/L、逐三角形静息/变形面积，直接对账。
//
// 不依赖 GL：可以在没有窗口的环境跑。

#ifndef JPOV_DEMO_VEGETATION_MESH_EXPORT_H_
#define JPOV_DEMO_VEGETATION_MESH_EXPORT_H_

#include <string>

#include "tools/jpov/interface/wind_deform.h"

namespace jpov_vegetation {

// 读取 glb_path，按 params 做风变形，写出：
//   <prefix>.obj            变形后的几何（v / vn / o / f）
//   <prefix>_verts.csv      prim,vid,joint,L,x,y,z（变形后坐标，模型系）
//   <prefix>_tris.csv       prim,tri,ja,jb,jc,area_rest,area_deformed,ratio
// Pre-condition: glb_path 可读；prefix 所在目录可写。
// 返回 true 表示至少写出了一个 primitive。
bool SaveDeformedMesh(const std::string& glb_path,
                      const std::string& prefix,
                      const jpov::WindDeformParams& params);

}  // namespace jpov_vegetation

#endif  // JPOV_DEMO_VEGETATION_MESH_EXPORT_H_
