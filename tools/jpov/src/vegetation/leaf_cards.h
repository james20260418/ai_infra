// JPOV 叶片卡片网格（leaf card mesh）—— 把叶位（LeafSite）摆成带透明切孔的叶片。
//
// 输入：骨架的 leaf_sites（叶标记 `l` 处记录的着生点 + 朝向）。
// 输出：一个可一次 DrawObject3D 的叶片网格（含 kPosition/kNormal/kUV）。
//
// 约定（见 docs/jpov_vegetation_design.md）：
//   · 每片叶 = **十字卡片（X-card，两片正交 quad）**——各视角都看得见，避免"纸片"感；
//   · 卡片几何**恒定**（单位卡），大小差异由 **per-叶 scale**（基准叶长 × 生长 × 随机抖动）体现；
//   · 朝向：卡片局部 +Y 对齐叶轴、+Z 对齐叶面法线；每片再绕叶轴**随机 roll** 出自然感；
//   · UV 把叶片贴图（白底抠孔后）铺满卡片；**v=0 采样贴图顶部（＝叶柄“根部”）对准着生点，
//     v=1 采样贴图底部（＝叶尖）在最远端**；切孔走材质 alpha-test（本文件只管几何/UV）。
//
// 本文件 GL-free、确定性、可单测（只依赖 mesh 类型 + geom 向量）。

#ifndef JPOV_VEGETATION_LEAF_CARDS_H_
#define JPOV_VEGETATION_LEAF_CARDS_H_

#include <cstdint>

#include "tools/jpov/interface/mesh.h"
#include "tools/jpov/src/vegetation/lsystem.h"

namespace jpov {
namespace vegetation {

// 叶片卡片参数。
struct LeafCardConfig {
    // 基准叶长（米）：卡片沿叶轴方向的世界长度（生长/抖动前）。
    float leaf_length = 0.15f;

    // 卡片宽/长比（宽 = leaf_length * aspect）。按叶片贴图裁剪后的长宽比给。
    float aspect = 0.6f;

    // 每片叶的随机大小抖动幅度 [0,1)：实际缩放 ∈ [1-jitter, 1+jitter]。
    float size_jitter = 0.25f;

    // 生长因子 [0,1]：整体缩放叶大小（0=刚抽芽极小，1=成熟）。确定性，可逐帧变（生长验收）。
    float growth = 1.0f;

    // 绕叶轴的随机 roll 幅度 [0,1]：0=不随机，1=整圈随机（自然摆动感）。
    float roll_jitter = 1.0f;

    // 每个叶位发几片叶（叶丛密度）。≥ 1；越大越密。
    int leaves_per_site = 3;

    // 随机种子（确定性语法下同 seed 同结果）。
    uint64_t seed = 0;
};

// 由叶位生成叶片卡片网格。骨架无叶位时返回空网格。
//
// Pre-condition: cfg.leaf_length > 0；cfg.aspect > 0；cfg.growth >= 0；cfg.leaves_per_site >= 1
jpov::MeshData BuildLeafCardsMesh(const Skeleton& skel, const LeafCardConfig& cfg);

}  // namespace vegetation
}  // namespace jpov

#endif  // JPOV_VEGETATION_LEAF_CARDS_H_
