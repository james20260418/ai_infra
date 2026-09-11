// JPOV Skeleton — Mixamo 23 骨标准骨架工厂（程序化生成，GL-free）
//
// 目的：**无资产**造出一个符合 Mixamo 官方布局的人形骨架（23 骨），供
//   - 火柴人 mesh 生成（JPOV retarget 的验证载体，见 docs/jpov_retarget_design.md §4）
//   - 蒙皮静态退化门 / retarget 数学的单测基准
//   - 将来接 FBX 动作时的 target 骨架
//
// 数据来源（重要，见下）：本文件的骨长比例 + bind 朝向 **全部从 Mixamo 官方 FBX 实测反推**
// （`Hip Hop Dancing.fbx`，2026-09-11 用 ufbx 逐 node 提取 local_transform）。
//   - Mixamo 从未发布骨架数值文档（骨名/拓扑只有社区映射表；官方只发资产），
//     故按 Danis 约定「优先上网查，查不到则用资产反推」。
//   - **不用 Tripo 生成的 mixamo_male.glb** 反推：该资产骨名沿用 mixamorig:*，
//     但骨架是 Tripo 烘的（手臂朝 ±Z、rest 朝向另有一套），与官方布局不同源。
//
// 骨架约定（Mixamo 官方布局，见 docs/jpov_retarget_design.md §2）：
//   - 骨长轴 = +Y（各骨 rest_offset 主分量在 +Y）
//   - 骨 up/roll = +Z
//   - bind_rotation 通常 ≈ 恒等，但**非恒等**处表达各骨的 rest 朝向
//     （如 LeftShoulder 的 ±90° 级旋转把手臂掰成水平 ±X；LeftUpLeg 的 w=0 表示腿向下长）
//   - **pose 全恒等 → 本骨架呈现标准 T-pose**（头顶 ~1.60m、手 ±0.71m 水平、脚踩地）
//
// 单位：**米**。源 FBX 是厘米（`unit_meters = 0.01`），本文件把 rest_offset ×0.01 转成米；
//   `bind_rotation` 是纯旋转，与单位无关，原样搬。
//
// 身高语义（Danis 定）：`height` = **骨骼身高**（脚底到头顶，**不含皮**、不含头发）。
//   官方资产实测骨骼身高 ≈ 1.600 m（Head 骨顶 y≈1.5993，ToeBase y≈0.0009 踩地）。
//   本工厂按 `scale = height / kMixamoOfficialBoneHeight` 对 **rest_offset 等比缩放**，
//   骨长比例严格保持官方；**bind_rotation 不变**（旋转与尺度无关）。
//
// 不在这里做的（留给后续 PR）：
//   - 「从资产反推骨长比例」的通用函数（架构 §5 / 编辑校准工具）
//   - 部位级微调（腿长/臂长系数）—— 官方比例优先，微调由资产反推承担

#ifndef JPOV_INTERFACE_MIXAMO23_SKELETON_H_
#define JPOV_INTERFACE_MIXAMO23_SKELETON_H_

#include <glog/logging.h>
#include "geom/common/quaternion.h"
#include "geom/common/vec.h"
#include "tools/jpov/interface/skeleton_types.h"

namespace jpov {

// Mixamo 官方骨架的**骨骼身高**（米）：脚底(ToeBase)到头顶(Head 骨末端)。
// 由 `Hip Hop Dancing.fbx` 实测 T-pose 世界坐标反推（Head y=1.5993，ToeBase y=0.0009）。
// 用途：把用户给的 `height`（骨骼身高，米）换算成对官方 rest_offset 的等比缩放系数。
inline constexpr float kMixamoOfficialBoneHeight = 1.600f;

// 本骨架的骨数（23）：Root + 22 具名人形骨。与常见简化 Mixamo 骨架（去手指/去叶端）一致。
inline constexpr int kMixamo23BoneCount = 23;

// 造一个符合 Mixamo 官方布局的 23 骨人形 SkeletonType。
//
// 参数：
//   height —— **骨骼身高**（米，脚底到头顶，不含皮）。必须 > 0。
//             默认 1.75 为常见成年人身高；传 0 或负值 → LOG(FATAL)（不 fallback，见工程约定）。
//
// 返回：SkeletonType{ joints(23 骨树 + rest_offset) , bind_rotation(23 个官方朝向) }。
//   - 骨名用 `mixamorig:*`（与 FBX 动画源同名，便于按名对位；Root 无名，是包装层）。
//   - joints 拓扑序：0=Root，其余 parent 索引均 < 自身。
//   - inverse_bind 不存（派生量）—— 需要时调 `type.ComputeInverseBind()`，
//     程序化骨架自算天然正确（见 docs/jpov_retarget_design.md §5.5）。
//
// 配套 T-pose：`SkeletonPose::Identity(type.bone_count())`（本骨架 bind 朝向已含 T-pose 姿态，
//   pose 不再叠旋转，故全恒等 pose 驱动出 T-pose）。
inline SkeletonType Mixamo23Skeleton(float height = 1.75f) {
    CHECK_GT(height, 0.0f)
        << "Mixamo23Skeleton: height(骨骼身高,米) 必须 >0，收到 " << height;

    const float k = height / kMixamoOfficialBoneHeight;  // 对官方比例的整体缩放

    // ── 官方骨架表（FBX 实测；长度已 cm→m，旋转原样）────────────────────────────
    // 每项：{ 骨名, parent 索引, rest_offset(米, 未缩放), bind_rotation(x,y,z,w) }
    // ⚠️ 表内 rest_offset 是**官方 1.600m 骨架**的值；实际输出时统一 ×k。
    // ⚠️ 顺序即 joints 下标，须满足拓扑序（parent < 自身）。
    struct Entry {
        const char* name;
        int parent;
        float tx, ty, tz;       // rest_offset（米，官方 1.6m 基准）
        float rx, ry, rz, rw;   // bind_rotation（单位四元数，相对父）
    };
    static const Entry kTable[kMixamo23BoneCount] = {
        // idx 0：Root —— 包装层。官方 FBX 里 Hips 直接是顶层，无 Root；
        //   这里补一个恒等 Root 作为 0 号关节，好处：
        //   ① 与 glTF 资产（Armature/Root 包装层）结构一致，便于跨源对位；
        //   ② 给整体位移/旋转（root-motion、模型摆放）一个统一抓手。
        {"",              kSkeletonNoParent, 0.0f,      0.0f,      0.0f,      0.0f, 0.0f, 0.0f, 1.0f},

        {"mixamorig:Hips",        0,  0.000000f,  1.042749f,  0.000000f,  0.006459f,  0.000000f,  0.000000f,  0.999979f},
        {"mixamorig:Spine",       1,  0.000000f,  0.101824f,  0.000000f, -0.080155f,  0.000000f,  0.000000f,  0.996782f},
        {"mixamorig:Spine1",      2,  0.000000f,  0.100027f,  0.000000f,  0.000000f,  0.000000f,  0.000000f,  1.000000f},
        {"mixamorig:Spine2",      3,  0.000000f,  0.093221f,  0.000000f,  0.012885f,  0.000000f,  0.000000f,  0.999917f},
        {"mixamorig:Neck",        4,  0.000000f,  0.168653f,  0.000000f,  0.000000f,  0.000000f,  0.000000f,  1.000000f},
        {"mixamorig:Head",        5,  0.000000f,  0.093419f,  0.028410f,  0.000000f,  0.000000f,  0.000000f,  1.000000f},

        {"mixamorig:LeftShoulder", 4,  0.045704f,  0.111956f, -0.008066f, -0.484423f, -0.570970f,  0.526162f, -0.403090f},
        {"mixamorig:LeftArm",      7,  0.000000f,  0.108377f,  0.000000f, -0.024607f, -0.002562f,  0.103504f,  0.994321f},
        {"mixamorig:LeftForeArm",  8,  0.000000f,  0.278415f,  0.000000f,  0.000000f,  0.000000f,  0.000000f,  1.000000f},
        {"mixamorig:LeftHand",     9,  0.000000f,  0.283288f,  0.000000f,  0.000000f,  0.000001f,  0.000000f,  1.000000f},

        {"mixamorig:RightShoulder", 4, -0.045700f,  0.111958f, -0.008066f, -0.484430f,  0.570964f, -0.526163f, -0.403087f},
        {"mixamorig:RightArm",     11,  0.000000f,  0.108382f,  0.000000f, -0.024616f,  0.002562f, -0.103499f,  0.994322f},
        {"mixamorig:RightForeArm", 12,  0.000000f,  0.278415f,  0.000000f,  0.000000f,  0.000000f,  0.000000f,  1.000000f},
        {"mixamorig:RightHand",    13,  0.000000f,  0.283288f,  0.000000f,  0.000000f,  0.000003f,  0.000000f,  1.000000f},

        {"mixamorig:LeftUpLeg",    1,  0.082078f, -0.067718f, -0.015122f,  0.000000f,  0.010368f,  0.999946f,  0.000000f},
        {"mixamorig:LeftLeg",     15,  0.000000f,  0.443714f,  0.000000f, -0.038112f,  0.000000f,  0.000000f,  0.999273f},
        {"mixamorig:LeftFoot",    16,  0.000000f,  0.445278f,  0.000000f,  0.459749f,  0.000000f,  0.000000f,  0.888049f},
        {"mixamorig:LeftToeBase", 17,  0.000000f,  0.138169f,  0.000000f,  0.335241f,  0.000000f,  0.000000f,  0.942132f},

        {"mixamorig:RightUpLeg",   1, -0.082078f, -0.067718f, -0.015122f,  0.000000f,  0.010357f,  0.999946f,  0.000000f},
        {"mixamorig:RightLeg",    19,  0.000000f,  0.443715f,  0.000000f, -0.038091f,  0.000000f,  0.000000f,  0.999274f},
        {"mixamorig:RightFoot",   20,  0.000000f,  0.445278f,  0.000000f,  0.459740f,  0.000000f,  0.000000f,  0.888053f},
        {"mixamorig:RightToeBase",21,  0.000000f,  0.138169f,  0.000000f,  0.335242f,  0.000000f,  0.000000f,  0.942132f},
    };

    SkeletonType type;
    type.joints.reserve(kMixamo23BoneCount);
    type.bind_rotation.reserve(kMixamo23BoneCount);
    for (int i = 0; i < kMixamo23BoneCount; ++i) {
        const Entry& e = kTable[i];
        SkeletonJoint j;
        j.parent = e.parent;
        j.rest_offset = Vec3f(e.tx * k, e.ty * k, e.tz * k);  // 等比缩放到目标身高
        j.name = e.name;
        type.joints.push_back(std::move(j));
        // bind_rotation 与尺度无关，原样收（表内已是单位四元数）。
        type.bind_rotation.push_back(
            geom::Quaternion<float>(e.rx, e.ry, e.rz, e.rw));
    }
    type.Validate();
    return type;
}

}  // namespace jpov

#endif  // JPOV_INTERFACE_MIXAMO23_SKELETON_H_
