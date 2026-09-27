// JPOV Skeleton — 骨架蒙皮子系统：CPU 侧数据定义（GL-free）
//
// 本文件是「骨架批量蒙皮 / instancing」（架构文档 docs/jpov_crowd_instancing_arch.md
// §6.2-B 骨骼动画纹理）子系统的 CPU/interface 数据层。
//
// 核心概念（v3，2026-09-07 与 Danis 收敛）：只有 **Pose**，没有 Clock/动画的概念流。
//   - 骨骼动画纹理 = 一块「散装 Pose 关键帧」(pose atlas)：把若干**静态姿态关键帧**解算成
//     每骨架每关节的**蒙皮变换**（对偶四元数，见 geom/math/dual_quat.h）后平铺进一张 RGBA
//     纹理。纹理的“行/列”只表达存放布局，
//     不表达时间语义 —— 它不要求 pose 相邻、不区分哪段动作，就是一仓库的单帧位姿。
//   - “动画”/“一段动作” = 用户自选的一组 pose 的**顺序＋推进**：走就是一个 12 帧的数组、
//     跳是另一个 60 帧的数组…… 这些是**用户自己维护的列表**，JPOV 不管播放、不管时间轴。
//   - 渲染端实例只做一件事：**在骨骼纹理里取两个 pose 的蒙皮变换，两者之间逐骨插值**，
//     拿插值结果蒙皮。state 唯一接口 = {pose_a, pose_b, ratio}（见 SkinnedInstanceState）。
//
// 物理链路：CPU 把每个 pose 沿骨架树拓扑解算出每关节相对角色根的 JointMatrix →
// 折入 inverse_bind 得刚体蒙皮变换 → 转成**对偶四元数**按 pose 平铺成骨骼动画纹理；
// 运行时实例送 {pose_a, pose_b, ratio}，蒙皮 VS 取这两个
// pose 的对偶四元数、逐骨插值（NLERP）后套 4-bone 刚体混合蒙皮（DLB）。（详见 src/skeleton/）
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

// ==================== 部位粗细（thickness scale） ====================
//
// 用途：给同一份共享骨架的实例补一条**个体差异**来源 —— 「胖瘦 / 部位粗细」，不新增
//   顶点属性、不改骨架拓扑、不需要重烘 pose atlas。
// 机制：蒙皮**之前**，把顶点相对关节的偏移转到「该骨的局部坐标系」里，只缩**横截面**
//   （垂直于骨长轴的两个方向），不缩沿轴长度 ⇒ 视觉上 = 「这根骨所辖的肉变粗/变细」；
//   之后照常走既有 DQS 蒙皮（公式一个字不改）。设计与推导见
//   docs/jpov_crowd_body_shape_face_design.md §3（尤其 §3.1 公式 / §3.2 为什么作用在 rest
//   顶点上 / §3.3 与 DQS 正交 / §3.3-4 零回归只能靠开关）。
//
// **接口只有两处**（其余全是骨架/渲染器自己消化的内部量）：
//   ① 骨架级「全局配置」：构造 SkeletonManager 时给「关节 index 分组」
//      （std::array<std::vector<int>, kNumThicknessGroup>），见 skeleton_manager.h；
//   ② 实例级取值：SkinnedInstanceState::thickness_scales（本文件）。
//   渲染侧要用的「每骨 bind 位置/朝向」由 SkeletonManager 自己从骨架导出（同 inverse_bind
//   的地位：派生量、从不做输入），不进任何公开签名。

// 粗细「关节组」的组数 = 每实例可独立控制的粗细自由度数。
//   取 8：与 per-instance attribute 的预算对齐 —— 2 个 vec4（loc11/12）= 8 个 float
//   （见 docs/jpov_crowd_body_shape_face_design.md §3.5 的 slot 预算表）。
inline constexpr int kNumThicknessGroup = 8;

// ==================== 部位额外旋转（partial rotation） ====================
//
// 用途：给「拉弓射箭 / 举枪瞄准」这类动作补一条**骨骼级整体转动** —— 例：姿势摆好后，把整个
//   上半身再扭一个角（扭腰）、把头再抬/低（仰头）。两个通道，各由**一根配置关节**驱动：
//   该关节及其**全部子树（descendants）**一起做一次刚体旋转。
// 机制（乙，2026-09-26 Danis 定：pose 先摆好、再叠加）：对参与某顶点的每根骨的对偶四元数
//   **前乘**一个模型系刚体变换 G_b（= ∏ 作用于该骨的通道 c 的 G_c，按 祖先→后代 顺序），其中
//     G_c = 在模型系里绕 **j_c 当前世界位置** pos_c 旋转 R_c = T(pos_c)·R_c·T(pos_c)⁻¹
//     pos_c = final(j_c)·p_c（final = pose atlas 里 j_c 本帧的最终变换；p_c = j_c 的 bind 位置）
//   ⇒ 落在该骨上的顶点被整体（绕 j_c 当前位置）转动；**权重混合区**（腰/颈边界）在相邻骨之间按
//   权重平滑过渡（与 thickness 同构：每骨算子按权重插值，见 skinning_shader.h）。
//   绕“当前点”的刚体旋转 ⇒ 根/祖先把身体挪到哪，扭转都跟得动，**不会腰斩**。
//
// 接口只有两处（与 thickness 同构）：
//   ① 骨架级「全局配置」：构造 SkeletonManager 时给两个**关节名**
//      （std::array<std::string, kNumPartialRotation>），见 skeleton_manager.h；
//   ② 实例级取值：SkinnedInstanceState::partial_rotations（本文件）。
// 每骨用到的「bind 位置（pivot）」与「该骨被哪条通道影响」由 SkeletonManager 自己从骨架
//   导出（子树的波及范围 = 派生量、从不做输入），不进任何公开签名。
//
// ⚠️ 语义（2026-09-26 Danis 定）：R_c 定义在**模型系**（= 骨架根空间，与 rest 顶点 / pose
//   atlas 同一空间；本工程人形资产面朝 +X、上为 +Y）—— 用户不必去处理骨局部系。额外旋转是
//   **在 pose 之后叠加**的（等价于在 j_c 的局部变换末尾额外乘一个旋转）；因 R_c 是模型系量，
//   落到 j_c **已 pose 的局部帧**后，净效果 = 绕 j_c **当前**位置、按**模型系 R_c** 转
//   （轴不随 pose 变）。详见设计文档 docs/jpov_partial_rotation_design.md。
inline constexpr int kNumPartialRotation = 2;

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
// ⚠️ 渲染端插值/蒙皮的只是“同一种骨架”内两个 pose 的蒙皮变换（见文件头铁律）。
//   pose 与骨架**强绑定**：一个 pose 严格属于某一种骨架（骨数量/树拓扑一致），否则无法解算
//   也不能插值。因此 pose 从不单独注册/分发：它只作为构造时整包的一部分落在那一份
//   SkeletonManager 里，天然不跨骨架。归属见 skeleton_manager.h。
//
// 数据表示取舍（定稿 2026-09-08）：SkeletonPose 是**用户资产层**（要可读、可手写、可 retarget），
//   因此收**每关节旋转（四元素，相对父）**，由 CPU 烘焙端沿骨架树解算出相对根 JointMatrix、
//   折入 inverse_bind 得到刚体蒙皮变换后再转对偶四元数落 atlas（skeleton_manager 负责烘焙，
//   本文件只定义资产格式）。一个 pose 严格从属某一份骨架
//   （joint_rotation.size() == 该骨架 bone_count），每个顶点的 rest 平移由 joints tree 提供，
//   pose 只驱动【旋转】；若动画/root-motion 带整体位移则由 root_offset 承载（纯原地动作默认为 0；
//   坐标系与单位见下方 root_offset 字段注释）。
struct SkeletonPose {
    int bone_count = 0;   // 应 == 所用 SkeletonType::bone_count（同一种骨架）。

    // 每关节相对其父关节的**旋转**（四元数，需整单位。索引与 SkeletonType::joints 对齐）。
    // joint_rotation[i] = 从父关节系转到本关节系的 local 旋转(相对父)。
    // 应为 size == bone_count（若含根且根无旋转可用 identity）。为空=走 rest(隐式全单位)。
    // 非空但 size != bone_count 视为非法（烘焙端 LOG(FATAL)）。
    std::vector<geom::Quaternion<float>> joint_rotation;

    // 根(0 号关节)的 **root-motion 平移量**：根关节在【其父坐标系】下、相对【其 bind 位置】
    // 的平移（= 当前平移 − bind 平移）。纯原地动作/静态 pose 保持默认 0（根停在 bind 位置）。
    //
    // 坐标系铁律（2026-09-20 定稿，详见 docs/jpov_root_offset_design.md §1）：
    //   - **是「根关节的父坐标系」，不是「根自己的 bind 系」**。对“根是顶层骨”的资产
    //     （Mixamo 源 / 多数 glb），父系 ≡ **模型系**（scene root）；两者在那种拓扑下数值
    //     重合，但定义取父系才在所有资产（含“零长包装 Root → Hips”两层拓扑）上站得住。
    //   - 理由：与 joint_rotation 语言对称（那根是“相对父的旋转增量”），且与烘焙式的
    //     jointLocal(root) = T(rest_offset[root]) · R(bind) · R(pose) 天然对齐。
    //
    // 单位 = **该 SkeletonType 的长度单位**（JPOV 统一为米）——与 rest_offset 同尺度。
    // ⚠️ 长度量在 pose 里、骨架尺度在 SkeletonType 里，二者独立：**缩放骨架不会自动缩放
    //    已产出的 pose**。故改了资产 bind（骨长/rest 朝向/根位置）⇒ 针对它重定向过的所有
    //    poses **必须重新重定向**（同“跨骨架 pose 不能插值”一类约束）。
    //
    // 施加规则：烘焙（SkeletonManager）与火柴人都把它加到【顶层骨】
    //   (parent == kSkeletonNoParent) 的平移上（= T(rest_offset + root_offset)）。合法骨架
    //   只有一根顶层骨（即 0 号）⇒ 等价于“加在 0 号上”。`Validate()` 并不禁止多顶层骨，
    //   那种骨架会把同一偏移加到每个顶层骨 —— 未定义的边缘情形，真人形资产不会出现。
    Vec3f root_offset{0.0f, 0.0f, 0.0f};

    // 全恒等 pose（每关节旋转 = identity，root_offset = 0）。
    // 语义（配合骨架）：当 SkeletonType 的 bind_rotation 是标准人形 bind 朝向时，
    //   本 pose 驱动出的姿态 = 该骨架的 **T-pose**（因为 bind 朝向已烘进骨架，pose 不再叠旋转）。
    // 用途：Mixamo23Skeleton 的配套静态姿势（T-pose）；也是蒙皮"静态退化门"的基准 pose。
    static SkeletonPose Identity(int bone_count);
};

// ==================== 运行时实例状态 ====================

// 单个实例的**摆放变换**（center 平移 + up/front 旋转 + scale 缩放）。
//
// 为什么把「摆放」从 SkinnedInstanceState 里单独抽成一层（2026-09-17）：
//   per-instance attribute 上传时，摆放是**矩阵**（4 个 vec4 slot），pose 选择是
//   **整数 + 浮点**。两者粒度不同，但蒙皮带骨实例与静态实例**共有的那一部分只有摆放**。
//   抽出来后，静态实例与带骨实例共用同一份 attribute layout / 同一套 VP 结构
//   （见 src/instance_buffer.h 的布局表，与 skinning_shader.h 的 aInstModel / uViewProj）。
//   这是「真 instanced draw」（一次 draw call 画 N 个实例）的基础设施：
//   摆放必须逐实例走 attribute（divisor=1），**不能**逐实例走 uniform（那还是 N 次 draw）。
//
// 语义与 Object3DCommand 的 center/up/front/scale 完全一致（同一套 BuildModelMatrix）。
struct InstanceTransform {
    Vec3f center;                      // 世界平移
    Vec3f up{0.0f, 1.0f, 0.0f};        // 局部 +Y → 世界 up（内部归一化）
    Vec3f front{0.0f, 0.0f, 1.0f};     // 局部 +Z → 世界 front（内部归一化）
    float scale = 1.0f;                // 整体缩放（先缩顶点，再旋转平移）
};

// 单个蒙皮实例的运行时状态：渲染按 (同 mesh + 同 skeleton) 把一批实例 instanced draw，
// 实例之间只差这份薄状态；渲染时作为 per-instance attribute 上传(glVertexAttribDivisor)。
//   这是一条"如何画出看得见的这一份"的 description(而非自己背整份几何)。
struct SkinnedInstanceState {
    // 模型摆放 —— center 平移 + up/front 旋转 + scale(见 InstanceTransform)。
    // 抽成嵌套结构是为了让「摆放矩阵」这道工序与「pose 选择」解耦：
    //   摆放 → per-instance attribute(mat4)；pose → per-instance attribute(ivec2 + float)。
    InstanceTransform transform;

    // ---- 运动：一份骨架内两个 pose 之间的插值（唯一接口）----
    // 唯一表达动画顶点的字段就是这个三元组：VS 取 pose_a/pose_b 两套蒙皮变换（对偶四元数），
    //   按 ratio ∈ [0,1] 逐骨插值（NLERP），得本实例这一帧的最终骨骼姿态后蒙皮（DLB）。
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

    // ---- 部位粗细：每实例给每个「关节组」一个横径缩放系数（Divisor=1 的 per-instance
    // attribute，见 src/instance_buffer.h 的 kInstanceThicknessAttrSpec）----
    // 语义：本组所辖关节上的顶点，其**横径**（垂直于骨长轴的两个方向）按该系数缩放，
    //   沿骨长轴的长度**不变**；1.0 = 原样（默认）。多骨权重混合区的顶点按权重插值
    //   （见 skinning_shader.h 的 shape 段）。
    // 对齐：数组下标 = **组号**（不是关节号）。「组号 → 哪些关节」由**骨架级**配置决定
    //   （SkeletonManager 构造时传入的 std::array<std::vector<int>, kNumThicknessGroup>）
    //   ⇒ **同一个组号在不同骨架上可以是完全不同的部位**，语义由那份骨架的配置负责。
    // 生效条件：该骨架的构造配置里有非空组；组号未被任何骨引用（或骨不在该组）时该系数
    //   无效果。配置里没有组 = 该骨架不做粗细（渲染侧整段跳过）。
    // Pre-condition: 每项 > 0（=0 会把截面压成零面积、<0 会翻法线）；实现应判非法即崩，
    //   不得 clamp 成「看起来还行」的值（否则 0.0 与 0.01 的区别会被静默吞掉）。
    // ⚠️ 默认值必须**逐个写满**（1.0 × 8）：std::array 的聚合初始化里写 {1.0f} 只会填第 0 个，
    //   其余为 0 —— 那是「零粗细」的致命退化，且极难一眼看出。
    std::array<float, kNumThicknessGroup> thickness_scales = {
        1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};

    // ---- 部位额外旋转：每实例给每个「旋转通道」一个**模型系**四元数 ----
    // 数组下标 = 通道号（0/1），与骨架级配置（skeleton_manager.h 的 partial_rotation_config）
    //   同序。这个 R **相对什么、定义在哪个系**（接口语义，很重要）：
    //
    //   · R **定义在模型系**（= 骨架根空间，与 rest 顶点 / pose atlas 同一空间；本工程人形
    //     资产面朝 +X、上为 +Y）——**不是**相对父关节、也**不是**相对该关节的局部坐标系；
    //     用户只需面对这一个坐标系。
    //   · 顺序是「**先摆 pose、再叠加 R**」：R 作用于该通道关节的**整个子树**，绕该关节的
    //     **当前 pose 位置**整体转（见 skinning_shader.h 的 ApplyPartialRotation）。
    //   · ⇒ R 的轴在模型系里是**固定**的，它的“解剖含义”取决于该关节**当前被 pose 摆成什么
    //     朝向**。
    //
    //   【例：TPose 歪头】TPose 下头朝 +X ⇒“仰头”= 绕模型 +Z 转 = 正常点头；但若 pose 已把
    //   上身转到头朝 +Z，同一个“绕 +Z”就变成**歪头（roll）**——而且此时再用另一条 partial 把
    //   上身扭回 +X 也救不回来（嵌套合成里“头的 R 先作用”，歪头已产生，后面的扭腰只把它整体
    //   转回去）。要“不管 pose 朝哪、仰头都相对身体点头”是另一种定义（轴随关节朝向共轭），
    //   **本接口不做**——这里刻意选的是“模型系固定轴”的清晰定义（2026-09-26 Danis 定）。
    //
    // 默认恒等 = 不转（旧场景零回归，靠 host 侧开关跳过整段）。
    // Pre-condition: 每项为单位四元数（实现会归一化；NaN/非有限判非法）。
    std::array<geom::Quaternion<float>, kNumPartialRotation> partial_rotations = {
        geom::Quaternion<float>::Identity(), geom::Quaternion<float>::Identity()};

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
