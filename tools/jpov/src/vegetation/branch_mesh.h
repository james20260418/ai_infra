// JPOV 枝干管网格（branch tube mesh）—— 把拓扑骨架「蒙皮」成管状枝干。
//
// 输入：L-System 生成器产出的 Skeleton（其 polylines 为连通枝链）。
// 输出：一个可一次 DrawObject3D 的管状网格（含 kPosition/kNormal/kUV）。
//
// 约定（见 docs/jpov_vegetation_design.md）：
//   · 每根枝链扫一串圆环（ring_edges 边），相邻环之间缝三角带；
//   · 半径逐点递减 → 管锥度；法线取径向（弯曲大体正确）；
//   · 用**平行传输标架**（rotation-minimizing）推进，避免管沿弯枝扭转；
//   · **分叉靠子枝插进母枝重叠盖住**，不做布尔/焊接，无端盖（尖端极细、孔看不见）；
//   · UV：u 绕环 0→1（首尾对齐，便于循环树皮）；v = 弧长 / uv_tile_length（避免沿枝拉伸）。
//
// 本文件 GL-free、确定性、可单测（只依赖 mesh 类型 + geom 向量）。

#ifndef JPOV_VEGETATION_BRANCH_MESH_H_
#define JPOV_VEGETATION_BRANCH_MESH_H_

#include "tools/jpov/interface/mesh.h"
#include "tools/jpov/src/vegetation/lsystem.h"

namespace jpov {
namespace vegetation {

// 枝干管网格参数（与树种无关的「怎么扫」）。
struct BranchMeshConfig {
    // 截面边数（>= 3）。8 = 性能/观感折中；12 更圆更重。
    int ring_edges = 8;

    // 树皮纹理沿枝方向的循环长度（米）：v = 弧长 / uv_tile_length。
    // 越大 → 同一段枝上重复的纹理越少（树皮格越大）。> 0。
    float uv_tile_length = 1.0f;
};

// 把骨架的枝链扫成管状网格。骨架无枝链（polylines 为空）时返回空网格。
//
// Pre-condition: cfg.ring_edges >= 3；cfg.uv_tile_length > 0
jpov::MeshData BuildBranchMesh(const Skeleton& skel, const BranchMeshConfig& cfg);

}  // namespace vegetation
}  // namespace jpov

#endif  // JPOV_VEGETATION_BRANCH_MESH_H_
