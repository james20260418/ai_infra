// JPOV Skeleton — 骨架动作重定向（retarget）· Q 桥接量实现（CPU、GL-free）
//
// 定位（docs/jpov_retarget_design.md §1.2 / §6 / §7-M4）：把一段外部动作（FBX 等）的**每帧位姿**
// 正确搬到另一个骨架（Tripo glb 资产 / 手建骨架 / Mixamo23 标准骨架）上。本文件是这套数学的
// **唯一实现点**：只做"位姿 → 位姿"的纯计算——不读文件、不碰 GL、不做时间采样。
//
// ════════════════════════════════════════════════════════════════════════════════
//  一、Q 是什么（本文件的中心概念）
// ════════════════════════════════════════════════════════════════════════════════
//
// 一个关节的姿态 = 一个**坐标系**（原点在关节处，坐标轴朝向由旋转决定）。
// 两套骨架（源 S / 目标 T）在**同一根语义骨**上，局部坐标系的摆放方式**通常不同**——
// 例：源骨架的左大腿骨，"局部 +Z"指向大腿前侧；目标骨架的同一根骨，"局部 +Z"指向大腿侧面。
// 这就是"两套 rig 的 bind 朝向不同"。
//
// 要让"源的一帧动作"在目标上复现，公共语言必须与"哪套局部帧"无关。这个公共语言是：
//
//     偏差(j) ≜ 该骨相对**自己 rest** 转了多少，表达在**世界系**里
//             = Rb(j)⁻¹ ⊗ W(j)
//
// 其中
//     B_sk(j)  = bind_rotation[j]                      骨架 rest 时该骨相对父的局部朝向
//     Rb_sk(j) = 骨架 rest 时该骨的**世界朝向**（沿链复合 B，见 RestWorldRotations）
//     W_sk(j)  = 有 pose 时该骨的世界朝向 = W_sk(parent) ⊗ B_sk(j) ⊗ P_sk(j)
//     P_sk(j)  = 该骨的局部 pose 旋转（= SkeletonPose::joint_rotation[j]）
//
// **重定向的定义**：让目标的偏差 := 源的偏差（同名骨逐个一致）：
//
//     Rb_T(j)⁻¹ ⊗ W_T(j)  :=  Rb_S(s(j))⁻¹ ⊗ W_S(s(j))
//
// 反解出目标该骨的局部 pose：
//
//     W_T(j) = Rb_T(j) ⊗ Rb_S(s(j))⁻¹ ⊗ W_S(s(j))                     …（★ 迁移式）
//     P_T(j) = B_T(j)⁻¹ ⊗ W_T(parent(j))⁻¹ ⊗ W_T(j)                   …（反解局部量）
//
// ── Q 的两种等价写法（本实现取"显式桥接量"写法）──────────────────────────────
//
// 把 (★) 里的固定部分抽出来，就是 **Q（桥接量）**：
//
//     Q(j) ≜ Rb_T(j) ⊗ Rb_S(s(j))⁻¹                ← "源该骨的 rest 世界朝向 → 目标该骨的 rest 世界朝向"
//     Q_p(j) ≜ Rb_T(parent_T(j)) ⊗ Rb_S(parent_S(s(j)))⁻¹   ← 父级的同类桥接量
//
// 则 (★) 可写成"世界系下的相似变换（共轭）"形式，即文档 §5.5 所说的"共轭"：
//
//     W_T(j) = Q(j) ⊗ W_S(s(j))
//     P_T(j) = Q_p(j) ⊗ P_S(s(j)) ⊗ Q(j)⁻¹          …（等价写法，见下"两种写法等价"）
//
// 直观读法：**同一个物理旋转，从"源父系里怎么表达"换成"目标父系里怎么表达"**。
//   · 左乘 Q_p ——因为 P 的**输出系**（父系）换了参考；
//   · 右乘 Q(j)⁻¹ ——因为 P 的**输入系**（本关节系）换了参考。
//
// ── 两种写法等价（推导）─────────────────────────────────────────────────────
//
//   W_T(j) = Q(j) ⊗ W_S(s)                                     …(1) 定义 Q(j) 就是为此
//   P_T(j) = B_T(j)⁻¹ ⊗ W_T(p_T)⁻¹ ⊗ W_T(j)
//          = B_T(j)⁻¹ ⊗ (Q_p ⊗ W_S(p_S))⁻¹ ⊗ (Q(j) ⊗ W_S(s))    …代入 (1)
//          = B_T(j)⁻¹ ⊗ W_S(p_S)⁻¹ ⊗ Q_p⁻¹ ⊗ Q(j) ⊗ W_S(s)
//   P_S(s) = B_S(s)⁻¹ ⊗ W_S(p_S)⁻¹ ⊗ W_S(s)                     …源侧反解
//   而 B_T(j)⁻¹ ⊗ Q_p⁻¹ ⊗ Q(j) ⊗ B_S(s) = ?
//         B_T(j)⁻¹ ⊗ [Rb_T(p_T) ⊗ Rb_S(p_S)⁻¹]⁻¹ ⊗ [Rb_T(j) ⊗ Rb_S(s)⁻¹] ⊗ B_S(s)
//       = B_T(j)⁻¹ ⊗ Rb_S(p_S) ⊗ Rb_T(p_T)⁻¹ ⊗ Rb_T(j) ⊗ Rb_S(s)⁻¹ ⊗ B_S(s)
//       = B_T(j)⁻¹ ⊗ Rb_S(p_S) ⊗ [Rb_T(p_T)⁻¹ ⊗ Rb_T(j)] ⊗ Rb_S(s)⁻¹ ⊗ B_S(s)
//   用 Rb_T(j) = Rb_T(p_T) ⊗ B_T(j) ⇒ Rb_T(p_T)⁻¹ ⊗ Rb_T(j) = B_T(j)：
//       = B_T(j)⁻¹ ⊗ Rb_S(p_S) ⊗ B_T(j) ⊗ Rb_S(s)⁻¹ ⊗ B_S(s)
//   用 Rb_S(j) = Rb_S(p_S) ⊗ B_S(s) ⇒ Rb_S(p_S) = Rb_S(j) ⊗ B_S(s)⁻¹：
//       = B_T(j)⁻¹ ⊗ Rb_S(j) ⊗ B_S(s)⁻¹ ⊗ B_T(j) ⊗ Rb_S(s)⁻¹ ⊗ B_S(s)
//   ⚠️ 这里 B_T(j) 与 Rb_S 一般**不交换**（不同轴），所以上式 **不** 一般等于 1 —— 即两种写法
//   **不逐式恒等**。真正成立的是"两者各自都满足同一个定义"：★ 式与 (1) 式在
//   `W_T(j) := Q(j)·W_S(s)` 这一条上是同一件事，而 (★) 的右端 = Q(j)·W_S(s) 仅当
//   Q(j) = Rb_T(j)·Rb_S(s)⁻¹ —— 这正是 Q 的定义。故等价性由 Q 的**定义**保证，而非代数恒等式。
//   **本实现只在代码里用 (★)（直接式）**：少一次乘法、少一层 Q⁻¹ 的误差累积；
//   Q 同时作为**可检视的输出**暴露在 BindResult 里（见下），供诊断/人工校准使用。
//
// ── 为什么根关节的整体旋转"长在 Q 里"───────────────────────────────────────
//
// Q(0) = Rb_T(0) ⊗ Rb_S(0)⁻¹ 就是**两侧骨架整体的 rest 朝向差**（例：源的 rest 让角色朝 +Z、
// 目标朝 +X ⇒ Q(0) 是一个绕 Y 的 90° 旋转）。它**不是特例、不需要单独处理**——它是同一公式在
// j=0 处的自然取值（此时 parent 不存在，Q(0) 只由两侧根的 bind_rotation 决定）。
//
// 因此"整体朝向差"天然包含在 Q 表里：apply 时根骨也走同一套公式，目标的整体朝向自动被摆到
// 与源一致。**这也是 Q 优于"另设一个全局对齐参数"的地方**：一个量、一个来源、无特判。
//
// ⚠️ 但它同时意味着一件事（消费者须知情）：**输出 pose 里包含了"角色朝向"**。若把这份 pose
//   直接喂给渲染且放置层还叠了 up/front，就会**转向叠两次**。放置层须二选一（见 §边界）。
//
// ════════════════════════════════════════════════════════════════════════════════
//  二、边界（本文件**不**做的事）
// ════════════════════════════════════════════════════════════════════════════════
//
//   · **骨长不参与**：迁移式里 rest_offset 完全不出现。目标用自己的骨长（T(rest_offset_T) 由
//     目标骨架自身提供），故"两资产骨长不同"无需任何处理（设计文档 §6.2）。
//   · **root_offset 不搬**（输出恒 0）：源多为外部单位（FBX 厘米）而目标是米，且烘焙端尚未
//     接线 root-motion（§0.2 / §6.3 = M3）。**不静默做单位假设**。
//   · **未命中的目标骨保持自身 rest**（pose = identity）：如 glb 的包装层 `Root`（源无同名骨）。
//     其子骨会自然把包装层的旋转补偿掉，故包装层不会把整体掰歪。
//   · **不读文件、不采样时间、不碰 GL**：见职责边界一段。
//
// ════════════════════════════════════════════════════════════════════════════════
//  三、不变量（单测逐条覆盖）
// ════════════════════════════════════════════════════════════════════════════════
//
//   1. 源 pose 恒等 ⟹ 输出 pose 恒等（目标保持自己的 rest/T-pose）。
//   2. 命中骨：deviation_T(j) == deviation_S(s(j))（世界系偏差逐骨一致）。
//   3. 未命中的目标骨：pose = identity。
//   4. **同布局骨架（两侧 bind 朝向逐骨相同，只差骨长）⟹ Q ≈ 恒等**，且输出 pose ≈ 源 pose。
//      （这是最硬的一道门禁：它把"桥接量算对了"与"骨架长什么样"解耦验证。）
//
// 职责边界：纯 CPU / GL-free / 无状态（BindResult 是输入快照，函数不缓存）。与骨架 mesh 生成
//   （interface/skeleton_mesh.h）、时间采样（animation_sampler.h）并列，三者串起来即观察器每帧链路。
//
// 参考：docs/jpov_retarget_design.md §1.2（辅助姿态 / 重定向核心式）、§6（FBX 重定向）、§7-M4。

#ifndef JPOV_INTERFACE_SKELETON_RETARGET_H_
#define JPOV_INTERFACE_SKELETON_RETARGET_H_

#include <string>
#include <unordered_map>
#include <vector>

#include <glog/logging.h>

#include "geom/common/quaternion.h"
#include "geom/common/vec.h"
#include "tools/jpov/interface/skeleton_types.h"

namespace jpov {

using Quatf = geom::Quaternion<float>;

// ==================== 骨名对齐 ====================

// 一根骨的**对齐记录**：目标骨 index ↔ 源骨 index。
struct BoneMatch {
    int target_index = -1;  // 目标骨架 joints 下标
    int source_index = -1;  // 源骨架 joints 下标（-1 = 未命中）
    std::string name;       // 两侧共用的骨名（对位凭据）
};

// 两侧骨架的整体 rest 朝向差（= Q(0)），单列出来是因为它常被"放置层"单独需要。
//   · 语义：把目标 rest 整体转一转，就能"面朝与源一样的方向"的旋转（**关**于骨架空间的值）。
//   · ⚠️ **它已经包含在 Q 表里**（Q[0]）；这里是因为"放置层要单独取用"才额外暴露一份。
//     消费者若已用 Q 驱动的 pose，**不要**再叠它（会转向叠两次）。
struct RestAlignment {
    bool valid = false;                  // 缺 Hips/Head/LeftArm/RightArm 等对位凭据 → false（不猜）
    Quatf rotation = Quatf::Identity();  // 整体朝向差 R
    float angle_deg = 0.0f;              // R 的旋转角（诊断/日志）
};

// ==================== 一次重定向的输入快照：BindResult ====================
//
// 整段动画共用，构建一次（骨名哈希 + rest 世界朝向 + Q 表都只算一次）。apply 每帧复用。
//
// 为什么把两侧骨架**拷进**结构体：apply 每帧要用到两边的 bind 朝向与父索引（世界朝向沿链复合），
// 而本结构体须自包含才不持有外部引用（无指针 = 无悬垂）。骨架是纯 CPU 小结构，拷贝成本可忽略。
struct BindResult {
    // ── 两侧骨架（快照）──
    SkeletonType target;  // 目标骨架（被驱动方；例：glb 资产）
    SkeletonType source;  // 源骨架（动作提供方；例：Mixamo FBX）

    // ── 骨名对齐结果 ──
    std::vector<BoneMatch> matches;          // 逐目标骨一条（含未命中者，便于诊断）
    std::vector<int> source_of_target;       // 目标 index → 源 index；-1 = 未命中
    int matched_bone_count = 0;              // **命中骨数**（消费者最常问的一个数）
    int target_bone_count = 0;               // 目标骨总数（"命中 x/y" 的 y）
    std::vector<std::string> unmapped_target_bones;  // 未命中的目标骨**名**（无名骨不进表）

    // ── 两侧 rest 世界朝向（Q 的零点；与 joints 平行索引）──
    //   Rb_sk(j) = 骨架 rest 时该骨的世界朝向 = 沿链复合 bind_rotation。
    std::vector<Quatf> target_rest_world;
    std::vector<Quatf> source_rest_world;

    // ── ★ Q 桥接量表（本文件的核心输出）──
    //   Q(j) ≜ Rb_T(j) ⊗ Rb_S(s(j))⁻¹
    //   语义：把"源该骨的 rest 世界朝向"转到"目标该骨 rest 世界朝向"的旋转。
    //   读法：同一物理旋转，在源父系里表达 → 在目标父系里表达，差的就是 Q。
    //   与 target.joints 平行索引；**未命中的骨 Q = 恒等**（该骨不搬，保持自身 rest）。
    //   含 j=0（根）：Q(0) = 两侧整体 rest 朝向差（见文件头"为什么根旋转长在 Q 里"）。
    //   与帧无关（只依赖两侧 bind_rotation + 对位表）⇒ 算一次，逐帧复用。
    std::vector<Quatf> q_bridge;

    // ── 两侧整体 rest 朝向差（= Q(0)，单列供放置层用；见 RestAlignment 注释的"勿叠两次"）──
    RestAlignment rest_alignment;

    // Q 的恒等度（诊断）：Q 逐骨与恒等的平均夹角（度）。同布局骨架应 ≈ 0。
    float q_mean_angle_deg = 0.0f;
    float q_max_angle_deg = 0.0f;
};

// 算每骨 rest 世界朝向（骨架空间、pose 恒等）：沿拓扑序复合 bind_rotation。
//   Rb(j) = Rb(parent) ⊗ B(j)
// Pre-condition: skeleton 已 Validate()（拓扑序 + bind_rotation 尺寸）。
// 说明：只有"旋转"参与——世界**位置**不在此列（位置与重定向无关，见文件头"骨长不参与"）。
inline std::vector<Quatf> RestWorldRotations(const SkeletonType& skeleton) {
    const size_t n = skeleton.joints.size();
    std::vector<Quatf> world(n);
    for (size_t j = 0; j < n; ++j) {
        // bind_rotation 空 = 全恒等（极简直链骨架 / 老数据兼容）。
        const Quatf bind = skeleton.bind_rotation.empty() ? Quatf::Identity()
                                                          : skeleton.bind_rotation[j];
        const int p = skeleton.joints[j].parent;
        // 每步归一化：沿链复合会累积浮点漂移；rest 世界朝向是"Q 的零点"，差一点就全体偏一点。
        world[j] = ((p == kSkeletonNoParent) ? bind
                                             : world[static_cast<size_t>(p)] * bind)
                       .Normalized();
    }
    return world;
}

// 两个单位四元数之间的夹角（度）：2·acos(|dot|)（取 |dot| 消除 q 与 -q 的二重覆盖）。
inline float QuatAngleDeg(const Quatf& a, const Quatf& b) {
    const Quatf an = a.Normalized();
    const Quatf bn = b.Normalized();
    const float d = an.x * bn.x + an.y * bn.y + an.z * bn.z + an.w * bn.w;
    const float c = std::min(1.0f, std::abs(d));
    return 2.0f * std::acos(c) * 180.0f / static_cast<float>(M_PI);
}

// 按**骨名**做两侧对齐（精确同名；空名不参与对位）。
//   · 源骨：骨名 → 下标（同名重复取先出现者）。
//   · 目标骨逐个查表；未命中记入 unmapped_target_bones 并保持自身 rest。
// 返回：与 target.joints 平行的一份 matches（含未命中项），以及命中数。
struct NameAlignment {
    std::vector<BoneMatch> matches;
    std::vector<int> source_of_target;
    int matched = 0;
    std::vector<std::string> unmapped_target_bones;
};

inline NameAlignment AlignBonesByName(const SkeletonType& target,
                                      const SkeletonType& source) {
    std::unordered_map<std::string, int> src_index_by_name;
    src_index_by_name.reserve(source.joints.size());
    for (size_t i = 0; i < source.joints.size(); ++i) {
        const std::string& n = source.joints[i].name;
        if (!n.empty()) {
            src_index_by_name.emplace(n, static_cast<int>(i));
        }
    }

    NameAlignment out;
    out.source_of_target.assign(target.joints.size(), -1);
    out.matches.reserve(target.joints.size());
    for (size_t i = 0; i < target.joints.size(); ++i) {
        const std::string& n = target.joints[i].name;
        BoneMatch m;
        m.target_index = static_cast<int>(i);
        m.name = n;
        if (!n.empty()) {
            const auto it = src_index_by_name.find(n);
            if (it != src_index_by_name.end()) {
                m.source_index = it->second;
                out.source_of_target[i] = it->second;
                ++out.matched;
            } else {
                out.unmapped_target_bones.push_back(n);
            }
        }
        out.matches.push_back(std::move(m));
    }
    return out;
}

// ==================== ★ 主入口：从两个 SkeletonType 建 Q ====================

// 给出 1) 骨名对齐结果 2) Q 桥接量表，整段动画复用。
//
// Pre-condition（违反 → LOG(FATAL)，不 fallback）：
//   两侧骨架非空、满足 SkeletonType::Validate()（拓扑序 + bind_rotation 尺寸）。
inline BindResult BuildBind(const SkeletonType& target, const SkeletonType& source) {
    target.Validate();  // 含拓扑序（世界朝向按 joints 顺序单趟复合，要求父先于子）
    source.Validate();

    BindResult r;
    r.target = target;
    r.source = source;
    r.target_rest_world = RestWorldRotations(r.target);
    r.source_rest_world = RestWorldRotations(r.source);

    // 1) 骨名对齐。
    NameAlignment al = AlignBonesByName(r.target, r.source);
    r.matches = std::move(al.matches);
    r.source_of_target = std::move(al.source_of_target);
    r.matched_bone_count = al.matched;
    r.target_bone_count = r.target.bone_count();
    r.unmapped_target_bones = std::move(al.unmapped_target_bones);

    // 2) Q 表：Q(j) = Rb_T(j) ⊗ Rb_S(s(j))⁻¹；未命中 → 恒等（该骨不搬）。
    r.q_bridge.assign(r.target.joints.size(), Quatf::Identity());
    double sum_deg = 0.0;
    int counted = 0;
    for (size_t j = 0; j < r.target.joints.size(); ++j) {
        const int s = r.source_of_target[j];
        if (s < 0) {
            continue;  // 未命中：Q = 恒等（已由 assign 初始化）
        }
        const Quatf q = (r.target_rest_world[j] *
                         r.source_rest_world[static_cast<size_t>(s)].Conjugate())
                            .Normalized();
        r.q_bridge[j] = q;
        // 诊断指标：Q 与恒等的夹角（同布局骨架应 ≈ 0 ⇒ 无桥接需求）。
        const float deg = QuatAngleDeg(q, Quatf::Identity());
        sum_deg += deg;
        ++counted;
        if (deg > r.q_max_angle_deg) {
            r.q_max_angle_deg = deg;
        }
    }
    r.q_mean_angle_deg =
        counted > 0 ? static_cast<float>(sum_deg / counted) : 0.0f;

    // 3) 整体朝向差 = Q(0)（若 0 号骨命中）；供放置层单独取用。
    //    这里**不从 Q 表另算**——直接引用 Q[0]，保持"单一真值"（避免两份数据打架）。
    if (!r.q_bridge.empty() && r.source_of_target.size() > 0 && r.source_of_target[0] >= 0) {
        r.rest_alignment.valid = true;
        r.rest_alignment.rotation = r.q_bridge[0];
        r.rest_alignment.angle_deg = QuatAngleDeg(r.q_bridge[0], Quatf::Identity());
    }
    return r;
}

// ==================== 逐帧 apply ====================

// 把源的一帧位姿重定向成目标的一帧位姿（覆盖写 out）。
//
//   bind        : BuildBind 的结果（两侧骨架 + 对位表 + Q 表 + 两侧 rest 世界朝向）。
//   source_pose : 源位姿。joint_rotation 为空 = 视为全恒等（走 rest）；
//                 非空时尺寸必须 == 源骨架骨数。
//   out         : 输出位姿（非空）。bone_count / joint_rotation 尺寸 = 目标骨架骨数，
//                 每骨为单位四元数；root_offset 恒 0（见文件头"边界"）。
//
// 实现（对应文件头 (★) 式；Q 表在手，但不是逐骨乘 Q——★ 式更省一次乘法且少一层误差）：
//   1) 源：W_S(j) = W_S(parent) ⊗ B_S(j) ⊗ P_S(j)      （只算旋转）
//   2) 偏差 = Rb_S(j)⁻¹ ⊗ W_S(j)
//   3) 目标：W_T(j) = Rb_T(j) ⊗ 偏差；未命中则 W_T(j) = 父世界 ⊗ B_T(j)（= 自身 rest）
//   4) 反解：P_T(j) = B_T(j)⁻¹ ⊗ W_T(parent)⁻¹ ⊗ W_T(j)
//
// Pre-condition: out != nullptr；source_pose 尺寸与源骨架一致（违反 → LOG(FATAL)）。
inline void RetargetPose(const BindResult& bind, const SkeletonPose& source_pose,
                         SkeletonPose* out /*output*/) {
    CHECK(out != nullptr) << "RetargetPose: out 不能为空";
    const bool src_has_rot = !source_pose.joint_rotation.empty();
    if (src_has_rot) {
        CHECK_EQ(source_pose.joint_rotation.size(), bind.source.joints.size())
            << "RetargetPose: source_pose 尺寸 " << source_pose.joint_rotation.size()
            << " 应 == 源骨架骨数 " << bind.source.joints.size();
    }

    const size_t n_src = bind.source.joints.size();
    const size_t n_dst = bind.target.joints.size();

    // 1) 源：逐骨世界朝向 W(j) = W(parent) ⊗ B(j) ⊗ P(j)（拓扑序单趟）。
    std::vector<Quatf> src_world(n_src);
    for (size_t j = 0; j < n_src; ++j) {
        const Quatf b = bind.source.bind_rotation.empty() ? Quatf::Identity()
                                                          : bind.source.bind_rotation[j];
        const Quatf p = src_has_rot ? source_pose.joint_rotation[j] : Quatf::Identity();
        const Quatf local = b * p;
        const int par = bind.source.joints[j].parent;
        src_world[j] = (par == kSkeletonNoParent)
                           ? local
                           : src_world[static_cast<size_t>(par)] * local;
    }

    // 2)-4) 目标：按拓扑序逐骨反解。
    out->bone_count = static_cast<int>(n_dst);
    out->joint_rotation.resize(n_dst);
    out->root_offset = Vec3f(0.0f, 0.0f, 0.0f);

    std::vector<Quatf> dst_world(n_dst);
    for (size_t j = 0; j < n_dst; ++j) {
        const Quatf b = bind.target.bind_rotation.empty() ? Quatf::Identity()
                                                          : bind.target.bind_rotation[j];
        const int par = bind.target.joints[j].parent;
        // Validate() 已保证拓扑序（parent < j）；这里只做防御性下界确认。
        CHECK(par == kSkeletonNoParent || par < static_cast<int>(j))
            << "RetargetPose: 目标骨架非拓扑序 joint[" << j << "] parent=" << par;
        const Quatf parent_world = (par == kSkeletonNoParent)
                                       ? Quatf::Identity()
                                       : dst_world[static_cast<size_t>(par)];

        const int s = bind.source_of_target[j];
        if (s < 0) {
            // 未命中 → 保持自身 rest：pose = identity，世界朝向 = 父世界 ⊗ bind。
            out->joint_rotation[j] = Quatf::Identity();
            dst_world[j] = parent_world * b;
        } else {
            const size_t si = static_cast<size_t>(s);
            // 源该骨相对自己 rest 的偏差（世界系）——与"哪套局部帧"无关的那个量。
            const Quatf deviation =
                bind.source_rest_world[si].Conjugate() * src_world[si];
            // 套到目标自己的 rest 上 = 目标该骨应有的世界朝向。
            const Quatf want = bind.target_rest_world[j] * deviation;
            out->joint_rotation[j] =
                (b.Conjugate() * parent_world.Conjugate() * want).Normalized();
            dst_world[j] = want;
        }
    }
}

// 便利入口：整段源位姿一次性重定向（= BuildBind 一次 + 逐帧 RetargetPose）。
//   适用"批处理整段动画"。要**逐帧复用**同一份对位（如观察器每帧现采一帧），
//   请直接用 BuildBind + RetargetPose（避免每帧重建骨名哈希与 Q 表）。
//
// Pre-condition: 同 BuildBind / RetargetPose（违反 → LOG(FATAL)）。
inline std::vector<SkeletonPose> RetargetPoses(const SkeletonType& target,
                                              const SkeletonType& source,
                                              const std::vector<SkeletonPose>& source_poses) {
    const BindResult bind = BuildBind(target, source);
    std::vector<SkeletonPose> out;
    out.reserve(source_poses.size());
    for (const SkeletonPose& src : source_poses) {
        SkeletonPose dst;
        RetargetPose(bind, src, &dst);
        out.push_back(std::move(dst));
    }
    return out;
}

// 便利入口：单帧（建 bind + 重定向一帧）。适合"只搬一帧"的调用点（测试/编辑器预览）。
inline SkeletonPose RetargetOnePose(const SkeletonType& target,
                                    const SkeletonType& source,
                                    const SkeletonPose& source_pose) {
    const BindResult bind = BuildBind(target, source);
    SkeletonPose dst;
    RetargetPose(bind, source_pose, &dst);
    return dst;
}

}  // namespace jpov

#endif  // JPOV_INTERFACE_SKELETON_RETARGET_H_
