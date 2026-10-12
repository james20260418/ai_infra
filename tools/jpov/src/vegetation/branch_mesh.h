// JPOV 枝干管网格（branch tube mesh）—— 把拓扑骨架「蒙皮」成管状枝干。
//
// 输入：L-System 生成器产出的 Skeleton（其 polylines 为连通枝链）。
// 输出：一个可一次 DrawObject3D 的管状网格（含 kPosition/kNormal/kUV）。
//
// 约定（见 docs/jpov_vegetation_design.md）：
//   · 每根枝链扫一串圆环（ring_edges 边），相邻环之间缝三角带；
//   · 半径逐点递减 → 管锥度；法线取径向（弯曲大体正确）；
//   · 用**平行传输标架**（rotation-minimizing）推进，避免管沿弯枝扭转；
//   · **分叉靠子枝插进母枝重叠盖住**，不布尔/焊接；子枝起点做**领口缝合**（branch collar）：
//     半径从母枝半径平滑收细到子枝半径（见 junction_blend），避免分叉处粗细突变。
//   · UV：u、v 均按物理弧长 / bark_tile_meters（纹理循环）——树皮不随枝长/半径拉伸；
//     u **沿枝**（贴图 X＝纹理“长向”顺枝，故条带纵向）、v **绕环**（随半径变、绕一圈）。
//     注意接缝处可能不连续（周向非整数循环），MVP 接受。
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

    // 树皮纹理覆盖的**物理尺寸**（米）：u 沿枝 = 枝弧长 / u 瓦长；v 绕环 = 周向弧长 / v 瓦长。
    // 两个值必须按贴图长宽比给（否则纹理会拉伸）：
    //   贴图 2:1 且面积 S m² ⇒ u=√(2S)、v=√(S/2)。例：S=0.5 m²、2:1 → u=1.0、v=0.5。
    // Pre-condition: 两者均 > 0
    float bark_tile_u_meters = 1.0f;
    float bark_tile_v_meters = 0.5f;

    // 是否封盖枝链两端（三角扇）。枝尖封口才不露"平切"空洞；分叉处端盖埋在母枝内无害。
    bool cap_ends = true;

    // 分叉领口（branch collar）缝合长度系数：（>0 生效）单位 母枝半径。
    // 子枝起点半径从**母枝半径**平滑收细到子枝自身半径，过渡长度 = 系数 × 母枝半径；
    // 使子管从母管上"长出"而不是硬切换（0 = 关闭，零回归）。
    float junction_blend = 4.0f;
};

// 把骨架的枝链扫成管状网格。骨架无枝链（polylines 为空）时返回空网格。
//
// Pre-condition: cfg.ring_edges >= 3；cfg.bark_tile_{u,v}_meters > 0
jpov::MeshData BuildBranchMesh(const Skeleton& skel, const BranchMeshConfig& cfg);

}  // namespace vegetation
}  // namespace jpov

#endif  // JPOV_VEGETATION_BRANCH_MESH_H_
