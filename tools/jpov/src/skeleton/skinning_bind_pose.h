// JPOV skeleton — 临时 bind pose（M1 scaffolding）
//
// ⚠️ 本文件是 **M1 临时桥**，只为了先跑通蒙皮渲染链路（T-pose 静态退化门），
// 不是长期正确方案。背景见 memory/2026-09-09：glb 里没存“bind pose 姿态”，只有
// inverseBindMatrices（bind 的逆）+ 每骨 node local 变换。要从 mesh 反解 bind pose，
// 风险是 mesh 顶点并未覆盖全部 23 骨（某些骨无顶点/权重=0 → 反解不完整=半个 pose）。
// 故 M1 用**临时硬编码**蓝人(mixamo_male.glb)的 bind pose 每骨 local 旋转。
//
// 本 pose 与 LoadGltfSkeleton 产出的 SkeletonType（同一 male glb）**同构**：
//   - 骨序 = skin.joints 顺序（23 个）：[0]Root,[1]Hips,[2]Spine,...,[22]RightToeBase。
//   - rest_offset（骨长/平移）由 LoadGltfSkeleton 从 node.translation 填，本文件**不重复**；
//     这里只给每骨的 **local 旋转（四元数 xyzw）** —— 放进 SkeletonPose.joint_rotation。
//
// 正确性：SkeletonManager(type,{bindPose}) 沿树 jointWorld(j)=T(rest_offset)·R(joint_rotation)，
// 合成的 jointWorld(bind) 恰为该 mesh 绑定时各骨全局变换；×骨架级 inverseBind = I →
// 蒙皮结果=原 rest 网格（M1 静态退化门成立）。
//
// 后续健全时：改为从 skin node local 干净提取（去掉本临时表），并以 FBX 动画覆盖单测。
// 合入前先核对本表与 mixamo_male.glb 实测一致（见本文件末尾数据源注释）。
#ifndef JPOV_SRC_SKELETON_SKINNING_BIND_POSE_H_
#define JPOV_SRC_SKELETON_SKINNING_BIND_POSE_H_

#include <vector>

#include "geom/common/quaternion.h"
#include "tools/jpov/interface/skeleton_types.h"

namespace jpov {

// 蓝人(mixamo_male.glb, skin.joints 序)的 bind pose 每骨 local 旋转（相对父）。
// 与 LoadGltfSkeleton 出的 SkeletonType 一一对应（同骨序/同树）。
// 数值 = 从该 glb 每关节 node 的 rotation(xyzw) 原样摘出。
inline std::vector<geom::Quaternion<float>> MixamoBindPoseRotations() {
    using Q = geom::Quaternion<float>;
    // 顺序与 skin.joints 一致：[0]Root [1]Hips [2]Spine [3]Spine1 ... [22]RightToeBase
    return {
        Q(-0.500000f, 0.500000f, 0.500000f, 0.500000f),  // 0 Root (轴修正 -90°X)
        Q(0.489979f, -0.489979f, -0.470985f, 0.545907f), // 1 Hips
        Q(-0.000000f, 0.679620f, 0.000000f, 0.733564f),  // 2 Spine
        Q(-0.000000f, 0.000000f, -0.000000f, 1.000000f), // 3 Spine1
        Q(0.000000f, -0.000000f, 0.000000f, 1.000000f),  // 4 Spine2
        Q(-0.000000f, 0.000000f, -0.000000f, 1.000000f), // 5 Neck
        Q(0.000000f, -0.000000f, 0.000000f, 1.000000f),  // 6 Head
        Q(-0.478577f, -0.482544f, -0.502944f, 0.534007f),// 7 LeftShoulder
        Q(0.148146f, 0.988147f, 0.039784f, 0.005965f),   // 8 LeftArm
        Q(0.040224f, -0.000558f, 0.013872f, 0.999094f),  // 9 LeftForeArm
        Q(0.060353f, -0.007234f, -0.118788f, 0.991057f), //10 LeftHand
        Q(0.534007f, -0.502944f, 0.482544f, 0.478577f),  //11 RightShoulder
        Q(-0.039590f, 0.001140f, -0.028769f, 0.998801f), //12 RightArm
        Q(0.039227f, 0.005471f, -0.138018f, 0.989637f),  //13 RightForeArm
        Q(-0.000000f, -0.000000f, 0.001938f, 0.999998f), //14 RightHand
        Q(0.679285f, -0.044445f, 0.732427f, 0.012103f),  //15 LeftUpLeg
        Q(-0.054591f, -0.000029f, -0.009133f, 0.998467f),//16 LeftLeg
        Q(0.658976f, -0.117960f, 0.088939f, 0.737513f),  //17 LeftFoot
        Q(0.000000f, 0.000005f, 0.000000f, 1.000000f),   //18 LeftToeBase
        Q(0.679282f, -0.044579f, 0.732420f, 0.012228f),  //19 RightUpLeg
        Q(-0.049698f, 0.000214f, -0.004301f, 0.998755f), //20 RightLeg
        Q(0.654999f, 0.112046f, -0.079536f, 0.743032f),  //21 RightFoot
        Q(-0.000000f, -0.000003f, 0.000000f, 1.000000f), //22 RightToeBase
    };
}

// 便捷：直接构造 bind pose（单 pose），bone_count=23。
inline SkeletonPose MakeMixamoBindPose() {
    SkeletonPose pose;
    pose.bone_count = 23;
    pose.joint_rotation = MixamoBindPoseRotations();
    // root_offset 留 0（bind T-pose 站原地；Hips 平移已在 rest_offset 里，见 SkeletonType）。
    return pose;
}

}  // namespace jpov

#endif  // JPOV_SRC_SKELETON_SKINNING_BIND_POSE_H_

// 数据源：mixamo_male.glb（assets/test/object3d/mixamo_male）。实测每关节 node rotation(xyzw)
// 即上表（2026-09-09 用 ufbx/校验脚本抓取）。骨架树父链含 Root(0)→Hips(1)→...，
// 与 LoadGltfSkeleton 一致。
