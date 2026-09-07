// JPOV skeleton — 骨架蒙皮子系统 GL 层：SkeletonManager（资源持有）声明
//
// src/skeleton/ 与 interface/skeleton_types.h 对应：CPU 侧类型(GL-free)在 interface，
// 真正碰 GL 的上传/持有在这里，仿照 src/object3d/（其 CPU 侧在 interface/render_command.h +
// interface/pbr_material.h，GPU 的 DrawObject3D 在 src/object3d/object3d_renderer.h）。
//
// SkeletonManager 职责（对照架构文档 docs/jpov_crowd_instancing_arch.md §6.2-B / §8 #2）：
//   - 接收 CPU 的 SkeletonTemplate（拓扑 + rest 平移 / 逆绑定），上传该骨架的**逆绑定矩阵**
//     constant（蒙皮用之；随骨架不变、不随实例姿态走，故单独存，不塞进逐 pose 骨骼纹理）。
//   - 接收单个 SkeletonPose（见 interface/skeleton_types.h；一个 pose = 某骨架的一份静态
//     位姿关键帧），把每个 pose 沿骨架树解算出的每关节 JointMatrix 平铺进该骨架的 RGBA
//     骨骼动画纹理（**pose atlas**：纹理每"行"=一个 pose、每"列"=某骨某根 4x4 的 4 个 texel；
//     摆放不打时间语义，见 skeleton_types.h 文件头）。一个骨架维护一个 pose 库。
//     运行期实例只送 {pose_a, pose_b, ratio}，蒙皮 VS 查该两 pose 的 mat4 并逐骨插值。
//   - 分配 skeleton_id / pose_id 句柄，供 RenderCommand 里的 DrawMeshWithSkeleton 引用。
//
// —— 骨架与 pose atlas 一一对应：SkeletonManager 可登记多份骨架模板（人与马各不相同），但
//   **每个骨架各持一张 pose atlas**（逆绑定/骨数/pose 都强绑那份骨架）。pose 只能在“同一
//   份骨架”的 atlas 内插值 —— 不同骨架绝不混插（跨骨架 = 要读两张 atlas，语义无意义，见
//   skeleton_types.h 文件头铁律）。这个“一个 manager 内多张骨架 atlas，各自独立”的所有权
//   是否应收敛成“一个 manager ≡ 一种骨架”待 Danis 在实现 PR 前裁定（见此前 review 的 point3）。
//
// ⚠️ 本文件当前为 **framework 声明阶段**：只给出可供 review 的接口与所有权模型，
// 具体的 GL 上传 / pose 烘焙 / instanced 蒙皮实现标 TODO，待实现 PR 落地（见各方法注释）。

#ifndef JPOV_SRC_SKELETON_SKELETON_MANAGER_H_
#define JPOV_SRC_SKELETON_SKELETON_MANAGER_H_

#include <cstdint>

#include "tools/jpov/interface/skeleton_types.h"

namespace jpov {

// ==================== 骨骼动画纹理（pose atlas）的生产者 ====================

// 骨骼动画纹理烘焙结果（GL 资源），归 SkeletonManager 持有：一张 RGBA 纹理把若干 pose 按
// (row: pose, col: bone 4x4) 平铺，存各 pose 各骨的 JointMatrix。(真正上传进显存在实现 PR)
//
// TODO(2026-09-06): 此 struct 目前是所有权模型的占位，实现时再决定字段(gl_tex、
// bone_count/pose_count、采样选项)放这里还是放 manager record 内。
struct SkeletonAnimTexture {
    // TODO(2026-09-06): GLuint gl_tex; int bone_count; int pose_count; ...
};

// ==================== SkeletonManager ====================

// 一个骨架（或骨架内多份 pose）的 GPU 资源持有者。RegisterSkeleton 登记一份 template、
// RegisterPose 给它挂一个位姿（平铺进该骨架的骨骼动画纹理），之后 renderer 用一个骨架复用
// 同一份 rest mesh / 逆绑定 / pose atlas 把一批实例 instanced draw 掉（每实例取两 pose 插值）。
class SkeletonManager {
public:
    SkeletonManager() = default;
    ~SkeletonManager() = default;  // framework 阶段无 GL 子资源；实现 PR 再改为真正释放。

    SkeletonManager(const SkeletonManager&) = delete;
    SkeletonManager& operator=(const SkeletonManager&) = delete;

    // RegisterSkeleton: 登记一份骨架模板，返回 skeleton_id（供 RenderCommand 引用）。
    //
    //   上传：按 tpl 拓扑算/存每关节逆绑定矩阵 constant（GL 资源在实现 PR，本方法当前
    //   只登记元数据并分配 id）。
    //
    // Pre-condition: tpl.Validate() 已通过。
    // TODO(2026-09-06): GL 上传逆绑定 constant；同 tpl 重复登记是否去重待定(参照
    // TextureManager::LoadFromFile 去重 vs Register 不查重，取后者=调用方保证不重复登记)。
    uint32_t RegisterSkeleton(const SkeletonTemplate& tpl);

    // RegisterPose: 给某骨架挂一个静态位姿 pose，平铺进该骨架的骨骼动画纹理，返回 pose_id。
    //
    //   将 pose（每关节旋转，用户资产层，见 skeleton_types.h SkeletonPose）沿该骨架树拓扑
    //   解算出相对根的 JointMatrix，烘焙进该骨架的 SkeletonAnimTexture 的下一空项。
    //   运行期实例在 <同一 skeleton_id> 下送 {pose_a, pose_b, ratio}，VS 查这两项逐骨插值。
    //
    // Pre-condition: skeleton_id 已 RegisterSkeleton；pose.bone_count == 该骨架 bone_count。
    // ⚠️ 一个 manager 内同骨架的 texture 容纳 pose 数有上限（=atlas 行数），S0 之上/交互式
    //   素材库预分配的安全与超限策略（crash vs 扩纹理）见 TODO —— 该主题待实现 PR 与 Danis
    //   裁定（预建一个骨架的 atlas 姿势容量 vs 运行期扩列）。
    // TODO(2026-09-06): pose 烘焙实现；pose 的 CPU 数据容器(SkeletonPose 目前只有 bone_count
    //   meta)见 interface/skeleton_types.h 里 SkeletonPose 注释(待实现 PR 连四元素/欧拉->mat4
    //   工具 + atlas 行数 capacity 一起落)。
    uint32_t RegisterPose(uint32_t skeleton_id, const SkeletonPose& pose);

    // ReleaseSkeleton / ReleasePose: 释放本 manager 持有的 GL 资源。
    // 不存在的 id → 静默忽略(允许重复释放)。
    // TODO(2026-09-06)
    void ReleaseSkeleton(uint32_t skeleton_id);
    void ReleasePose(uint32_t skeleton_id, uint32_t pose_id);

    // 让某骨架的可采样骨骼动画资源就绪(把 pose atlas bind 到某纹理单元, 供渲染该骨架的批次;
    // 实例在 VS 里查两个 pose 项逐骨插值)。
    // TODO(2026-09-06): 若某批次完全静态(所有实例 pose_a==pose_b==同一 pose 且用户要 rest 不
    //   查纹理)可退化为 "仅把逆绑定传好, 不 bind pose atlas" → 纯静态蒙皮(零动画)是 S0
    //   "rest 姿态退化为现有不带蒙皮 VS=零回归门" 的第一个里程碑(见 render_command.h
    //   DrawMeshWithSkeleton)。多 pose / instancing 批次绑定细节在实现 PR 定。

    // ---- 元数据查询(给 renderer / 测试) ----
    // TODO(2026-09-06): GetBoneCount(id), 查询逆绑定是否显式给过等, 实现时按需补。

private:
    uint32_t next_skeleton_id_ = 1;   // 0 = 无效 skeleton_id
    uint32_t next_pose_id_ = 1;       // 0 = 无效 pose_id
    // TODO(2026-09-06): 骨架/pose 的 GL 子资源(逆绑定 VBO/UBO、pose atlas 骨骼纹理)的记录容器
    //   (如 skeleton_id → SkeletonRecord) 在实现 PR 落；本 framework 只把句柄分配接口立好。
};

}  // namespace jpov

#endif  // JPOV_SRC_SKELETON_SKELETON_MANAGER_H_
