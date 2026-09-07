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
#include <vector>

#include "geom/common/vec.h"

namespace jpov {

// 类型别名：复用 geom 向量（与 camera.h / mesh.h 一致）
using Vec3f = geom::Vec3<float>;

// 根关节的父索引哨兵（SkeletonJoint::parent）
inline constexpr int kSkeletonNoParent = -1;

// ==================== 骨架关节（一棵有根树的节点） ====================

// 骨架中一个关节（一根骨/树的节点）。每个关节一个父（根的父为 kSkeletonNoParent），
// 组成骨架树。S0 建模为"链式根树 + rest 平移"，够覆盖人群低模/直链走姿的立正/侧摆。
struct SkeletonJoint {
    int   parent = kSkeletonNoParent;  // 父关节索引（= joints 中某 id）。根 = kSkeletonNoParent。
    Vec3f rest_offset;                 // rest(bind) 姿态下相对父关节的**局部平移**，描述骨架树形状。
    //
    // TODO(2026-09-06): rest 目前只建模平移，未含每关节 rest 朝向(SO(3)/四元素)。S0 人形
    //   通常只需把"每部位 mesh 绑到根/直链主轴"即可；要表达更真实骨骼(肩髋球窝、每骨头 rest
    //   朝向)给每关节加 rest 旋转，待真实 rig 进来自主扩展，勿现在铺。
};

// ==================== 骨架定义（SkeletonType） ====================

// 一份有根骨架(人形/马形…)的 CPU 描述，定义“一种骨架”。它是骨架资源在 CPU 侧的类型：
// SkeletonManager 构造时绑定一个 SkeletonType + 一整套 pose，上传成可渲染资源
// （逆绑定 constant、骨骼动画纹理）。
// 人/马是各自的 SkeletonType → 各自的 SkeletonManager；骨架与物种无关是关键（见顶部注释，
// 以及 src/skeleton/skeleton_manager.h）。
struct SkeletonType {
    std::vector<SkeletonJoint> joints;   // 0 号应为根；需满足拓扑序(每个 non-root 的 parent<自身)

    // 每关节相对"角色局部原点"的逆绑定矩阵(inverse bind)：rest 静止时的逆，随骨架 constant。
    //   蒙皮 = JointMatrix(bone, 该实例最终插值出的 pose) · inverse_bind(bone) 作用 rest 顶点。
    //   空 = SkeletonManager 按 SkeletonType 链式 rest 自算；否则用显式值。
    //
    // ⚠️ 代码库无 Mat4 类型，矩阵以 float[16] 列主序传（同 Object3D float[16] 约定）。
    //   每关节一个阵。TODO(2026-09-06): 用 std::array<float,16> 还是手写 4x4 小结构待定；
    //   与 mesh.h 用 std::array 保证可拷贝一致，先按 flat float[16] 占位，实现时配 4x4 工具。
    // TODO(2026-09-06): 首个实现若人形确实是"直链 rest"(每关节只沿某轴偏)，可由树自动推
    //   inverse_bind 而不必让用户喂满阵，此字段保留为可选显式覆盖。
    std::vector<std::array<float, 16>> inverse_bind;  // 需 <array>

    int bone_count() const { return static_cast<int>(joints.size()); }

    // 校验：joints 非空、0 为根、每 parent 索引合法且在拓扑序早于自身；
    // 若 inverse_bind 非空须尺寸==joints.size()。非法 LOG(FATAL)。
    // TODO(2026-09-06): 实现阶段按 mesh.h MeshData::Validate() 风格补全。
    void Validate() const;
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
// 数据表示取舍（2026-09-07 待点选，point4）：SkeletonPose 是**用户资产层**（要可读、可手写、
// 可 retarget），理应收**每关节旋转**（四元素/欧拉，相对父链，而非整份矩阵）—— 由 CPU 烘焙端
//   沿骨架树解算出相对根 JointMatrix 后落 atlas。但精确字节布局（四元素 vs 欧拉、有无根位移）
//   尚未定，framework 阶段不把它锁死，等实现 PR 连四元素工具一起给完整容器。
//   本成员仅立 meta；真正每关节姿态数据容器见下方 TODO。
struct SkeletonPose {
    int bone_count = 0;   // 应 == 所用 SkeletonType::bone_count（同一种骨架）。
    //
    // TODO(2026-09-07): 每关节位姿数据容器（旋转 + 根位移），用户层可读、烘焙端转 mat4；
    //   见 struct 注释数据表示取舍。框架阶段不锁格式，待实现 PR（配四元素/欧拉→mat4 工具）。
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

// ==================== Validate 声明 ====================

inline void SkeletonType::Validate() const {
    // 见 struct 注释；TODO(2026-09-06): 实现阶段补完整校验(log(FATAL) on illegal)，
    // 参照 mesh.h MeshData::Validate() 风格 crash——绝不 fallback 隐藏非法输入。
}

}  // namespace jpov

#endif  // JPOV_INTERFACE_SKELETON_TYPES_H_
