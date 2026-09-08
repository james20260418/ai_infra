// JPOV gen3d/skeleton — 带骨骼 3D 模型生成（占位骨架）
//
// 本目录是 gen3d 顶层的带骨骼部分（对应 tools/jpov/gen3d_skeleton.sh），
// 与 gen3d/static/（静态 PBR 模型）并列。目标链路：
//   文本/基础模型 → Tripo rig（spec=mixamo，出 mixamo 兼容骨骼）→ 带骨骼 GLB（可带动画）
//
// 命名空间约定：gen3d 相关统一用 jpov::gen3d（与 gen3d/static 的 tripo_client 一致）。
//
// ★ 占位状态（2026-09-08）：本期 PR 只建目录/结构占位，不写真实生成逻辑。
//   设计见 docs/jpov_clothes_rig_design.md；gltf loader 尚未接 skins/animations（loader 不动）。
//   真实接口待实际落地时在此定义，勿在占位期臆造 API。

#ifndef JPOV_GEN3D_SKELETON_SKELETON_RIG_H_
#define JPOV_GEN3D_SKELETON_SKELETON_RIG_H_

namespace jpov {
namespace gen3d {

// TODO(2026-09-08 skeleton): 带骨骼生成配置/入口将在此定义
//   （建议对齐 gen3d/static 的供应商无关分层 + fixme 管线约束，spec 选 mixamo）。

}  // namespace gen3d
}  // namespace jpov

#endif  // JPOV_GEN3D_SKELETON_SKELETON_RIG_H_
