// JPOV Skeleton — 骨架蒙皮子系统：CPU 侧数据定义（GL-free）
//
// 本文件是「骨架批量蒙皮 / instancing」（架构文档 docs/jpov_crowd_instancing_arch.md
// §6.2-B 骨骼动画纹理）子系统的 CPU/interface 数据层。
// 物理链路：CPU 把每根骨头 JointMatrix 按 clip→帧烘焙成一张 RGBA 纹理（骨骼动画纹理）；
// 运行时 instance 只送帧号/相位，蒙皮 VS 查表 + 保留 4-bone 蒙皮。（详见 src/skeleton/）
//
// 职责边界（沿用 interface/(CPU/用户类型) vs src/(GL/manager) 分离，同 mesh.h/gpumesh.h、
// interface/render_command.h vs src/object3d/object3d_renderer.h）：
//   - 本文件只声明 CPU 侧、GL-free 的骨架模板 / 动画 clip / 运行时实例状态；
//   - GPU 资源上传(逆绑定矩阵 constant、动画烘焙成骨骼纹理、instancing)全部在
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

// ==================== 骨架模板 ====================

// 一份有根骨架(人形/马形…)的 CPU 描述，是"骨架资源"的可配置模板。—— 谁用它谁喂一份。
//   SkeletonManager 把任意 template 上传成可渲染资源（逆绑定 constant、动画烘焙）。
//   人/马只是各自一份 template → SkeletonManager 多实例；骨架与物种无关是关键(见顶部注释
//   "多骨架=多实例/多 manager"，见 src/skeleton/skeleton_manager.h)。
struct SkeletonTemplate {
    std::vector<SkeletonJoint> joints;   // 0 号应为根；需满足拓扑序(每个 non-root 的 parent<自身)

    // 每关节相对"角色局部原点"的逆绑定矩阵(inverse bind)：rest 静止时的逆，随骨架 constant。
    //   蒙皮 = JointMatrix(bone, frame动画) · inverse_bind(bone) 作用 rest 顶点。
    //   空 = SkeletonManager 按 template 链式 rest 自算；否则用显式值。
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

// 说明（mesh 绑定位置）：骨架模板本身上不挂 rest mesh。蒙皮的 rest 几何由调用方以
// mesh_id 直接引用现有 GPUMesh（DrawMeshWithSkeleton / SkinnedMeshCommand），非本层
// 责任 —— 复用 MeshManager 上传即可，不在这里另起 part-pool/mesh 池。

// ==================== 骨骼动画 clip（CPU 动画源，供烘焙） ====================

// 一段可被烘焙成"骨骼动画纹理"的动作 clip 的 CPU 描述。
//   人群动画走离线/烘焙：CPU 一次性把每帧解成每骨头 JointMatrix，写入骨骼纹理；
//   运行期实例只送相位/所在 clip（GPU 查纹理）。S0 CPU 端不做逐角色实时解算 pose。
//   clip 与物种解耦 —— 只描述"某段时间动作怎么让人/马摆"，绑 SkeletonTemplate 一起烘焙。
//
// TODO(2026-09-06): clip 的 CPU 存法先不定义死，先给帧元数据：
//   (a) 逐帧已解算 JointMatrix(每帧 bone_count×16 float)；还是
//   (b) 原始每关节旋转(四元素)+ CPU 烘焙端边走树边解。
//   人群 S0 用"离线 bake→GPU 查纹理"，偏好 (a)：烘焙端直接给 mat4 场。
//   真正数据容器(给整份 clip 的内存家)归属动画源/骨架侧，本类型只放帧元数据占位，
//   免得把 CPU 动画源布局在骨架 framework 写死。
struct SkeletonClip {
    float duration_seconds = 0.0f;  // 动画时长(秒)，须 >0
    int   frame_count = 0;          // 烘焙帧数(= 骨骼纹理高度)，须 >0
    int   bone_count = 0;           // 应与所用 SkeletonTemplate.bone_count 一致
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

    // 运动信息：本实例用哪个 clip 的哪个相位。
    //   clip_id：骨骼动画 clip 的 id（由 SkeletonManager 登记；视实现：0 = rest/静态）。
    //   phase  ：clip 内相位 [0,1]。VS 由 phase 得到前后两动画帧，查骨骼纹理并对两帧
    //            JointMatrix lerp(插值单位是矩阵不是角度,便宜且两帧接近时误差可忽略)。
    //   TODO(2026-09-06): 跨 clip 动作混合(S1: clipA/clipB + blend_t)在此不落结构，等 S1。
    int    clip_id = 0;
    float  phase = 0.0f;

    // 外观 select：==架构 doc §3== 换外观=换索引/材质变体(非换几何)。S1 才用。
    // S0 全低模统一外观，占位常 0；将来换服饰/肤=在此给 baseColor 变体/texture-array index。
    // TODO(2026-09-06): 动态 per-instance 颜色通道(B 决策)延后，S0 不建。
    uint32_t appearance_index = 0;
};

// ==================== Validate 声明 ====================

inline void SkeletonTemplate::Validate() const {
    // 见 struct 注释；TODO(2026-09-06): 实现阶段补完整校验(log(FATAL) on illegal)，
    // 参照 mesh.h MeshData::Validate() 风格 crash——绝不 fallback 隐藏非法输入。
}

}  // namespace jpov

#endif  // JPOV_INTERFACE_SKELETON_TYPES_H_
