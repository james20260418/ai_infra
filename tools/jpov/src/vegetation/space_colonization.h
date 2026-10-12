// JPOV 空间殖民（Space Colonization, SC）—— 植被程序化生成的第二条主路。
//
// 与 L-System（`lsystem.h`）并列：L-System 是「语法/规则驱动」（自上而下写出来），
// SC 是「空间/竞争驱动」（自下而上长出来）。Runions 等 2007 的算法短得出奇，效果自然。
//
// ── 算法（就这四步循环，直到吸引点耗尽或节点封顶）──
//   1) 在树冠体积内撒一堆**吸引点**（attractor）——代表「待占领的空间/光」；
//   2) 每个吸引点认领**最近的节点**（距离 < influence_radius）为「母节点」；
//   3) 每个有吸引点的母节点，朝其吸引点的平均方向长出**一个新节点**（长度 = step）
//      （可叠加向性 tropism：重力下垂 / 向光上挺）；
//   4) 删掉落在任意节点 kill_radius 内的吸引点（"吃掉"）。
//   分叉是**涌现**的：某节点跨多轮仍是某些吸引点的最近节点时，就会长出第二条子枝。
//
// ── 与 L-System 的共同输出 ──
// 两者都只产出**拓扑骨架**（`Skeleton`：segments / polylines / leaf_sites），下游的
// 管网格（branch_mesh）与叶片卡片（leaf_cards）**完全复用、不区分来源**。
//
// 本文件 GL-free、确定性（同 seed 同参数必得同结果、跨平台一致）、可单测。
//
// 设计见 tools/jpov/docs/jpov_vegetation_design.md。

#ifndef JPOV_VEGETATION_SPACE_COLONIZATION_H_
#define JPOV_VEGETATION_SPACE_COLONIZATION_H_

#include <cstdint>

#include "geom/common/vec.h"
#include "tools/jpov/src/vegetation/lsystem.h"   // Skeleton / Polyline / LeafSite

namespace jpov {
namespace vegetation {

// 树冠吸引点云的**包络形状**——区分树型的最大杠杆（高窄锥 vs 矮宽丛）。
enum class CrownEnvelope {
    // 线性锥/台：半径从底 `crown_radius` 线性变到顶 `crown_top_radius`
    // （顶 = 0 即尖锥）。代表：高瘦尖塔形（杉、其它针叶）。
    kCone,
    // 椭球穹顶：半径 r ∝ √(1 − t²)，底最宽、顶收圆。代表：圆冠 / 伞盖（松、龙爪槐）。
    kDome,
    // 竖椭球：r ∝ √(1 − (2t−1)²)，**中部最胖**、上下收。代表：矮宽丛生（灌木）。
    kEllipsoid,
};

// 空间殖民生成参数 = 一棵树的「配方」（程序）。同参数 + 同 seed ⇒ 同一棵树。
struct SpaceColonizationConfig {
    // ── 吸引点云包络 ──
    CrownEnvelope envelope = CrownEnvelope::kCone;
    // 吸引点数量（越多树冠填得越满、越贵）。Pre-condition: > 0
    int   attractor_count = 2200;
    // 裸干高度（米）：此高度以下**不放**吸引点（= 树干下部不长枝）。
    // Pre-condition: >= 0
    float trunk_height = 0.2f;
    // 树冠高度（米）。Pre-condition: > 0
    float crown_height = 2.6f;
    // 树冠底部半径（米）。Pre-condition: >= 0
    float crown_radius = 0.5f;
    // 树冠顶部半径（米，仅 kCone 用；顶=0 → 尖锥）。Pre-condition: >= 0
    float crown_top_radius = 0.05f;

    // ── SC 核心参数 ──
    // 影响距离 d（米）：吸引点只认领这个距离内的最近节点。Pre-condition: > 0
    float influence_radius = 0.35f;
    // 吞噬距离 dk（米）：节点这个距离内的吸引点被删掉。Pre-condition: > 0，且 < influence_radius
    float kill_radius = 0.16f;
    // 每步生长长度 t（米）。Pre-condition: > 0
    float step = 0.10f;
    // 节点数上限（防止病态输入无限生长）。Pre-condition: >= 1
    int   max_nodes = 6000;

    // ── 向性（重力下垂 / 向光上挺）──
    // 生长方向 = normalize(吸引平均方向 + tropism_strength · normalize(tropism))。
    // 重力下垂：tropism = (0,-1,0)；向光上挺：tropism = (0,+1,0)。
    // tropism_strength == 0（默认）或 tropism 为零向量 → **关闭**（纯平均方向，零回归）。
    geom::Vec3<float> tropism = {0.0f, 0.0f, 0.0f};
    // 向性强度 e（建议 0.05 ~ 0.30）。
    float tropism_strength = 0.0f;

    // ── 粗细（管道模型 / 达芬奇定律）──
    // 半径 = min_radius + (base_radius − min_radius) · (w / w_root)^(1/radius_power)，
    // 其中 w = 该节点子树的末梢数（w_root = 全树末梢数）⇒ 越靠主干越粗。
    //   · radius_power = 2 即 r ∝ √w（面积守恒，达芬奇）；越大主干越细。
    // Pre-condition: base_radius > 0；min_radius > 0；base_radius >= min_radius；radius_power > 0
    float base_radius = 0.05f;
    float min_radius = 0.004f;
    float radius_power = 2.0f;

    // 是否在**末梢枝**（未分叉的外围细枝：从最后一个分叉点到枝尖的整条链）的每个节点
    // 放一个叶位。true = 外围细枝铺满叶（正常的叶密度）；false = 不放叶（纯枝干）。
    bool leaves_on_terminal_twigs = true;

    // 随机种子（决定吸引点云采样；确定性，同 seed 同结果）。
    uint64_t seed = 0;
};

// 跑一次空间殖民，产出拓扑骨架。
//
// 输出约定（与 L-System 的 `Skeleton` 完全一致，下游不区分来源）：
//   · segments —— 每个节点一段（start=母节点位置, end=本节点位置, radius=本节点半径）；
//   · polylines —— 从分叉点（含根）到下一分叉点/末梢的连通枝链，首点半径用
//     `attach_radius`（= 分叉点母枝半径）以便下游做领口缝合；根链 attach_radius = 0；
//   · leaf_sites —— 末梢枝（未分叉的外围细枝）的每个节点一个叶位（leaves_on_terminal_twigs 为真时）。
//
// Pre-condition: 见 SpaceColonizationConfig 各字段注释。
Skeleton Colonize(const SpaceColonizationConfig& cfg);

// ============================ 内置树种（4 种常见植物）============================
//
// 与 L-System 版（BuildFirTree 等）同名的**第二实现**，用 SC 生成，供对照验收。
// 各树种只改**包络形状/尺寸 + SC 参数 + 向性**这几个可解释的旋钮。

// 杉树：高窄尖锥（吸引点云 = 高瘦锥，微向光上挺）。
SpaceColonizationConfig FirConfig();
// 松树：下部裸干 + 上部圆冠（冠 = 穹顶，抬高裸干段）。
SpaceColonizationConfig PineConfig();
// 灌木：几乎无干 + 矮宽丛（吸引点云 = 中部最胖的竖椭球）。
SpaceColonizationConfig ShrubConfig();
// 龙爪槐：矮干 + 宽扁伞盖 + 枝下垂（强重力向性）。
SpaceColonizationConfig DragonClawConfig();

}  // namespace vegetation
}  // namespace jpov

#endif  // JPOV_VEGETATION_SPACE_COLONIZATION_H_
