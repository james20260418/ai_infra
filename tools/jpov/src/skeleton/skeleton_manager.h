// JPOV skeleton — 骨架蒙皮子系统 GL 层：SkeletonManager（GPU 资源对象）
//
// src/skeleton/ 与 interface/skeleton_types.h 对应：CPU 侧类型(GL-free)在 interface，
// 真正碰 GL 的上传/持有在这里，仿照 src/object3d/（其 CPU 侧在 interface/render_command.h +
// interface/pbr_material.h，GPU 的 DrawObject3D 在 src/object3d/object3d_renderer.h）。
//
// SkeletonManager 职责（对照架构文档 docs/jpov_crowd_instancing_arch.md §6.2-B / §8 #2，
// 布局/分工契约定稿见 docs/jpov_skeleton_manager_design.md）：
//   - 构造即绑定一种骨架（SkeletonType）+ 一整套 pose。GPU 资源只有**一张**骨骼动画纹理
//     （pose atlas），逆绑定不进 GPU 独立存储 —— 方案甲：CPU 在烘焙期沿骨架树把每 pose 的
//     jointWorld(相对根) × 骨架级 inverseBind 乘好成“最终肤矩阵”再落 atlas 行，蒙皮 VS
//     每骨一次点采样即得（业界 palette.skinningMatrix[j]=globalPose[j]×inverseBind[j]，
//     见 docs/jpov_skeleton_manager_design.md §3 方案甲）。
//     **骨骼动画纹理（pose atlas）** → RGBA32F 固定 2048×2048 2D 纹理：每个 pose 宽
//        bone_count×4 texel（每骨一个 4×4 矩阵=4 texel，23 骨=92），每行 floor(2048/(4*bone))
//        → 个 pose（23 骨=22 pose/行），**整 pose 不跨行**（行宽=pose 宽整数倍，行尾 padding
//        不用）；capacity≈floor(2048²/(4*bone))（23 骨≈45,590 pose，远超业务 ~1800）。
//        2D 平铺而非"一行一 pose 高窄条"，故高度不撞保守上限、海量 pose 无压力。
//   - 析构释放全部 GL。**无 Register/Release/自增 id** —— 资源在构造/析构锁死。
//   - 运行时实例只在 {pose_a, pose_b, ratio} 间插值（pose_a/b 引用 atlas 中某 pose 的行列，见
//     SkinnedInstanceState）：**per-instance 传 CPU 预算好的 (row,col) 行列 offset**（divmod
//     在 CPU 每 instance 一次算，VS 全程零除法），蒙皮 VS 查这两行 mat4、逐骨 lerp 后套 4-bone
//     蒙皮。VS 只读该顶点被影响的 ≤4 骨，每顶点最多 16 次 RGBA32F texelFetch。
//   - 蒙皮链（方案甲）= Σ weight·(Atlas 里 (pose,bone) 最终肤矩阵 × rest 顶点)。
//
// —— 所有权 / 可见性（v3，2026-09-07 与 Danis 收敛）——
//   - SkeletonManager 是 renderer/内部(GPU)私有的资源对象：**对 JPOV 用户不可见**。
//   - 用户经 JPOV 代理 RegisterSkeleton(SkeletonType, poses) 创建，拿到手的只有 skeleton id。
//   - **renderer 持 id→SkeletonManager× 映射 + id 分配**（freelist，见：
//     新加的骨架 id 管理用 LIFO 空槽复用 + 避回绕）。
//   - SkeletonManager **无自管 id**：它不产生对外的整数句柄，只是 renderer 在映射里持有它。
//
// —— 一个 SkeletonManager ≡ 一种骨架（一种 SkeletonType）—— 跨骨架绝不混插。
//   pose 与骨架强绑；不同骨架 = 不同骨数量/拓扑/骨骼纹理，跨骨架插值语义无意义且要读两张纹理，
//   故实例的 pose_a/pose_b 只能在同一种骨架（同一个 SkeletonManager）内。见 skeleton_types.h 铁律。
//
// ⚠️ 实现状态（2026-09-08 准备 PR）：头文件契约与 GpuHandles 已定稿；pose atlas 烘焙/上传
//   （方案甲折入 inverse_bind）在 skeleton_manager.cc 落地；instanced 蒙皮 draw 交给下个
//   PR（renderer 消费 kSkinnedMesh / 蒙皮 VS）。本文件只负责资源层的 GPU 持有 + 句柄出口。

#ifndef JPOV_SRC_SKELETON_SKELETON_MANAGER_H_
#define JPOV_SRC_SKELETON_SKELETON_MANAGER_H_

#include <vector>

#include "tools/jpov/interface/skeleton_types.h"

namespace jpov {

// ==================== SkeletonManager ====================

// 一种骨架（一个 SkeletonType + 一整包 pose）的 GPU 资源持有者。构造分配资源、析构释放，
// 生命周期锁死在对象本身；renderer 用同一份骨骼纹理/逆绑定把一批实例 instanced draw 掉
// （每实例取两 pose 行插值）。由于拥有 GL 资源，禁止拷贝/移动，仅可被 renderer 用
// unique_ptr 持有。
class SkeletonManager {
public:
    // Poses atlas 边长（布局契约，勿改）：固定 2048×2048 RGBA32F 2D。
    // 见 docs/jpov_skeleton_manager_design.md §2。放类内以免污染 namespace。
    static constexpr int kPoseAtlasDim = 2048;

    // renderer 批量蒙皮时要 bind 到 shader 的句柄集合：**本类不自己 load 自己**，
    // 只老实暴露底层 GL 资源句柄，由 renderer 在 DrawMeshWithSkeleton 里取用并 bind。
    // 方案甲：GPU 只有**一张** pose atlas 纹理（inverse_bind 已被 CPU 在烘焙期折入每 pose
    // 行的“最终肤矩阵”，无独立逆绑定 GPU 资源 —— 工程 GL 层无 SSBO/UBO range 可用）。
    //   pose_atlas_tex : RGBA32F 2048×2048 atlas。atlas 每 (pose 行, bone 列 4×4) 存的是最终
    //                    肤矩阵 jointWorld(pose,bone)×inverseBind(bone)（CPU 已折入），
    //                    蒙皮 VS 每骨一次点采样即得，直接 Σ weight·M·v。
    //   bone_count  : 该骨架骨数（= type.bone_count = 每 pose 宽 /4 = 一行 4×4 矩阵个数）。
    //   pose_per_row: 一行容纳的 pose 数 = floor(2048 / (4*bone_count))。CPU 端用它把
    //                 pose_idx divmod 成 (row, col) 行列 offset 作 per-instance 传入。
    //   由 renderer 在批量 draw 时从 SkeletonManager 取 GpuHandles 并 bind；本结构体本身
    //   GL-free（只存 GLuint），不提供资源生命周期（生命周期归 SkeletonManager）。
    struct GpuHandles {
        unsigned int pose_atlas_tex = 0;  // RGBA32F 2D pose atlas（含折入 inverse_bind 的最终肤矩阵）
        int bone_count = 0;    // 该骨架骨数
        int pose_per_row = 0;  // 每行 pose 数 = floor(2048/(4*bone_count))
    };

    // 构造：绑定一种骨架的【定义 + 全套 pose】，烘焙并上传骨骼动画纹理（pose atlas）。
    //   type : SkeletonType（内部即调 Validate()，joints 拓扑序合法、inverse_bind 尺寸对齐）。
    //   poses: 该骨架的全部静态位姿关键帧。构造即把每个 pose 沿骨架树解算成每骨 jointWorld
    //          （相对角色根）× 骨架级 inverseBind → 折成“最终肤矩阵”烘焙上传（atlas 每 pose
    //          占一行内 bone_count×4 texel，行排按 pose_per_row 摊入 2048×2048）。
    //   Pre-condition: GL context 已激活；type.Validate() 通过。
    //   ⚠️ 每个 pose 的 bone_count 应与 type.bone_count 一致（同一种骨架）。poses 总容量
    //      不得超过 pose_capacity()（超→LOG(FATAL)，不 fallback）。
    SkeletonManager(const SkeletonType& type, std::vector<SkeletonPose> poses);

    // 析构：释放本 manager 持有的 GL 资源（骨骼动画纹理）。
    ~SkeletonManager();

    SkeletonManager(const SkeletonManager&) = delete;
    SkeletonManager& operator=(const SkeletonManager&) = delete;

    // ---- 只读元数据（renderer / 测试用）----
    // 该 atlas 一行能容纳几个 pose（= floor(kPoseAtlasDim/(4*bone_count))），CPU 端据此
    //   把 pose 下标 divmod 成 (row,col) 行列 offset。
    int pose_per_row() const { return pose_per_row_; }
    // 本骨架的骨数量。
    int bone_count() const { return bone_count_; }
    // 构造时传入的 pose 总数（atlas 实际容纳的 pose 行引用范围 [0, pose_count_)）。
    int pose_count() const { return pose_count_; }
    // 该 atlas 最多能容纳的 pose 数（= floor(kPoseAtlasDim²/(4*bone_count))）。
    //   Pre-condition: bone_count 已由 ctor 定（>0）。超出即 LOG(FATAL)。
    int pose_capacity() const { return capacity_; }

    // ---- renderer 内部 GL 句柄入口（不让本类"自己 load 自己"，改由 renderer 来 bind）----
    // 这里**老实暴露**底层 GPU 资源的句柄，renderer 据此把骨骼动画纹理/逆绑定 bind 到蒙皮
    // 批次。本方法只提供 renderer 取句柄的入口，不对外暴露 id / 不做面向用户的 load。
    GpuHandles gpu_handles() const { return handles_; }

private:
    int bone_count_ = 0;
    int pose_per_row_ = 0;
    int capacity_ = 0;
    int pose_count_ = 0;
    GpuHandles handles_;   // 本 manager 实际持有的 GL 资源句柄（构造填、析构释放）
};

}  // namespace jpov

#endif  // JPOV_SRC_SKELETON_SKELETON_MANAGER_H_
