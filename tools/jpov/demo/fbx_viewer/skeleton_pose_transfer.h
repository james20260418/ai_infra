// JPOV FBX 观察器 — 源骨架 pose 按骨名【原样搬】到目标骨架（**刻意不做重定向**）
//
// ⚠️ 这不是 retarget：本函数把源骨架每帧的 joint_rotation **数值原样**写进目标骨架的同名骨，
//    不做任何 rest / 局部帧朝向的共轭校正。它的用途是当**对照组**，验证：
//    「两个骨架的局部帧差很大时，直接把 lcl rotation 数值搬过去，四肢会绕错轴」。
//    实测（Tripo glb 23 骨 vs Mixamo fbx 65 骨）：局部帧差最大 179.6°（LeftArm）/ 176.9°
//    （LeftShoulder）；这正是设计文档 §0.1 记的 M1「姿态全乱」根因。
//    正式重定向（按世界朝向共轭，M4）是**另一个函数**——勿把本函数当正解使用。
//
// 语义（全部刻意"直搬"）：
//   - 目标骨架每关节：按 name 在源骨架里找同名关节 → 命中则
//       dst.joint_rotation[i] = src.joint_rotation[src_idx]（数值原样，不转轴、不共轭）；
//   - 未命中的目标关节 = identity（该骨保持自身 rest，不动）；
//   - 命中但源 pose 为空（= 源走 rest，没旋转可搬）→ 也是 identity；
//   - stats.mapped 统计的是**骨名命中数**（与源 pose 有无内容无关，供面板显示"对得上多少骨"）；
//   - 源里多出的骨（如手指）原样忽略；
//   - root_offset **不搬**（输出恒 0）：本函数是**刻意不做重定向的对照组**，没有两侧几何
//     基准（没有 Q_body、没有两侧腿长比）⇒ 无从换算。注：火柴人渲染现已会用 root_offset
//     （见 interface/skeleton_mesh.h），故本函数置 0 是"不搬"，不是"下游用不上"。
//
// 职责边界：纯 CPU / GL-free / 不碰渲染（同 animation_sampler.h、skeleton_mesh.h）。

#ifndef JPOV_DEMO_FBX_VIEWER_SKELETON_POSE_TRANSFER_H_
#define JPOV_DEMO_FBX_VIEWER_SKELETON_POSE_TRANSFER_H_

#include <string>
#include <unordered_map>

#include <glog/logging.h>

#include "tools/jpov/interface/skeleton_types.h"

namespace jpov_fbx_viewer {

// 一次搬运的统计（供日志 / 面板显示"命中多少骨"）。
struct PoseTransferStats {
    int target_bones = 0;   // 目标骨架骨数
    int mapped = 0;         // 骨名命中的骨数（未命中者保持 identity；与源 pose 内容无关）
};

// 按骨名把源 pose 原样搬到目标骨架（不做重定向，见文件头）。
//
//   src_type : 源骨架定义（提供骨名）。
//   src_pose : 源位姿。joint_rotation 为空 = 视为全 identity（走 rest）；
//              非空时尺寸必须 == src_type.bone_count()。
//   dst_type : 目标骨架定义（骨名用于匹配）。
//   dst_pose : 输出位姿（非空）。bone_count / joint_rotation 尺寸 = dst_type 骨数，
//              root_offset 恒 0（见文件头）。
//   返回     : {目标骨数, 命中骨数}。
//
// Pre-condition: dst_pose != nullptr；src_pose 非空时尺寸与 src_type 一致（违反 → LOG(FATAL)）。
inline PoseTransferStats TransferPoseByNameNoRetarget(
    const jpov::SkeletonType& src_type, const jpov::SkeletonPose& src_pose,
    const jpov::SkeletonType& dst_type, jpov::SkeletonPose* dst_pose /*output*/) {
    CHECK(dst_pose != nullptr) << "TransferPoseByNameNoRetarget: dst_pose 不能为空";
    const bool src_has_rot = !src_pose.joint_rotation.empty();
    if (src_has_rot) {
        CHECK_EQ(src_pose.joint_rotation.size(), src_type.joints.size())
            << "TransferPoseByNameNoRetarget: src_pose 尺寸 " << src_pose.joint_rotation.size()
            << " 应 == 源骨架骨数 " << src_type.joints.size();
    }

    // 源：骨名 → 关节下标。
    std::unordered_map<std::string, int> src_by_name;
    src_by_name.reserve(src_type.joints.size());
    for (size_t i = 0; i < src_type.joints.size(); ++i) {
        const std::string& n = src_type.joints[i].name;
        if (!n.empty()) {
            src_by_name.emplace(n, static_cast<int>(i));  // 同名重复时取先出现者
        }
    }

    PoseTransferStats stats;
    stats.target_bones = dst_type.bone_count();
    dst_pose->bone_count = stats.target_bones;
    dst_pose->joint_rotation.assign(static_cast<size_t>(stats.target_bones),
                                    geom::Quaternion<float>::Identity());
    dst_pose->root_offset = jpov::Vec3f(0.0f, 0.0f, 0.0f);

    for (size_t i = 0; i < dst_type.joints.size(); ++i) {
        const std::string& name = dst_type.joints[i].name;
        if (name.empty()) {
            continue;  // 无名骨无法匹配 → 保持 identity
        }
        const auto it = src_by_name.find(name);
        if (it == src_by_name.end()) {
            continue;  // 源里没有同名骨 → 保持 identity
        }
        ++stats.mapped;  // 命中 = 骨名对上了
        if (!src_has_rot) {
            continue;  // 源没给旋转（走 rest）→ 该骨也是 identity
        }
        dst_pose->joint_rotation[i] = src_pose.joint_rotation[static_cast<size_t>(it->second)];
    }
    return stats;
}

}  // namespace jpov_fbx_viewer

#endif  // JPOV_DEMO_FBX_VIEWER_SKELETON_POSE_TRANSFER_H_
