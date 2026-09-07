// JPOV skeleton — 骨架蒙皮子系统 GL 层：SkeletonManager（GPU 资源对象）声明
//
// src/skeleton/ 与 interface/skeleton_types.h 对应：CPU 侧类型(GL-free)在 interface，
// 真正碰 GL 的上传/持有在这里，仿照 src/object3d/（其 CPU 侧在 interface/render_command.h +
// interface/pbr_material.h，GPU 的 DrawObject3D 在 src/object3d/object3d_renderer.h）。
//
// SkeletonManager 职责（对照架构文档 docs/jpov_crowd_instancing_arch.md §6.2-B / §8 #2）：
//   - 构造即绑定一种骨架（SkeletonType）+ 一整套 pose：由 SkeletonType 算/存每关节逆绑定矩阵
//     constant；把每个 pose 解算成 JointMatrix 平铺成一张 RGBA 骨骼动画纹理（**pose atlas**，
//     纹理每"行"=一个 pose、每"列"=某骨某根 4x4 的 4 个 texel；行数=构造时传入 pose 数，摆放
//     不打时间语义，见 skeleton_types.h 文件头）。
//   - 析构释放全部 GL。**无 Register/Release/自增 id** —— 资源在构造/析构锁死。
//   - 运行时实例只在 {pose_a, pose_b, ratio} 间插值（pose_a/b = atlas 行 = 构造时 pose 数组下标，
//     见 SkinnedInstanceState）：蒙皮 VS 查这两行 mat4、逐骨 lerp 后套 4-bone 蒙皮。
//
// —— 所有权 / 可见性（v3，2026-09-07 与 Danis 收敛）——
//   - SkeletonManager 是 renderer/内部(GPU)私有的资源对象：**对 JPOV 用户不可见**。
//   - 用户经 JPOV 代理 RegisterSkeleton(SkeletonType, poses) 创建，拿到手的只有 skeleton id。
//   - **renderer 持 id→SkeletonManager× 映射 + id 分配**（freelist，见：
//     新加的骨架 id 管理用 LIFO 空槽复用 + 避回绕；旧 TextureManager/MeshManager 的同款回绕
//     bug 不进本 PR，下个 PR 单独修，本段为正确范例）。
//   - SkeletonManager **无自管 id**：它不产生对外的整数句柄，只是 renderer 在映射里持有它。
//
// —— 一个 SkeletonManager ≡ 一种骨架（一种 SkeletonType）—— 跨骨架绝不混插。
//   pose 与骨架强绑；不同骨架 = 不同骨数量/拓扑/骨骼纹理，跨骨架插值语义无意义且要读两张纹理，
//   故实例的 pose_a/pose_b 只能在同一种骨架（同一个 SkeletonManager）内。见 skeleton_types.h 铁律。
//
// ⚠️ 本文件当前为 **framework 声明阶段**：只给出可供 review 的接口与所有权模型，
// 具体的 GL 上传 / pose 烘焙 / instanced 蒙皮实现标 TODO，待实现 PR 落地（见各方法注释）。

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
    // 构造：绑定一种骨架的【定义 + 全套 pose】，上传逆绑定 constant + 烘焙骨骼动画纹理。
    //   type : SkeletonType 已 Validate()（joints 拓扑序合法、inverse_bind 尺寸对齐）。
    //   poses: 该骨架的全部静态位姿关键帧。构造即把骨骼动画纹理分配为（行数 = poses 数）。
    //   Pre-condition: GL context 已激活；type.Validate() 已通过。
    //   ⚠️ 每个 pose 的 bone_count 应与 type.bone_count 一致（同一种骨架）。
    // TODO(2026-09-06): GL 上传逆绑定 constant + poses 逐行烘焙成骨骼动画纹理；
    //   atlas 行数 = poses.size()。这里构造即分配纹理容量（呼应 point2 "预分配安全"）。
    SkeletonManager(const SkeletonType& type, std::vector<SkeletonPose> poses);

    // 析构：释放本 manager 持有的全部 GL 资源（逆绑定 constant、骨骼动画纹理）。
    ~SkeletonManager();

    SkeletonManager(const SkeletonManager&) = delete;
    SkeletonManager& operator=(const SkeletonManager&) = delete;

    // ---- 只读元数据（renderer / 测试用）----
    // TODO(2026-09-06): pose 数行 = atlas 行数、bone_count 等查询按需补。

    // ---- renderer 内部 GL 句柄入口（不让本类"自己 load 自己"，改由 renderer 来 bind）----
    // 这里**老实暴露**底层 GPU 资源的句柄，renderer 据此把骨骼动画纹理/逆绑定 bind 到蒙皮
    // 批次。字段与上传细节在实现 PR 定（见下方 TODO）；本方法仅提供 renderer 取句柄的入口，
    // 不对外暴露 id / 不做面向用户的 load。
    // TODO(2026-09-06): struct GpuHandles { GLuint bone_texture /*或骨骼type矩阵相关 index*/; };
    //   GpuHandles gpu_handles() const;   —— 由 renderer 在 DrawMeshWithSkeleton 批量蒙皮时取用。

private:
    // TODO(2026-09-06): SkeletonType 逆绑定 constant 的 GPU 资源 + pose atlas 骨骼动画纹理
    //   的实际句柄成员在此（GLuint 等）。框架阶段无 GL 字段，构造/析构为空实现即可先编过。
};

}  // namespace jpov

#endif  // JPOV_SRC_SKELETON_SKELETON_MANAGER_H_
