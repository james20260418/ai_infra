// JPOV Skeleton — 骨架蒙皮子系统：CPU 侧数据定义（GL-free）
//
// 本文件是「骨架批量蒙皮 / instancing」（架构文档 docs/jpov_crowd_instancing_arch.md
// §6.2-B 骨骼动画纹理）子系统的 CPU/interface 数据层。
//
// 核心概念（v3，2026-09-07 与 Danis 收敛）：只有 **Pose**，没有 Clock/动画的概念流。
//   - 骨骼动画纹理 = 一块「散装 Pose 关键帧」(pose atlas)：把若干**静态姿态关键帧**解算成
//     每骨架每关节 JointMatrix 后平铺进一张 RGBA 纹理。纹理的“行/列”只表达存放布局，
//     不表达时间语义 —— 它不要求 pose 相邻、不区分哪段动作，就是一仓库的单帧位姿。
//   - “动画”/“一段动作” = 用户自选的一组 pose 的**顺序＋推进**：走就是一个 12 帧的数组、
//     跳是另一个 60 帧的数组…… 这些是**用户自己维护的列表**，JPOV 不管播放、不管时间轴。
//   - 渲染端实例只做一件事：**在骨骼纹理里取两个 pose 的 JointMatrix，两者之间逐骨插值**，
//     拿插值结果蒙皮。state 唯一接口 = {pose_a, pose_b, ratio}（见 SkinnedInstanceState）。
//
// 物理链路：CPU 把每个 pose 沿骨架树拓扑解算出每关节相对角色根的 JointMatrix →
// 按 pose 平铺成骨骼动画纹理；运行时实例送 {pose_a, pose_b, ratio}，蒙皮 VS 查这两个
// pose 的 mat4、逐骨 lerp 后套 4-bone 蒙皮。（详见 src/skeleton/）
//
// 约束（铁律）：**同一份骨架的 pose 之间才能插值**。pose 强绑骨架：不同骨架 = 不同
// 骨骼纹理/骨数量/拓扑，跨骨架插值 = 读两张 GL 纹理、语义也无从谈起 —— 因此插值永远
// 发生在“同一种骨架”的 atlas 内。一个实例的 pose_a/pose_b 必须同属一种骨架。
//
// 职责边界（沿用 interface/(CPU/用户类型) vs src/(GL/manager) 分离，同 mesh.h/gpumesh.h、
// interface/render_command.h vs src/object3d/object3d_renderer.h）：
//   - 本文件只声明 CPU 侧、GL-free 的骨架定义 / pose 关键帧 / 运行时实例状态；
//   - GPU 资源上传(逆绑定矩阵 constant、pose 烘焙成骨骼纹理、instancing)全部在
//     src/skeleton/skeleton_manager.h。
//   - rest-mesh 上传复用现有 MeshManager/GPUMesh（VAO 已按属性分 VBO；joint loc3 /
//     weight loc4 预留为 skinned 每顶点权重输入，见 gpumesh.h/mesh_manager.h），
//     S0 直接 reuse GPUMesh，不新造 mesh 上传管线。

#ifndef JPOV_INTERFACE_SKELETON_TYPES_H_
#define JPOV_INTERFACE_SKELETON_TYPES_H_

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <glog/logging.h>
#include "geom/common/quaternion.h"
#include "geom/common/vec.h"
#include "geom/math/mat4.h"

namespace jpov {

// 类型别名：复用 geom 向量（与 camera.h / mesh.h 一致）
using Vec3f = geom::Vec3<float>;

// 根关节的父索引哨兵（SkeletonJoint::parent）
inline constexpr int kSkeletonNoParent = -1;

// ==================== 骨架关节（一棵有根树的节点） ====================

// 骨架中一个关节（一根骨/树的节点）。每个关节一个父（根的父为 kSkeletonNoParent），
// 组成骨架树。建模为"链式根树 + rest 平移 + bind 朝向"，覆盖人群低模/直链走姿与标准人形 rig。
struct SkeletonJoint {
    int   parent = kSkeletonNoParent;  // 父关节索引（= joints 中某 id）。根 = kSkeletonNoParent。
    Vec3f rest_offset;                 // bind 姿态下相对父关节的**局部平移**，描述骨架树形状。
    // 关节名（如 "mixamorig:LeftArm"）。用于把外部来源（FBX loader / Mixamo 动画）与
    // 本骨架做骨名对位 / sanity 对比（是否同一种骨架、骨对不对得上）。可为空（程序化骨架）。
    std::string name;
    //
    // 注：本骨的 **bind 朝向** 不在这里，而在 SkeletonType::bind_rotation（按关节 index 平行
    //   一份）。理由："朝向"是骨架级数据（要有 bind_rotation 齐备才能说是骨架）；把两个字段
    //   平行放同一容器，便于一起校验尺寸、一起遍历，也便于将来单独替换其一。
};

// ==================== 骨架定义（SkeletonType） ====================

// 一份有根骨架(人形/马形…)的 CPU 描述，定义“一种骨架”。它是骨架资源在 CPU 侧的类型：
// SkeletonManager 构造时绑定一个 SkeletonType + 一整套 pose，上传成可渲染资源
// （逆绑定 constant、骨骼动画纹理）。
// 人/马是各自的 SkeletonType → 各自的 SkeletonManager；骨架与物种无关是关键（见顶部注释，
// 以及 src/skeleton/skeleton_manager.h）。
//
// 骨架的完整定义 = **树 + 骨长(rest_offset) + bind 朝向(bind_rotation)**，三者齐备即"健全"：
//   - joints[].parent / rest_offset  → 骨架树的形状与各骨长度
//   - bind_rotation[]               → 各骨在 bind 姿态下"朝哪长"（相对父的旋转）
//   pose 全恒等时，沿树复合 T(rest_offset)·R(bind_rotation) 得到的即该骨架的标准 T-pose。
//
// ⚠️ **inverse_bind 不是字段**（2026-09-11 定，Danis）：它是「joints + bind_rotation」的**派生量**
//   （`inverse_bind[j] = JW_bind[j]⁻¹`），输入定了它就定了。留作字段会出现"输入 + 输入的导出物"
//   两份数据打架，违反 Minimal Surprise。需要时用 ComputeInverseBind() 现算（可选带缓存）。
//
// ⚠️ 对外部资产（Tripo glb / Mixamo FBX）有一条 IMPORTANT：`bind_rotation` 应取**资产自带的**
//   node/joint rest 旋转（它和资产网格的 `POSITION` 是配套长出来的），**不要用别处推的值覆盖**。
//   程序化骨架（如 Mixamo23Skeleton）则天然自洽，直接用工厂给的表。
struct SkeletonType {
    std::vector<SkeletonJoint> joints;   // 0 号应为根；需满足拓扑序(每个 non-root 的 parent<自身)

    // 每关节的 **bind 朝向**（相对父关节的旋转，四元数）。索引与 joints 对齐。
    // 语义：jointLocal(j) = T(rest_offset[j]) · R(bind_rotation[j]) · R(pose.joint_rotation[j])。
    //   pose 恒等 → jointLocal = T(rest_offset)·R(bind_rotation) = 该骨在 T-pose 下的局部变换。
    // ⚠️ bind_rotation 通常**不是恒等**：正是它把骨长轴从局部 +Y 掰到"实际朝向"（如手臂掰水平）。
    // 空 = 全恒等（"骨长即朝向"的极简直链骨架；也兼容老数据）。非空须 size == joints.size()。
    std::vector<geom::Quaternion<float>> bind_rotation;

    int bone_count() const { return static_cast<int>(joints.size()); }

    // 校验：joints 非空、0 为根、每 parent 索引合法且在拓扑序早于自身；
    // bind_rotation 若非空须尺寸 == joints.size()。非法 LOG(FATAL)，不 fallback。
    void Validate() const;

    // 派生：算每关节的 inverse bind（相对"骨架空间"原点的逆绑定矩阵）。
    //   JW_bind[j] = JW_bind[parent] · T(rest_offset[j]) · R(bind_rotation[j])   （pose 恒等）
    //   inverse_bind[j] = JW_bind[j]⁻¹                                          （仿射求逆）
    // 返回：joints.size() 个列主序 float[16]（与 GLSL mat4 内存布局一致；与骨骼纹理烘焙配套）。
    // 用途：SkeletonManager 烘焙 pose atlas 时把 inverse_bind 折入（方案甲）。
    //   **外部资产**应优先用资产自带 IBM（经共轭），本函数用于**程序化骨架**（自算天然正确）；
    //   详见 docs/jpov_retarget_design.md §5.5。
    std::vector<std::array<float, 16>> ComputeInverseBind() const;
};

// 说明（mesh 绑定位置）：骨架定义（SkeletonType）本身上不挂 rest mesh。蒙皮的 rest 几何由调用方以
// mesh_id 直接引用现有 GPUMesh（DrawMeshWithSkeleton / SkinnedMeshCommand），非本层
// 责任 —— 复用 MeshManager 上传即可，不在这里另起 part-pool/mesh 池。

// ==================== 骨骼姿态关键帧（单个 Pose） ====================

// 一份骨架的**单个静态位姿关键帧**（pose）：对骨架每根骨的一个姿态。本类型是“关键帧”的
// 最小原子 —— 用户在构造 SkeletonManager 时把【一整套】pose 一并传入（见 skeleton_manager.h），
// SkeletonManager 据此把每 pose 解算、烘焙成骨骼动画纹理的一行（pose 在 vector 的下标即它的
// atlas 行 / 运行时 pose_a/pose_b 引用）。一段“动作”仅是用户自选的一组 pose 的数组（见文件头），
// JPOV 不在此表达“哪几帧连成一个动作”。
//
// ⚠️ 渲染端插值/蒙皮的只是“同一种骨架”内两个 pose 的 JointMatrix（见文件头铁律）。
//   pose 与骨架**强绑定**：一个 pose 严格属于某一种骨架（骨数量/树拓扑一致），否则无法解算
//   也不能插值。因此 pose 从不单独注册/分发：它只作为构造时整包的一部分落在那一份
//   SkeletonManager 里，天然不跨骨架。归属见 skeleton_manager.h。
//
// 数据表示取舍（定稿 2026-09-08）：SkeletonPose 是**用户资产层**（要可读、可手写、可 retarget），
//   因此收**每关节旋转（四元素，相对父）**，由 CPU 烘焙端沿骨架树解算出相对根 JointMatrix 后
//   落 atlas（skeleton_manager.h 负责烘焙，本文件只定义资产格式）。一个 pose 严格从属某一份骨架
//   （joint_rotation.size() == 该骨架 bone_count），每个顶点的 rest 平移由 joints tree 提供，
//   pose 只驱动【旋转】；若动画/root-motion 带整体位移则由 root_offset 承载（纯原地动作默认为 0）。
struct SkeletonPose {
    int bone_count = 0;   // 应 == 所用 SkeletonType::bone_count（同一种骨架）。

    // 每关节相对其父关节的**旋转**（四元数，需整单位。索引与 SkeletonType::joints 对齐）。
    // joint_rotation[i] = 从父关节系转到本关节系的 local 旋转(相对父)。
    // 应为 size == bone_count（若含根且根无旋转可用 identity）。为空=走 rest(隐式全单位)。
    // 非空但 size != bone_count 视为非法（烘焙端 LOG(FATAL)）。
    std::vector<geom::Quaternion<float>> joint_rotation;

    // 根(0 号关节)相对“角色原点”的位移 / root motion（通常 0）。纯原地动作/静态 pose 保持默认。
    Vec3f root_offset{0.0f, 0.0f, 0.0f};

    // 全恒等 pose（每关节旋转 = identity，root_offset = 0）。
    // 语义（配合骨架）：当 SkeletonType 的 bind_rotation 是标准人形 bind 朝向时，
    //   本 pose 驱动出的姿态 = 该骨架的 **T-pose**（因为 bind 朝向已烘进骨架，pose 不再叠旋转）。
    // 用途：Mixamo23Skeleton 的配套静态姿势（T-pose）；也是蒙皮"静态退化门"的基准 pose。
    static SkeletonPose Identity(int bone_count);
};

// ==================== 运行时实例状态 ====================

// 单个蒙皮实例的运行时状态：渲染按 (同 mesh + 同 skeleton) 把一批实例 instanced draw，
// 实例之间只差这份薄状态；渲染时作为 per-instance attribute 上传(glVertexAttribDivisor)。
//   这是一条"如何画出看得见的这一份"的 description(而非自己背整份几何)。
struct SkinnedInstanceState {
    // 模型摆放 —— 复用 Object3DCommand 变换约定(center 平移 + up/front 旋转 + scale)。
    Vec3f center;            // 角色根(骨盆/原点)在某物体坐标系下的世界平移
    Vec3f up{0.0f, 1.0f, 0.0f};        // 局部 +Y → 世界 up(会被归一化)
    Vec3f front{0.0f, 0.0f, 1.0f};     // 局部 +Z → 世界 front(会被归一化)
    float scale = 1.0f;     // 整体缩放(先缩顶点再转+平移)，S0 只做全局/轴向 scale(§6.1)

    // ---- 运动：一份骨架内两个 pose 之间的插值（唯一接口）----
    // 唯一表达动画顶点的字段就是这个三元组：VS 取 pose_a/pose_b 两套 JointMatrix，
    //   按 ratio ∈ [0,1] 逐骨 lerp，得本实例这一帧的最终骨骼姿态后蒙皮。
    // ratio==0 → 完全 pose_a；==1 → 完全 pose_b；中间=两者平滑过渡。
    // 连续动画 = 用户在相邻姿态对之间推进该三元组(自己记数组/自己走时间)——JPOV 不做播放：
    //   例：走＝把 12 个 pose 排成 a0,a1…a11，逐帧发 (a_{k},a_{k+1},t) 推进 k/t。
    // pose_a/pose_b 是【构造该骨架的 SkeletonManager 时传入的 pose 数组下标】(0-based)。
    //   要“静态”就让 pose_a==pose_b==那一姿态。索引越界 → 实现应 LOG(FATAL)/批次剔除。
    int    pose_a = 0;       // 插值起点：SkeletonManager pose 数组下标（0-based）。
    int    pose_b = 0;       // 插值终点：同上；==pose_a 时无插值(=pose_a 静态)。
    float  ratio = 0.0f;     // [0,1] pose_a→pose_b 的权重。
    //
    // 约束：pose_a / pose_b 是同一个 SkeletonManager(同一种骨架) 的 pose 下标；不同骨架(
    //   不同 SkeletonManager)严禁放同实例混插 —— 语义无意义且要读两张骨骼纹理。见本文件顶铁律。

    // 外观 select：==架构 doc §3== 换外观=换索引/材质变体(非换几何)。S1 才用。
    // S0 全低模统一外观，占位常 0；将来换服饰/肤=在此给 baseColor 变体/texture-array index。
    // TODO(2026-09-06): 动态 per-instance 颜色通道(B 决策)延后，S0 不建。
    uint32_t appearance_index = 0;
};

// ==================== Validate / 派生量 定义 ====================

inline void SkeletonType::Validate() const {
    // 校验：joints 非空、parent 索引合法(不在自身/越界)、每个关节父早于自身(拓扑序);
    // bind_rotation 若非空必须尺寸 == joints.size()。非法 LOG(FATAL)，绝不 fallback。
    CHECK(!joints.empty())
        << "SkeletonType::Validate: joints 不能为空（缺一种骨架定义）";
    const size_t n = joints.size();
    for (size_t i = 0; i < n; ++i) {
        const int p = joints[i].parent;
        if (p != kSkeletonNoParent) {
            CHECK_GE(p, 0) << "SkeletonType::Validate: joint[" << i << "].parent="
                           << p << " 非法(parent<0 只能用 kSkeletonNoParent)";
            CHECK_LT(p, static_cast<int>(n))
                << "SkeletonType::Validate: joint[" << i
                << "].parent 越界=" << p;
            CHECK_NE(p, static_cast<int>(i))
                << "SkeletonType::Validate: joint[" << i
                << "] 不能是自己的父";
            CHECK_LT(p, static_cast<int>(i))
                << "SkeletonType::Validate: joint[" << i << "].parent=" << p
                << " 不满足拓扑序(须 < " << i << ", 否则树在容器里乱序)";
        }
    }
    if (!bind_rotation.empty()) {
        CHECK_EQ(bind_rotation.size(), n)
            << "SkeletonType::Validate: bind_rotation 尺寸 " << bind_rotation.size()
            << " 应 == joints " << n;
    }
}

inline SkeletonPose SkeletonPose::Identity(int bone_count) {
    CHECK_GT(bone_count, 0) << "SkeletonPose::Identity: bone_count 必须 >0";
    SkeletonPose pose;
    pose.bone_count = bone_count;
    // 显式给满 bone_count 个恒等旋转（而非留空走隐式 rest）—— 语义更明确、尺寸可校验。
    pose.joint_rotation.assign(static_cast<size_t>(bone_count),
                               geom::Quaternion<float>::Identity());
    return pose;
}

inline std::vector<std::array<float, 16>> SkeletonType::ComputeInverseBind() const {
    using geom::math::Mat4;
    Validate();
    const size_t n = joints.size();

    // 1) 沿拓扑序复合出 bind 姿态（pose 恒等）下每关节的 jointWorld（相对骨架空间原点）。
    //    JW_bind[j] = JW_bind[parent] · T(rest_offset[j]) · R(bind_rotation[j])；根则无父。
    std::vector<Mat4> jw(n);
    for (size_t j = 0; j < n; ++j) {
        const geom::Quaternion<float> bind =
            bind_rotation.empty() ? geom::Quaternion<float>::Identity()
                                  : bind_rotation[j];
        const geom::math::Mat4 local =
            geom::math::JointLocalRest(joints[j].rest_offset, bind);
        const int p = joints[j].parent;
        if (p == kSkeletonNoParent) {
            jw[j] = local;
        } else {
            // Validate() 已保证拓扑序（parent < j），此处只需再确认一次下界（防御）。
            CHECK_GE(p, 0);
            jw[j] = geom::math::Mat4Mul(jw[static_cast<size_t>(p)], local);
        }
    }

    // 2) inverse_bind[j] = JW_bind[j]⁻¹（仿射求逆；含缩放也支持）。
    std::vector<std::array<float, 16>> out(n);
    for (size_t j = 0; j < n; ++j) {
        const geom::math::Mat4 inv = geom::math::Mat4InverseAffine(jw[j]);
        for (int k = 0; k < 16; ++k) {
            out[j][k] = inv.m[k];
        }
    }
    return out;
}

}  // namespace jpov

#endif  // JPOV_INTERFACE_SKELETON_TYPES_H_
