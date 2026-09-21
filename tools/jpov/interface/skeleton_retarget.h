// JPOV Skeleton — 骨架动作重定向（retarget）· 人体随动系实现（CPU、GL-free）
//
// 定位（docs/jpov_retarget_design.md §1.2 / §6 / §7-M4）：把一段外部动作（FBX 等）的**每帧位姿**
// 正确搬到另一个骨架（Tripo glb 资产 / 手建骨架 / Mixamo23 标准骨架）上。本文件是这套数学的
// **唯一实现点**：只做"位姿 → 位姿"的纯计算——不读文件、不碰 GL、不做时间采样。
//
// ════════════════════════════════════════════════════════════════════════════════
//  一、不变量（Danis 2026-09-15 定，全部推导的出发点）
// ════════════════════════════════════════════════════════════════════════════════
//
// 一个关节的姿态 = 一个**坐标系**（原点在关节处，坐标轴朝向由旋转决定）。两套骨架（源 S /
// 目标 T）在同一根语义骨上，局部坐标系的摆放**通常不同**（例：源左大腿的"局部 +Z"指前侧、
// 目标同一根骨指侧面）。
//
// 让"源的一帧动作"在目标上复现，公共语言必须与"哪套局部帧"无关。取「**关节相对 bind 的
// 额外旋转**（世界系表达）」：
//
//     Δ_sk(j) ≜ W_sk(j) · Rb_sk(j)⁻¹
//
//     其中 B_sk(j)  = bind_rotation[j]                   骨架 rest 时该骨相对父的局部朝向
//          Rb_sk(j) = 沿链复合 B 得到的 **rest 世界朝向**（见 RestWorldRotations）
//          W_sk(j)  = W_sk(parent) ⊗ B_sk(j) ⊗ P_sk(j)     有 pose 时的世界朝向
//          P_sk(j)  = SkeletonPose::joint_rotation[j]（相对 bind 的增量 = FBX 的 Lcl Rotation 语义）
//
// **不变式**：Δ 在「**人体随动系**」下的数值跨资产相同：
//
//     M_s⁻¹ · Δ_s(j) · M_s   ==   M_t⁻¹ · Δ_t(j) · M_t                …⟨★⟩
//
// ⚠️ 前提：`M` 必须由**几何**（人体随动系，见 EstimateBodyFrame）确定，**不能**用逐骨 bind
//    帧（`Rb_j`）—— 后者含**几何看不见的 roll**（骨指向不变、子骨位置基本不变）。拿它当
//    基准去共轭，等于**无理由地把动作转轴拧一下**：roll 差 180° 时动作被**镜像**。
//    （这是一条已修的 bug：旧的逐骨 Q = Rb_T·Rb_S⁻¹ 就栽在这里，最小用例可复现。）
//
// ── 由 ⟨★⟩ 推出的两条式子（Danis 逐行验过）──────────────────────────────────
//
//   ⟨1⟩ 目标该骨应有的世界总旋转：
//
//        W_t(j) = Q_body · W_s(j) · Rb_s(j)⁻¹ · Q_body⁻¹ · Rb_t(j)
//
//        其中 Q_body ≜ M_t · M_s⁻¹   （一个常量，整段动画只算一次）
//
//   ⟨2⟩ 反解目标的 local rotation（= SkeletonPose::joint_rotation[j]）：
//
//        P_t(j) = b_t(j)⁻¹ · W_t(parent_T(j))⁻¹ · W_t(j)
//
//        b_t(j) = **目标骨架自己的** bind_rotation(j)（不搬源的 bind）。
//        按**拓扑序**逐骨算（父先于子）；根节点 W_t(parent) 视作**恒等**。
//
// **整体朝向差**（例：源朝 +Z、目标朝 +X）由 `Q_body` 承载 —— 不是特例，就是两侧人体系
// 之比。故目标保持**自己的** rest 朝向，蓝骨"站姿朝向"不会变成源的（面板把 Q_body 报出来）。
//
// ════════════════════════════════════════════════════════════════════════════════
//  二、边界（本文件**不**做的事）
// ════════════════════════════════════════════════════════════════════════════════
//
//   · **骨长不参与**：⟨1⟩⟨2⟩ 里 rest_offset 完全不出现。目标用自己的骨长，故"两资产骨长
//     不同"无需任何处理（设计文档 §6.2）。
//   · **root_offset 会被搬**（2026-09-20 起）：按 `Q_body · root_offset_s · (leg_t/leg_s)`
//     缩放搬运（scale adaptation + 朝向归一），见 `BodyRetargetPose` 内注释与
//     docs/jpov_root_offset_design.md。源为 0 ⇒ 输出 0（向后兼容）。
//     ⚠️ **单位/尺度契约（硬要求）**：`leg_t/leg_s` 是无量纲**比值**，只能做**比例缩放**、
//     **不能做单位换算**。故 `source_pose` 的长度量必须与 `plan.source` **同一尺度**
//     （即 source_pose 得与 plan.source 配套）。违反它不会报错 —— 只会把位移静默放大
//     `1/unit` 倍（如把 cm 的位姿配到 m 的骨架 ⇒ 100× ⇒ 角色“飞走” 63 m，真实踩过）。
//     ✅ **现状已是安全区**：FBX / glTF 两个 loader 都在**加载边界**把长度量换成米
//     （见 fbx_loader.h「单位铁律」），故本仓内合法的输入天然同尺度 —— 这条契约从
//     “调用方要自觉”变成“loader 已保证”。自行造 pose 的调用方（如注入的 test pose）
//     仍需遵守：长度量请用米。
//     ⚠️ 改变资产 bind（骨长/rest 朝向/根位置）⇒ 已重定向的 poses 必须**重新重定向**
//     （root_offset 携带"属于哪份骨架"的尺度与坐标系）。
//   · **未命中的目标骨保持自身 rest**（pose = identity）：如 glb 的包装层 `Root`（源无同名骨）。
//     其子骨会自然把包装层的旋转补偿掉，故包装层不会把整体掰歪。
//   · **不读文件、不采样时间、不碰 GL**。
//   · ⚠️ **单根骨自己的段方向差，Q_body 吸收不了**（Q_body 只含**整体**朝向差）。要逐骨完全
//     一致须做资产规范化（改资产的 rest/bind，设计文档 §5 / M5）。属已知残余。
//
// ════════════════════════════════════════════════════════════════════════════════
//  三、不变量（单测逐条覆盖）
// ════════════════════════════════════════════════════════════════════════════════
//
//   1. 源 pose 恒等 ⟹ 输出 pose 恒等（目标保持自己的 rest/T-pose）。
//   2. **几何一致的两套骨架（不管 bind/rest_offset 自由度怎么变）⟹ 零误差**；
//      再加一个 root 整体旋转也成立（结果只差 Q_body）。← 最硬的一道门禁。
//   3. 有几何朝向真差时：目标骨指向 == Q_body · 源骨指向。
//   4. 未命中的目标骨：pose = identity。
//
// 职责边界：纯 CPU / GL-free / 无状态（BodyRetargetPlan 是输入快照，函数不缓存）。与骨架 mesh
//   生成（interface/skeleton_mesh.h）、时间采样（animation_sampler.h）并列，三者串起来即观察器每帧链路。
//
// 参考：docs/jpov_retarget_design.md §1.2（重定向核心式）、§6（FBX 重定向）、§7-M4。

#ifndef JPOV_INTERFACE_SKELETON_RETARGET_H_
#define JPOV_INTERFACE_SKELETON_RETARGET_H_

#include <string>
#include <unordered_map>
#include <utility>
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


// ==================== 骨架几何辅助（人体随动系要用） ====================

// 算每骨 rest 世界朝向（骨架空间、pose 恒等）：沿拓扑序复合 bind_rotation。
//   Rb(j) = Rb(parent) ⊗ B(j)
// 只有“旋转”参与——世界**位置**不在内（位置与重定向无关，见文件头“骨长不参与”），
// 但人体随动系要位置，见 RestWorldPositions。
// Pre-condition: skeleton 已 Validate()（拓扑序 + bind_rotation 尺寸）。
inline std::vector<Quatf> RestWorldRotations(const SkeletonType& skeleton) {
    const size_t n = skeleton.joints.size();
    std::vector<Quatf> world(n);
    for (size_t j = 0; j < n; ++j) {
        // bind_rotation 空 = 全恒等（极简直链骨架 / 老数据兼容）。
        const Quatf bind = skeleton.bind_rotation.empty() ? Quatf::Identity()
                                                          : skeleton.bind_rotation[j];
        const int p = skeleton.joints[j].parent;
        // 每步归一化：沿链复合会累积浮点漂移。
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

// 找骨名以 suffix **结尾**（大小写敏感）的关节；找不到返回 -1。
// 例：suffix="LeftHand" 可匹配 "mixamorig:LeftHand"（兼容前缀差异）。
inline int FindJointBySuffix(const SkeletonType& skeleton, const std::string& suffix) {
    for (size_t i = 0; i < skeleton.joints.size(); ++i) {
        const std::string& n = skeleton.joints[i].name;
        if (n.size() >= suffix.size() &&
            n.compare(n.size() - suffix.size(), suffix.size(), suffix) == 0) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// rest 姿态下每关节在**骨架空间**下的世界位置：
//   pos(j) = pos(parent) + R(parent 的 rest 世界朝向) · rest_offset(j)
//   根：pos(root) = rest_offset(root)（根的 rest_offset 本就在骨架空间里）。
// 只算**位置**（不碰 pose）；与重定向无关，供几何判定用（人体随动系等）。
// Pre-condition: skeleton 已 Validate()（拓扑序 ⇒ 单趟复合即可）。
inline std::vector<Vec3f> RestWorldPositions(const SkeletonType& skeleton) {
    const std::vector<Quatf> world = RestWorldRotations(skeleton);
    const size_t n = skeleton.joints.size();
    std::vector<Vec3f> pos(n);
    for (size_t j = 0; j < n; ++j) {
        const int p = skeleton.joints[j].parent;
        if (p == kSkeletonNoParent) {
            pos[j] = skeleton.joints[j].rest_offset;
        } else {
            pos[j] = pos[static_cast<size_t>(p)] +
                     geom::RotateVector(world[static_cast<size_t>(p)],
                                        skeleton.joints[j].rest_offset);
        }
    }
    return pos;
}

// ==================== 人体随动系（body-following frame） ====================
//
// ── M 的定义（务必先读；2026-09-15 Danis 要求写清）────────────────────────
//
//   M 是一个**纯旋转**（旋转阵，用四元数承载；**不含平移**）。
//
//   语义 = 把「人体随动系(body)」的坐标换算到「骨架空间(world)」：
//
//         p_world = M · p_body
//
//   等价写法：  p_body = M⁻¹ · p_world = Mᵀ · p_world（M 是旋转 ⇒ 转置即逆）。
//   ⚠️ 若习惯写成「p_body = M·p_world」，那个 M 就是本函数 M 的**转置**（两者只差转置）。
//      本实现取 `p_world = M·p_body`，因为**构造时 M 的三列就是我们在 world 里算出的三根轴**：
//          M 第 0 列 = X_body（左）在 world 下的表达
//          M 第 1 列 = Y_body（上）在 world 下的表达
//          M 第 2 列 = Z_body（前）在 world 下的表达
//      即可验：RotateVector(M, (1,0,0)) == axis_left（同理 y/z；单测覆盖）。
//
// ── 三轴构造（Danis 2026-09-15 定；前提：rest ≈ T-pose + 资产统一 y-up）──
//
//   Y_body = world 的 +Y            （上；本函数**直接取固定 +Y**，不从骨算）
//   X_body = normalize( 去掉 Y 分量后的 (右腕 → 左腕) )   （左 = +X；命名已区分左右）
//   Z_body = X_body × Y_body        （前 = +Z；右手系下 left×up = forward）
//
//   ⇒ 因 Y_body ≡ world +Y，M 实际是一个**绕 Y 的 yaw**。
//     （若将来上方向要改成从骨算（Hips→Head），这里换成「三正交轴 → 四元数」的一般式即可，
//       本函数的返回结构与调用方都不用改。）
//
// ── 骨名与退化 ──────────────────────────────────────────────────────────
//   手腕优先（LeftHand/RightHand），缺则退到紧邻关节（LeftForeArm/RightForeArm）；
//   两侧都找不到、或连线**去 Y 后近零**（退化为沿 up 的线）⇒ `valid=false`（**不猜、不 fallback**）。

// 人体随动系的估计结果（含诊断字段，便于日志/面板显示）。
struct BodyFrame {
    bool valid = false;                                    // 估计失败（缺骨/退化）
    Quatf rotation = Quatf::Identity();                    // M：p_world = M · p_body
    std::string left_bone;                                 // 实际用于定 +X 的左骨名（诊断）
    std::string right_bone;                                // 右骨名
    Vec3f axis_left{1.0f, 0.0f, 0.0f};                     // world 下的三轴（诊断/可验）
    Vec3f axis_up{0.0f, 1.0f, 0.0f};
    Vec3f axis_forward{0.0f, 0.0f, 1.0f};
    float yaw_deg = 0.0f;                                  // M 绕 +Y 的转角（诊断）
};

// 退化阈值（米）：去 Y 后的腕连线短于此 ⇒ 视为退化（无法定左右向），返回 invalid。
// 取值依据：人形两腕间距 ≫ 0.1m；此值只用于挡「连线 ∥ up」的构造错误。
inline constexpr float kBodyFrameMinSpan = 1e-4f;

// 估计人体随动系（见上「M 的定义」）。
// Pre-condition: skeleton 非空且满足 SkeletonType::Validate()（违反 → LOG(FATAL)）。
inline BodyFrame EstimateBodyFrame(const SkeletonType& skeleton) {
    skeleton.Validate();
    BodyFrame f;
    const std::vector<Vec3f> pos = RestWorldPositions(skeleton);

    // 候选骨对：手腕优先，退到紧邻关节（前臂）。顺序即优先级。
    const std::pair<const char*, const char*> kPairs[] = {
        {"LeftHand", "RightHand"},
        {"LeftForeArm", "RightForeArm"},
    };
    for (const auto& pr : kPairs) {
        const int li = FindJointBySuffix(skeleton, pr.first);
        const int ri = FindJointBySuffix(skeleton, pr.second);
        if (li < 0 || ri < 0) {
            continue;  // 这一档缺骨，试下一档
        }
        // ⚠️ 方向铁律：「右腕 → 左腕」= `pos[左] − pos[右]`（A→B 的定义是 B−A）。
        //    这里曾写反（写成 右−左），被单测直接抓出（整个 M 白 180°）。
        const Vec3f d0 = pos[static_cast<size_t>(li)] - pos[static_cast<size_t>(ri)];
        const Vec3f d(d0.x(), 0.0f, d0.z());  // 去掉 Y 分量（正交化到 up = +Y）
        const float len = d.Norm();
        if (len < kBodyFrameMinSpan) {
            return f;  // 退化：连线沿 up，定不了左右向 ⇒ 不猜
        }
        f.axis_left = Vec3f(d.x() / len, 0.0f, d.z() / len);
        f.axis_up = Vec3f(0.0f, 1.0f, 0.0f);  // 固定 +Y（Danis 定）
        // Z = X × Y（右手系；left × up = forward）。
        f.axis_forward = Vec3f(f.axis_left.y() * f.axis_up.z() - f.axis_left.z() * f.axis_up.y(),
                               f.axis_left.z() * f.axis_up.x() - f.axis_left.x() * f.axis_up.z(),
                               f.axis_left.x() * f.axis_up.y() - f.axis_left.y() * f.axis_up.x());
        // M = 绕 +Y 的 yaw：X_body 已被归一化且 y==0 ⇒ θ = atan2(-z, x)。
        const float theta =
            std::atan2(-f.axis_left.z(), f.axis_left.x());
        f.yaw_deg = theta * 180.0f / static_cast<float>(M_PI);
        f.rotation = Quatf::FromAxisAngle(Vec3f(0.0f, 1.0f, 0.0f), theta).Normalized();
        f.left_bone = skeleton.joints[static_cast<size_t>(li)].name;
        f.right_bone = skeleton.joints[static_cast<size_t>(ri)].name;
        f.valid = true;
        return f;
    }
    return f;  // 两档骨名都缺 ⇒ invalid（不猜）
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

// ════════════════════════════════════════════════════════════════════════════
//  人体随动系版重定向（BodyRetarget）—— 本文件对外的唯一重定向 API
// ════════════════════════════════════════════════════════════════════════════
//
// 公式与不变量见**文件头**（⟨★⟩ / ⟨1⟩ / ⟨2⟩）；本节只声明接口。
//
// 一句话：把「关节相对 bind 的额外旋转」在**人体随动系**下的数值搬过去
//   Δ_t = Q_body · Δ_s · Q_body⁻¹,   Q_body = M_t · M_s⁻¹
// ⇒ 几何一致的两骨人**零误差**；几何朝向真不同也能正确对应。
//
// （历史：早先有 T 一套逐骨 `Q(j)=Rb_T·Rb_S⁻¹` 的实现，它把**几何看不见的 roll**
//   当成真差异共轭进动作 → roll 差 180° 时动作**镜像**。已删除，由本节取代。）

// 一次人体随动系重定向的**输入快照**：骨名对位 + 两侧骨架 + 两侧人体随动系 + Q_body。
// 整段动画建一次，之后每帧复用（不重复做骨名哈希）。
struct BodyRetargetPlan {
    // ── 两侧骨架（快照；自包含，不持外部引用）──
    SkeletonType target;  // 目标骨架（被驱动方）
    SkeletonType source;  // 源骨架（动作提供方）

    // ── 骨名对齐 ──
    std::vector<BoneMatch> matches;                  // 逐目标骨一条（含未命中）
    std::vector<int> source_of_target;               // 目标 index → 源 index；-1 = 未命中
    int matched_bone_count = 0;                      // 命中骨数
    int target_bone_count = 0;                       // 目标骨总数（“命中 x/y”的 y）
    std::vector<std::string> unmapped_target_bones;  // 未命中的目标骨名（无名骨不进表）

    // ── 两侧人体随动系（由几何定，见 EstimateBodyFrame）──
    BodyFrame source_frame;
    BodyFrame target_frame;

    // ── ★ 桥接量：Q_body = M_t · M_s⁻¹（唯一新增的量，一个整体旋转）──
    //   语义：∃  p_body = M⁻¹·p_world；Q_body 把“源的人体系”旋到“目标的人体系”。
    //   几何一致的两骨人 ⇒ Q_body = 恒等（→ 目标零误差复现源）。
    Quatf q_body = Quatf::Identity();
    float q_body_angle_deg = 0.0f;  // Q_body 的旋转角（诊断；= 两侧整体朝向差量级）

    // ── ★ 根位移尺度因子：root-motion 的 scale adaptation（2026-09-20）──
    //   root_offset_t = Q_body · root_offset_s · root_offset_scale
    //   取两侧 rest 时**【骨盆/腰】高度 y** 作比值（按骨名找 Hips，与骨名对位同一套凭据）：
    //     root_offset_scale = target_leg_length / source_leg_length
    //   ⚠️ 不能用 joints[0]：两套资产的"根骨"语义不同 —— FBX 源 joints[0]=Hips（y≈腰高），
    //      Tripo glb joints[0]='Root' 包装层（导出残差，高度≈0）。用骨名才一致。
    //   单位：无量纲比值（两侧若各自量法一致则单位自动约掉；本仓两侧都是米）。
    //   目的：源走了 d 米，目标（不同身高）走 d×ratio 米 —— "原来两脚站地上，
    //         retarget 后也站在地上"。粗糙但对走路/跑步类模糊动作够用
    //         （业界称 scale adaptation，精确踩点需 IK，见设计文档残余）。
    float source_leg_length = 0.0f;  // 源骨架 rest 骨盆高度 y
    float target_leg_length = 0.0f;  // 目标骨架 rest 骨盆高度 y
    float root_offset_scale = 1.0f;  // = target / source（退化时 FATAL，不猜）

    // ── 两侧 rest 世界朝向（Rb，与 joints 平行；算 P_t 与诊断用）──
    std::vector<Quatf> target_rest_world;
    std::vector<Quatf> source_rest_world;
};

// 建 plan：骨名对位 + 两侧人体随动系 + Q_body。整段动画建一次。
//
// Pre-condition（违反 → LOG(FATAL)，不 fallback）：
//   两侧骨架非空、满足 SkeletonType::Validate()；
//   两侧人体随动系都能估出（否则**拒绝**而不是给个错的结果——没 M 就没几何基准）。
inline BodyRetargetPlan BuildBodyRetargetPlan(const SkeletonType& target,
                                              const SkeletonType& source) {
    target.Validate();
    source.Validate();

    BodyRetargetPlan p;
    p.target = target;
    p.source = source;
    p.target_rest_world = RestWorldRotations(p.target);
    p.source_rest_world = RestWorldRotations(p.source);

    // 骨名对位（复用既有实现：同一份对位规则，不另起一套）。
    NameAlignment al = AlignBonesByName(p.target, p.source);
    p.matches = std::move(al.matches);
    p.source_of_target = std::move(al.source_of_target);
    p.matched_bone_count = al.matched;
    p.target_bone_count = p.target.bone_count();
    p.unmapped_target_bones = std::move(al.unmapped_target_bones);

    // 两侧人体随动系（几何基准）；估不出就 FATAL——静默用一个错的基准会产出错动作。
    p.source_frame = EstimateBodyFrame(p.source);
    p.target_frame = EstimateBodyFrame(p.target);
    LOG_IF(FATAL, !p.source_frame.valid)
        << "BuildBodyRetargetPlan: 源骨架估不出人体随动系（缺 Left/Right Hand 与 ForeArm，"
           "或腕连线退化沿 up）—— 重定向需要几何基准，不猜。";
    LOG_IF(FATAL, !p.target_frame.valid)
        << "BuildBodyRetargetPlan: 目标骨架估不出人体随动系（同上）。";

    // Q_body = M_t · M_s⁻¹（M 为纯旋转 ⇒ 逆 = 共轭）。
    p.q_body = (p.target_frame.rotation * p.source_frame.rotation.Conjugate()).Normalized();
    p.q_body_angle_deg = QuatAngleDeg(p.q_body, Quatf::Identity());

    // ── 根位移尺度因子（"腿长"比）：两侧 rest 时【骨盆/腰】高度的 y 之比 ──
    //   量法两侧必须一致。
    //
    //   ⚠️ 用哪根骨量（2026-09-20 实测结论）：**不能用 joints[0]**。
    //   两套资产的"根骨"语义不同：
    //     · FBX 源（Mixamo 官方）   ：joints[0] = mixamorig:Hips，其 rest y ≈ 腰高 ✓
    //     · Tripo glb（mixamo_male）：joints[0] = 'Root' 包装层，导出残差 ≈ (0,0,-0.0045)
    //                               —— 高度 **≈ 0**；真正的 Hips 是它的**子骨**（y≈0.534）
    //   所以直接取 joints[0].y 对 glb 会得 0（TPose 假设下这是数据事实，不是 bug）。
    //
    //   改用**按骨名找骨盆**（Hips），两侧同一语义骨 —— 与骨名对位（AlignBonesByName）
    //   同一套凭据。找不到时（如无骨名的极简测试骨架）退回 joints[0]，**并告警**（不静默）。
    auto pelvis_rest_height = [](const SkeletonType& skel) -> float {
        const int hips = FindJointBySuffix(skel, "Hips");  // 兼容 mixamorig:* 前缀
        if (hips < 0) {
            LOG(WARNING) << "BuildBodyRetargetPlan: 骨架里没有 *Hips 骨名，退回 joints[0]"
                            "量骨盆高度。若 joints[0] 是零长包装层（如 Tripo glb 的 Root），"
                            "这个比值会是错的。";
        }
        return RestWorldPositions(skel)[(hips >= 0) ? static_cast<size_t>(hips) : 0u].y();
    };
    {
        p.source_leg_length = pelvis_rest_height(p.source);
        p.target_leg_length = pelvis_rest_height(p.target);
        // 退化（骨盆高度 ≤ 0：非人形 / 数据错）⇒ 不猜、不 fallback。
        LOG_IF(FATAL, p.source_leg_length <= 0.0f)
            << "BuildBodyRetargetPlan: 源骨架 rest 骨盆高度 y=" << p.source_leg_length
            << " ≤ 0，无法建立 root-motion 尺度比（不猜）。";
        LOG_IF(FATAL, p.target_leg_length <= 0.0f)
            << "BuildBodyRetargetPlan: 目标骨架 rest 骨盆高度 y=" << p.target_leg_length
            << " ≤ 0，无法建立 root-motion 尺度比（不猜）。";
        p.root_offset_scale = p.target_leg_length / p.source_leg_length;
        LOG(INFO) << "BuildBodyRetargetPlan: 骨盆高度 src=" << p.source_leg_length
                  << " tgt=" << p.target_leg_length
                  << " ⇒ root_offset_scale=" << p.root_offset_scale
                  << " (Q_body=" << p.q_body_angle_deg << "°)";
    }
    return p;
}

// 把源的一帧位姿，按人体随动系不变式重定向到目标骨架（覆盖写 out）。
//
//   plan        : BuildBodyRetargetPlan 的结果（对位表 + 两侧骨架 + M + Q_body）。
//   source_pose : 源位姿（joint_rotation = 相对源 bind 的增量；为空 = 全恒等）。
//   out         : 输出位姿。bone_count/joint_rotation 尺寸 = 目标骨数；
//                 每骨为单位四元数、即 `P_t(j)`（相对**目标** bind 的增量，直接可喂烘焙）；
//                 root_offset = 源 root_offset 按 `Q_body · (leg_t/leg_s)` 换算到目标骨架
//                 （源为 0 ⇒ 0）。定义/单位见 interface/skeleton_types.h 的 root_offset。
//
// 逐帧代价：一次拓扑序扫（源 W）+ 一次拓扑序扫（目标 W_t 与 P_t），纯四元数乘。
//
// Pre-condition: out != nullptr；source_pose 尺寸与源骨数一致（违反 → LOG(FATAL)）。
inline void BodyRetargetPose(const BodyRetargetPlan& plan, const SkeletonPose& source_pose,
                             SkeletonPose* out /*output*/) {
    CHECK(out != nullptr) << "BodyRetargetPose: out 不能为空";
    const bool src_has_rot = !source_pose.joint_rotation.empty();
    if (src_has_rot) {
        CHECK_EQ(source_pose.joint_rotation.size(), plan.source.joints.size())
            << "BodyRetargetPose: source_pose 尺寸 " << source_pose.joint_rotation.size()
            << " 应 == 源骨架骨数 " << plan.source.joints.size();
    }

    const size_t n_src = plan.source.joints.size();
    const size_t n_dst = plan.target.joints.size();
    const Quatf& q_body = plan.q_body;
    const Quatf q_body_inv = q_body.Conjugate();  // 纯旋转 ⇒ 逆 = 共轭

    // ── 1) 源：世界总旋转 W_s(j) = W_s(parent) ⊗ b_s(j) ⊗ P_s(j)（拓扑序单趟）──
    std::vector<Quatf> src_world(n_src);
    for (size_t j = 0; j < n_src; ++j) {
        const Quatf b = plan.source.bind_rotation.empty() ? Quatf::Identity()
                                                          : plan.source.bind_rotation[j];
        const Quatf pose = src_has_rot ? source_pose.joint_rotation[j] : Quatf::Identity();
        const Quatf local = (b * pose).Normalized();
        const int par = plan.source.joints[j].parent;
        src_world[j] = (par == kSkeletonNoParent)
                           ? local
                           : (src_world[static_cast<size_t>(par)] * local).Normalized();
    }

    // ── 2) 目标：按 ⟨1⟩ 得 W_t(j)，再按 ⟨2⟩ 反解 P_t(j)。全程拓扑序。──
    out->bone_count = static_cast<int>(n_dst);
    out->joint_rotation.resize(n_dst);

    // ── 3) 根位移（root-motion）缩放搬运（2026-09-20 接线）──
    //   root_offset_t = Q_body · root_offset_s · (leg_t / leg_s)
    //   · 尺度因子：把源的位移从【源骨架尺度】换算到【目标骨架尺度】（scale adaptation）。
    //   · Q_body：位移方向随角色朝向一起转 —— 源朝 +Z / 目标朝 +X 时不会"往前走变往侧面滑"。
    //   · 源为 0（纯原地动作）⇒ 输出 0（向后兼容，既有 gold 零回归）。
    out->root_offset = geom::RotateVector(q_body, source_pose.root_offset) * plan.root_offset_scale;

    std::vector<Quatf> dst_world(n_dst);
    for (size_t j = 0; j < n_dst; ++j) {
        const Quatf b = plan.target.bind_rotation.empty() ? Quatf::Identity()
                                                          : plan.target.bind_rotation[j];
        const int par = plan.target.joints[j].parent;
        CHECK(par == kSkeletonNoParent || par < static_cast<int>(j))
            << "BodyRetargetPose: 目标骨架非拓扑序 joint[" << j << "] parent=" << par;
        const Quatf parent_world =
            (par == kSkeletonNoParent) ? Quatf::Identity()
                                       : dst_world[static_cast<size_t>(par)];

        const int s = plan.source_of_target[j];
        if (s < 0) {
            // 未命中 → 保持自身 rest（P = 恒等，W = 父世界 ⊗ b）。
            out->joint_rotation[j] = Quatf::Identity();
            dst_world[j] = (parent_world * b).Normalized();
            continue;
        }

        const size_t si = static_cast<size_t>(s);
        // ⟨1⟩ W_t(j) = Q_body · W_s(j) · Rb_s(j)⁻¹ · Q_body⁻¹ · Rb_t(j)
        const Quatf w_t = (q_body * src_world[si] *
                           plan.source_rest_world[si].Conjugate() * q_body_inv *
                           plan.target_rest_world[j])
                              .Normalized();
        // ⟨2⟩ P_t(j) = b_t(j)⁻¹ · W_t(parent)⁻¹ · W_t(j)
        out->joint_rotation[j] =
            (b.Conjugate() * parent_world.Conjugate() * w_t).Normalized();
        dst_world[j] = w_t;
    }
}

// 便利入口：整段源位姿一次性重定向（= 建 plan 一次 + 逐帧 BodyRetargetPose）。
inline std::vector<SkeletonPose> BodyRetargetPoses(
    const SkeletonType& target, const SkeletonType& source,
    const std::vector<SkeletonPose>& source_poses) {
    const BodyRetargetPlan plan = BuildBodyRetargetPlan(target, source);
    std::vector<SkeletonPose> out;
    out.reserve(source_poses.size());
    for (const SkeletonPose& src : source_poses) {
        SkeletonPose dst;
        BodyRetargetPose(plan, src, &dst);
        out.push_back(std::move(dst));
    }
    return out;
}

// 便利入口：建 plan + 重定向一帧（要复用 plan 请直接用上面两个）。
inline SkeletonPose BodyRetargetOnePose(const SkeletonType& target,
                                       const SkeletonType& source,
                                       const SkeletonPose& source_pose) {
    const BodyRetargetPlan plan = BuildBodyRetargetPlan(target, source);
    SkeletonPose dst;
    BodyRetargetPose(plan, source_pose, &dst);
    return dst;
}

}  // namespace jpov

#endif  // JPOV_INTERFACE_SKELETON_RETARGET_H_
