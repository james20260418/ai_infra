// JPOV MeshData — 顶点级网格数据定义
//
// MeshData 是 CPU 端的网格数据载体，描述"这个 mesh 包含哪些顶点属性"
// （位置 / 法线 / UV / 骨骼蒙皮 / 布料 LAG 松弛度），每个属性一个独立 vector。
//
// 设计要点：
// - flags 位掩码声明 mesh 包含哪些属性，各属性 vector 的长度必须与
//   positions 一致（Validate() 校验）。
// - 骨骼蒙皮（kJoints）扩展为每个顶点 4 组 joint 索引 + 4 组权重，
//   即每顶点至多被 4 个关节影响。
// - 布料 LAG 副运动（kRelax）：每顶点一对 (lag_ratio, max_lag)，用于蒙皮 VS 里对
//   「当前 pose」与「滞后 pose」的混合比例 + 位移限幅（见 docs/jpov_instance_attr_lag_design.md）。
// - 顶点 base tint 分区（kBaseTint）：每顶点一个 uint8 索引（0 = 不换色，1..N = 用第 i 号
//   用户色乘 baseColor）。可与 kRelax / kWind **同时存在**（共用 loc6 的 .z）。
// - GPUMesh（CPU → GPU 上传后的句柄）在 gpumesh.h 中定义，
//   本文件只关心 CPU 侧数据。
//
// 用法：
//   MeshData mesh;
//   mesh.flags = MeshVertexFlags::kPosition | MeshVertexFlags::kUV;
//   mesh.positions = {...};
//   mesh.uvs = {...};
//   mesh.indices = {...};
//   mesh.Validate();  // 校验数组对齐，非法输入 LOG(FATAL) crash
//
// 说明：
//   - indices（索引）不属于 flags 位掩码：indexed mesh 渲染必需，
//     non-indexed mesh 可留空（此时渲染按顶点序逐三角形）。

#ifndef JPOV_MESH_H_
#define JPOV_MESH_H_

#include <array>
#include <cstdint>
#include <limits>
#include <vector>

#include <glog/logging.h>
#include "geom/common/vec.h"
#include "tools/jpov/interface/pbr_material.h"  // kNumBaseTintColors（tint 索引空间单一来源）

namespace jpov {

// ==================== 类型别名 ====================

// 复用 geom 库的向量类型（与 render_command.h / camera.h 一致）
using Vec2f = geom::Vec2<float>;
using Vec3f = geom::Vec3<float>;

// 骨骼蒙皮每顶点固定 4 组 joint/weight（与 Danis 伪代码 int32_t[4]/float[4] 对应），
// 由 joint_indices / joint_weights 数组元素类型直接表达，无需运行时配置。

// ==================== 顶点属性位掩码 ====================

// Mesh 包含哪些顶点属性，位掩码可组合（| 运算）。
// kPosition 为必有项；其余按数据可用性声明。
enum class MeshVertexFlags : uint8_t {
    kNone     = 0,
    kPosition = 1 << 0,  // 必有
    kNormal   = 1 << 1,
    kUV       = 1 << 2,
    kJoints   = 1 << 3,  // 骨骼蒙皮（joint_indices + joint_weights）
    kTangent  = 1 << 4,  // 切线（法线映射 TBN 用，需 kNormal + kUV）
    kRelax    = 1 << 5,  // 布料 LAG 副运动（relaxations：每顶点 (lag_ratio, max_lag)）
    kWind     = 1 << 6,  // 植被风（wind_link：每顶点 (joint_id, phase)）；**与 kRelax 互斥**（同占 loc6 .xy）
    kBaseTint = 1 << 7,  // base tint 分区（base_tint：每顶点 uint8 索引）；与 kRelax/kWind **可共存**
};

// 位掩码按位运算辅助：判断 flags 是否包含某属性。
// 用法：HasFlag(mesh.flags, MeshVertexFlags::kNormal)
inline bool MeshHasFlag(MeshVertexFlags flags, MeshVertexFlags flag) {
    return (static_cast<uint8_t>(flags) & static_cast<uint8_t>(flag)) != 0;
}

// 有限性判据（**刻意不用 <cmath> 的 std::isfinite**）。
//
// 为什么：mesh.h 被大量 TU 间接包含。一旦这里 include <cmath>，MinGW 上 <math.h>
// 会在 TU 定义 _USE_MATH_DEFINES 之前就被锁死（include guard），随后 math_util.h /
// render_command.h 里的 M_PI/M_PI_2 全部“未定义” —— 2026-10-26 的 Windows CI 就是这么挂的。
// <limits> 不拉 <math.h>，且判据不依赖浮点运算（-ffast-math 下 `v-v==0` 会被优化掉）。
inline bool WindScalarIsFinite(float v) {
    return v == v && v != std::numeric_limits<float>::infinity() &&
           v != -std::numeric_limits<float>::infinity();
}

// ==================== CPU 侧网格数据 ====================

// CPU 端网格数据：逐属性数组 + 索引。
//
// 数组对齐规则（Validate() 强制）：
//   - normals / uvs / joint_indices / joint_weights 若非空，长度必须 == positions.size()
//   - positions 非空（至少 1 个顶点）
//   - positions.size() 必须能被子属性长度整除，保证每个顶点都有完整属性
//   - indices 可为空（non-indexed），非空时按 triangle list 语义使用
//   - 元素数量（vertex_count）上限以内置宏约束，超限 LOG(FATAL)
//
// Validate() 在数据就绪后显式调用一次；渲染前调用方可确保数据合法。
struct MeshData {
    // 声明这个 mesh 包含哪些属性
    MeshVertexFlags flags = MeshVertexFlags::kPosition;

    // 对应各 flag 的数据（长度须与 positions 一致）
    std::vector<Vec3f> positions;
    std::vector<Vec3f> normals;
    std::vector<Vec2f> uvs;
    std::vector<Vec3f> tangents;   // 切向量（逐顶点，法线映射 TBN 用；kTangent 时非空）
    // 布料 LAG 副运动标注（逐顶点；kRelax 时非空）：
    //   .x = lag_ratio ∈ [0,1]：该顶点参与滞后副运动的程度（0=完全跟随当前 pose，1=完全滞后）。
    //   .y = max_lag  ≥ 0    ：该顶点相对当前 pose 的最大允许位移（模型系，米；逐顶点限幅）。
    // 见 docs/jpov_instance_attr_lag_design.md §4.1 / §4.4。
    std::vector<Vec2f> relaxations;

    // base tint 分区标注（逐顶点 uint8；kBaseTint 时非空）：
    //   0 = 该顶点不参与换色；i ∈ [1, kNumBaseTintColors] = 用第 i 号用户色乘 baseColor。
    // 语义：工具（clothing tool 的 tint 画笔）把涂到的顶点标成某个 id；baseColor 贴图应由
    //   建模侧**涂白**或留白（白 × 用户色 = 用户色），引擎只做一次乘法、不改其它 PBR 通道。
    // GPU：与 kRelax/kWind 共用 loc6 vec4 的 **.z**（见 mesh_manager.cc 的布局表）：
    //   (lag_ratio, max_lag, tint, 0) 或 (joint_id, phase, tint, 0)。
    // 渲染约定：着色器按 **flat（provoking vertex）** 读 ⇒ 一个三角形整体归属一个色；
    //   画笔在三角形边界上可能左右不一致，边界由 provoking vertex 决定（MVP 接受）。
    // 消费路径：三条 3D 路径都读它（索引来自 loc6.z）：
    //   · object3d：颜色用 `PBRMaterial::base_tint`（逐物体）；
    //   · 静态实例：颜色用 `InstanceState::base_tint`（逐实例）；
    //   · 蒙皮：颜色用 `SkinnedInstanceState::base_tint`（逐实例）。
    std::vector<uint8_t> base_tint;

    // 植被风标注（逐顶点；kWind 时非空）—— **只存 2 个数**，把此桩“次级运动槽”用到最省：
    //   .x = joint_id ∈ [0, wind_joints.size())：该顶点所属关节在关节表中的下标；
    //   .y = phase    ∈ [0,1)（秒）：逐顶点的**连续**相位场（链外平动湍流用；连续⇒不撕裂）。
    // 其余风力所需量全部不进顶点，而进**关节表**（每关节一份，pivot/parent/level/phase/soft）；
    // softness 不再逐顶点存（湍流幅度掩膜改由关节表/位置现算）。见 docs/jpov_vegetation_wind_design.md。
    std::vector<Vec2f> wind_link;
    // 植被风的关节表（**每 mesh 一份**，来自资产 node extras jpov_wind_joints）：
    //   同树的多 primitive（trunk / leaves）共享同一张表。每个关节是刚性链的一环。
    struct WindJoint {
        float pivot[3];  // 枢轴位置（资产/模型系）
        float parent;    // 父关节下标；-1 = 根
        float level;     // 距根的关节数（θ 的频率/增益按此分档）
        float phase;     // 逐关节相位（秒）：同一刚性段内各顶点共享（否则段内剪切）
        float soft;      // 朝向继承度 0..1（0=硬/全继承父朝向；1=软/只随自己的 θ）
    };
    std::vector<WindJoint> wind_joints;
    std::vector<uint32_t> indices;

    // 骨骼专属（flags 包含 kJoints 时才有效）
    // 用 std::array 而非原始 C 数组：std::vector 容纳可拷贝元素，
    // 使 MeshData 可整体拷贝/移动（否则 vector<C数组> 不可拷贝）。
    std::vector<std::array<int32_t, 4>> joint_indices;  // 每顶点 4 个 joint 索引
    std::vector<std::array<float, 4>>   joint_weights;  // 每顶点 4 个 joint 权重

    // 顶点数（positions 长度）。
    // Pre-condition: 已调用 Validate()（或至少 positions 已填充）
    size_t VertexCount() const { return positions.size(); }

    // MakeBox: 构造一个局部坐标轴对齐盒（box），中心在局部原点。
    //
    // 只需三个尺寸（沿局部坐标轴的半宽），摆放位置/朝向交给渲染时的
    // DrawObject3D 的 center / up / front 决定（见 render_command.h）：
    //   - 局部 +Z 轴 = Draw 的 front 方向 → front_half_width
    //   - 局部 +Y 轴 = Draw 的 up 方向 → up_half_width
    //   - 局部 +X 轴 = Draw 的 left 方向（= cross(up, front)）→ left_half_width
    //
    // 生成的盒体：8 角点 = (±left_half_width, ±up_half_width, ±front_half_width)，
    // 6 个面、每面独立 4 顶点（共 24 顶点）+ 独立法线，12 个三角形（36 index）。
    // 顶点属性含 kPosition + kNormal。
    //
    // Pre-condition: 三个 half_width 均 > 0。
    static MeshData MakeBox(float front_half_width,
                            float up_half_width,
                            float left_half_width);

    // MakeOrientedBox: 构造一个**已带旋转与平移**的盒（中心在 origin 造好后整体变换）。
    //
    // 与 MakeBox 的唯一区别：MakeBox 只造局部轴对齐盒、朝向留给渲染时的
    // DrawObject3D(up/front)；本函数把朝向**烘进顶点**，因此可在一个 MeshData 里
    // 拼装多根不同朝向的几何（如骨架火柴人的每根骨——一个 Draw 只能给一个朝向，
    // 装不下十几根朝向各异的骨）。
    //
    // up / front 语义与 DrawObject3D 完全一致（见 render_command.h 的 DrawObject3D 注释），
    // 对应模型坐标系：
    //   - local +Y 轴 → up 方向
    //   - local +Z 轴 → front 方向
    //   - local +X 轴 → left 方向（= normalize(cross(up, front))，保证右手系）
    // 即三半宽与模型的对应关系同 MakeBox：front→+Z、up→+Y、left→+X。
    //
    // 变换顺序：顶点 = translation + R(up,front)·v_local；法线 = R(up,front)·n_local
    //   （法线只转不平移；R 为纯旋转 det=+1，故法线无需额外翻向）。
    //
    // Pre-condition: 三个 half_width 均 > 0；up / front 均非零且不平行（保证 cross 非退化）。
    static MeshData MakeOrientedBox(float front_half_width,
                                    float up_half_width,
                                    float left_half_width,
                                    const Vec3f& up, const Vec3f& front,
                                    const Vec3f& translation);

    // 校验各数组长度与 positions 一致、flag 声明与数据匹配。
    // 非法输入（长度不一致 / 缺 position / 骨骼 flag 但数据缺失）→ LOG(FATAL) crash。
    //
    // Pre-condition: 调用前应已设置 flags 并填充各数组。
    void Validate() const;
};

// Validate() 实现。
//
// 校验规则：
//   1. positions 非空（每个 mesh 至少 1 个顶点）。
//   2. 各可选属性数组（normals/uvs/joint_indices/joint_weights）非空时，
//      长度必须 == positions.size()。
//   3. flags 声明了某属性但对应数组为空 → crash（声明与数据不符）。
//   4. flags 未声明某属性但对应数组非空 → crash（多余数据，属误用）。
//   5. 骨骼 flag（kJoints）声明时 joint_indices / joint_weights 必须成对存在。
inline void MeshData::Validate() const {
    const size_t vcount = positions.size();

    // 1. 至少 1 个顶点
    CHECK_GT(vcount, 0u) << "MeshData::Validate: positions 不能为空";

    // 2. 位置属性必有（flags 必须声明 kPosition）
    CHECK(MeshHasFlag(flags, MeshVertexFlags::kPosition))
        << "MeshData::Validate: kPosition 为必有属性，flags 必须包含它";

    // 3. 各可选属性：非空数组长度必须与 positions 一致
    if (!normals.empty()) {
        CHECK_EQ(normals.size(), vcount)
            << "MeshData::Validate: normals.size() != positions.size()";
    }
    if (!uvs.empty()) {
        CHECK_EQ(uvs.size(), vcount)
            << "MeshData::Validate: uvs.size() != positions.size()";
    }
    if (!tangents.empty()) {
        CHECK_EQ(tangents.size(), vcount)
            << "MeshData::Validate: tangents.size() != positions.size()";
    }
    if (!relaxations.empty()) {
        CHECK_EQ(relaxations.size(), vcount)
            << "MeshData::Validate: relaxations.size() != positions.size()";
    }
    // base tint：逐顶点 uint8 索引，值域 0..kNumBaseTintColors（0 = 不换色）。
    //   越界 id 会让片元着色器取不到调色板（静默不变色），故必须在注册时崩。
    if (!base_tint.empty()) {
        CHECK_EQ(base_tint.size(), vcount)
            << "MeshData::Validate: base_tint.size() != positions.size()";
        for (size_t i = 0; i < base_tint.size(); ++i) {
            CHECK_LE(static_cast<int>(base_tint[i]), kNumBaseTintColors)
                << "MeshData::Validate: base_tint[" << i << "] = "
                << static_cast<int>(base_tint[i]) << " 越界（合法 0.."
                << kNumBaseTintColors << "，0 = 不换色）";
        }
    }
    // 植被风：逐顶点 (joint_id, phase) 须与顶点数对齐；有关节下标就一定要有表。
    if (!wind_link.empty()) {
        CHECK_EQ(wind_link.size(), vcount)
            << "MeshData::Validate: wind_link.size() != positions.size()";
        CHECK(!wind_joints.empty())
            << "MeshData::Validate: 有 wind_link 但无 wind_joints 关节表";
    }
    for (const Vec2f& wl : wind_link) {
        CHECK_GE(wl.x(), 0.0f) << "MeshData::Validate: wind_link 关节下标为负";
        CHECK_LT(static_cast<size_t>(wl.x()), wind_joints.size())
            << "MeshData::Validate: wind_link 关节下标越出关节表";
        CHECK_GE(wl.y(), 0.0f) << "MeshData::Validate: wind_link 相位为负";
        CHECK_LT(wl.y(), 1.0f) << "MeshData::Validate: wind_link 相位应落在 [0,1)";
    }
    if (!joint_indices.empty()) {
        CHECK_EQ(joint_indices.size(), vcount)
            << "MeshData::Validate: joint_indices.size() != positions.size()";
    }
    if (!joint_weights.empty()) {
        CHECK_EQ(joint_weights.size(), vcount)
            << "MeshData::Validate: joint_weights.size() != positions.size()";
    }

    // 4. flags 声明与数据一致性
    if (MeshHasFlag(flags, MeshVertexFlags::kNormal)) {
        CHECK_EQ(normals.size(), vcount)
            << "MeshData::Validate: flags 声明 kNormal 但 normals 长度不一致或缺数据";
    } else {
        CHECK(normals.empty())
            << "MeshData::Validate: 未声明 kNormal 但 normals 非空";
    }

    if (MeshHasFlag(flags, MeshVertexFlags::kUV)) {
        CHECK_EQ(uvs.size(), vcount)
            << "MeshData::Validate: flags 声明 kUV 但 uvs 长度不一致或缺数据";
    } else {
        CHECK(uvs.empty())
            << "MeshData::Validate: 未声明 kUV 但 uvs 非空";
    }

    if (MeshHasFlag(flags, MeshVertexFlags::kTangent)) {
        CHECK_EQ(tangents.size(), vcount)
            << "MeshData::Validate: flags 声明 kTangent 但 tangents 长度不一致或缺数据";
        // TBN 需要 normal + uv 才能从纹理推导切线空间
        CHECK(MeshHasFlag(flags, MeshVertexFlags::kNormal))
            << "MeshData::Validate: flags 声明 kTangent 但未声明 kNormal";
        CHECK(MeshHasFlag(flags, MeshVertexFlags::kUV))
            << "MeshData::Validate: flags 声明 kTangent 但未声明 kUV";
    } else {
        CHECK(tangents.empty())
            << "MeshData::Validate: 未声明 kTangent 但 tangents 非空";
    }

    if (MeshHasFlag(flags, MeshVertexFlags::kRelax)) {
        CHECK_EQ(relaxations.size(), vcount)
            << "MeshData::Validate: flags 声明 kRelax 但 relaxations 长度不一致或缺数据";
    } else {
        CHECK(relaxations.empty())
            << "MeshData::Validate: 未声明 kRelax 但 relaxations 非空";
    }

    if (MeshHasFlag(flags, MeshVertexFlags::kBaseTint)) {
        CHECK_EQ(base_tint.size(), vcount)
            << "MeshData::Validate: flags 声明 kBaseTint 但 base_tint 长度不一致或缺数据";
    } else {
        CHECK(base_tint.empty())
            << "MeshData::Validate: 未声明 kBaseTint 但 base_tint 非空";
    }

    if (MeshHasFlag(flags, MeshVertexFlags::kWind)) {
        CHECK_EQ(wind_link.size(), vcount)
            << "MeshData::Validate: flags 声明 kWind 但 wind_link 长度不一致或缺数据";
    } else {
        CHECK(wind_link.empty())
            << "MeshData::Validate: 未声明 kWind 但 wind_link 非空";
    }
    // 关节表内容：**引擎会拿它算数**（载入期算 cum 链长、VS 逐 texelFetch），
    // 所以非法值必须在注册时就崩，而不是渲染出畸形结果。
    //   · pivot 必须是有限数（NaN/Inf 会污染整条链的 qpos 和 L）；
    //   · parent 必须是 -1 或表内下标（越界会让 VS 的链走进垃圾 texel）；
    //   · level ≥ 0（θ 的增益按 level 分档，负数等于反向拧）、soft ∈ [0,1]。
    // phase 只查有限性：引擎对它只做 floor 取圈，任何有限值都合法。
    for (size_t i = 0; i < wind_joints.size(); ++i) {
        const WindJoint& j = wind_joints[i];
        for (int a = 0; a < 3; ++a) {
            CHECK(WindScalarIsFinite(j.pivot[a]))
                << "MeshData::Validate: wind_joints[" << i << "].pivot 非有限数";
        }
        // parent 用 float 存（资产 7 float/关节）但语义是下标：先按范围兜住（NaN/±Inf
        // 在这里必然为假），再确认是整数值 —— 否则“3.7 这种值”在 VS 里会四舍五入到 4。
        CHECK(j.parent >= -1.0f && j.parent < static_cast<float>(wind_joints.size()))
            << "MeshData::Validate: wind_joints[" << i << "].parent = " << j.parent
            << " 越出关节表（合法值：-1 或 [0, " << wind_joints.size() << ")）";
        const int parent = static_cast<int>(j.parent);   // 已被上式夹进 [-1, N) ⇒ 转换安全
        CHECK(j.parent == static_cast<float>(parent))
            << "MeshData::Validate: wind_joints[" << i << "].parent 必须是整数（-1 = 根），当前 "
            << j.parent;
        CHECK(WindScalarIsFinite(j.level) && j.level >= 0.0f)
            << "MeshData::Validate: wind_joints[" << i << "].level 必须是非负有限数";
        CHECK(WindScalarIsFinite(j.phase))
            << "MeshData::Validate: wind_joints[" << i << "].phase 非有限数";
        CHECK(WindScalarIsFinite(j.soft) && j.soft >= 0.0f && j.soft <= 1.0f)
            << "MeshData::Validate: wind_joints[" << i << "].soft 必须落在 [0,1]";
    }

    // kRelax 与 kWind **互斥**：二者同占顶点 loc6 的 .x/.y（次级运动通道），不能同一 mesh
    // 同时声明。kBaseTint 只占 .z ⇒ 与两者都可共存（风树也能换色 = 四季变色）。
    CHECK(!(MeshHasFlag(flags, MeshVertexFlags::kRelax) &&
            MeshHasFlag(flags, MeshVertexFlags::kWind)))
        << "MeshData::Validate: kRelax 与 kWind 互斥（同占 loc6 的 .x/.y），不能同时声明";

    const bool joints_declared = MeshHasFlag(flags, MeshVertexFlags::kJoints);
    // kWind 与 kJoints **互斥**：两条特性走的是两套互不组合的顶点着色器
    // （风 VS 只做关节链刚体变换、蒙皮 VS 只做骨骼查表），一个 mesh 同时声明等于
    // 「哪条路径画它，另一半特性就静默消失」——看到的是变形怪异而不是报错。
    // （2026-10-26 前这里是 loc3 属性槽冲突；顶点风改成 (joint_id, phase) 后 loc3 已释放，
    //   互斥的理由变成「渲染路径只能选一条」，约束本身保留。）
    CHECK(!(joints_declared && MeshHasFlag(flags, MeshVertexFlags::kWind)))
        << "MeshData::Validate: kWind 与 kJoints 互斥（风 VS 与蒙皮 VS 不组合）";
    if (joints_declared) {
        // 5. 骨骼 flag 时 joint_indices / joint_weights 必须成对存在且对齐
        CHECK_EQ(joint_indices.size(), vcount)
            << "MeshData::Validate: flags 声明 kJoints 但 joint_indices 缺失或长度不一致";
        CHECK_EQ(joint_weights.size(), vcount)
            << "MeshData::Validate: flags 声明 kJoints 但 joint_weights 缺失或长度不一致";
    } else {
        CHECK(joint_indices.empty())
            << "MeshData::Validate: 未声明 kJoints 但 joint_indices 非空";
        CHECK(joint_weights.empty())
            << "MeshData::Validate: 未声明 kJoints 但 joint_weights 非空";
    }
}

inline MeshData MeshData::MakeBox(float front_half_width,
                                  float up_half_width,
                                  float left_half_width) {
    CHECK_GT(front_half_width, 0.0f) << "MakeBox: front_half_width 必须 > 0";
    CHECK_GT(up_half_width, 0.0f) << "MakeBox: up_half_width 必须 > 0";
    CHECK_GT(left_half_width, 0.0f) << "MakeBox: left_half_width 必须 > 0";

    const float fw = front_half_width;
    const float uw = up_half_width;
    const float lw = left_half_width;

    // 8 角点（局部轴对齐，中心原点）。局部坐标轴：+X=left, +Y=up, +Z=front。
    // idx 0..7 = (±l, ±u, ±f) 组合（sl/su/sf 分别是 ±1 缩放 lw/uw/fw 半宽）。
    auto corner = [&](int sl, int su, int sf) {
        return Vec3f(sl * lw, su * uw, sf * fw);
    };
    const Vec3f corners[8] = {
        corner(+1, +1, +1),  // 0: +l +u +f
        corner(-1, +1, +1),  // 1: -l +u +f
        corner(+1, -1, +1),  // 2: +l -u +f
        corner(-1, -1, +1),  // 3: -l -u +f
        corner(+1, +1, -1),  // 4: +l +u -f
        corner(-1, +1, -1),  // 5: -l +u -f
        corner(+1, -1, -1),  // 6: +l -u -f
        corner(-1, -1, -1),  // 7: -l -u -f
    };

    MeshData mesh;
    mesh.flags = static_cast<MeshVertexFlags>(
        static_cast<uint8_t>(MeshVertexFlags::kPosition) |
        static_cast<uint8_t>(MeshVertexFlags::kNormal));

    // 每个面独立 4 顶点（共 24）+ 独立法线 + 2 三角形（共 12）。
    auto addFace = [&mesh](const Vec3f& p0, const Vec3f& p1,
                          const Vec3f& p2, const Vec3f& p3,
                          const Vec3f& n) {
        const uint32_t base = static_cast<uint32_t>(mesh.positions.size());
        mesh.positions.push_back(p0);
        mesh.positions.push_back(p1);
        mesh.positions.push_back(p2);
        mesh.positions.push_back(p3);
        for (int i = 0; i < 4; ++i) {
            mesh.normals.push_back(n);
        }
        mesh.indices.push_back(base + 0);
        mesh.indices.push_back(base + 1);
        mesh.indices.push_back(base + 2);
        mesh.indices.push_back(base + 0);
        mesh.indices.push_back(base + 2);
        mesh.indices.push_back(base + 3);
    };

    const Vec3f pos_x( 1.0f,  0.0f,  0.0f);  // +l（+X）
    const Vec3f neg_x(-1.0f,  0.0f,  0.0f);  // -l（-X）
    const Vec3f pos_y( 0.0f,  1.0f,  0.0f);  // +u（+Y）
    const Vec3f neg_y( 0.0f, -1.0f,  0.0f);  // -u（-Y）
    const Vec3f pos_z( 0.0f,  0.0f,  1.0f);  // +f（+Z）
    const Vec3f neg_z( 0.0f,  0.0f, -1.0f);  // -f（-Z）

    // 6 个面（绕序 CCW，从面外侧看）。
    // +f 面（法线 +Z）
    addFace(corners[0], corners[1], corners[3], corners[2], pos_z);
    // -f 面（法线 -Z）
    addFace(corners[4], corners[6], corners[7], corners[5], neg_z);
    // +u 面（法线 +Y）
    addFace(corners[0], corners[4], corners[5], corners[1], pos_y);
    // -u 面（法线 -Y）
    addFace(corners[2], corners[3], corners[7], corners[6], neg_y);
    // +l 面（法线 +X）
    addFace(corners[0], corners[2], corners[6], corners[4], pos_x);
    // -l 面（法线 -X）
    addFace(corners[1], corners[5], corners[7], corners[3], neg_x);

    return mesh;
}

inline MeshData MeshData::MakeOrientedBox(float front_half_width,
                                          float up_half_width,
                                          float left_half_width,
                                          const Vec3f& up,
                                          const Vec3f& front,
                                          const Vec3f& translation) {
    const float u_len = std::sqrt(up.x()*up.x() + up.y()*up.y() + up.z()*up.z());
    const float f_len = std::sqrt(front.x()*front.x() + front.y()*front.y() +
                                  front.z()*front.z());
    CHECK_GT(u_len, 1e-8f) << "MakeOrientedBox: up 向量不能为零";
    CHECK_GT(f_len, 1e-8f) << "MakeOrientedBox: front 向量不能为零";

    const Vec3f upn(up.x()/u_len, up.y()/u_len, up.z()/u_len);
    const Vec3f frn(front.x()/f_len, front.y()/f_len, front.z()/f_len);

    // left = normalize(cross(up, front))，与 MakeBox / DrawObject3D 的局部 +X 一致。
    Vec3f left(upn.y()*frn.z() - upn.z()*frn.y(),
               upn.z()*frn.x() - upn.x()*frn.z(),
               upn.x()*frn.y() - upn.y()*frn.x());
    const float l_len = std::sqrt(left.x()*left.x() + left.y()*left.y() +
                                  left.z()*left.z());
    CHECK_GT(l_len, 1e-8f)
        << "MakeOrientedBox: up 与 front 平行（cross 退化），无法确定朝向";
    left = Vec3f(left.x()/l_len, left.y()/l_len, left.z()/l_len);

    // frn 正交化：减去在 upn 上的投影，使 (left, upn, frn) 成标准正交基。
    // 否则 up/front 略不垂直时会在盒上引入剪切（MakeBox 里同样的隐患）。
    const float proj = frn.x()*upn.x() + frn.y()*upn.y() + frn.z()*upn.z();
    Vec3f f_orth(frn.x() - proj*upn.x(), frn.y() - proj*upn.y(),
                 frn.z() - proj*upn.z());
    const float fo_len = std::sqrt(f_orth.x()*f_orth.x() + f_orth.y()*f_orth.y() +
                                   f_orth.z()*f_orth.z());
    CHECK_GT(fo_len, 1e-8f)
        << "MakeOrientedBox: up 与 front 近乎平行，正交化后退化";
    f_orth = Vec3f(f_orth.x()/fo_len, f_orth.y()/fo_len, f_orth.z()/fo_len);

    // 先造局部轴对齐盒，再逐个顶点/法线施加旋转 R = [left | upn | f_orth] 与平移。
    MeshData mesh = MakeBox(front_half_width, up_half_width, left_half_width);
    for (Vec3f& p : mesh.positions) {
        const Vec3f v(p.x(), p.y(), p.z());
        // R·v：局部坐标在 (left, upn, f_orth) 三轴上的分量线性组合。
        const Vec3f rotated(left.x()*v.x() + upn.x()*v.y() + f_orth.x()*v.z(),
                            left.y()*v.x() + upn.y()*v.y() + f_orth.y()*v.z(),
                            left.z()*v.x() + upn.z()*v.y() + f_orth.z()*v.z());
        p = Vec3f(rotated.x() + translation.x(),
                  rotated.y() + translation.y(),
                  rotated.z() + translation.z());
    }
    for (Vec3f& n : mesh.normals) {
        const Vec3f v(n.x(), n.y(), n.z());
        n = Vec3f(left.x()*v.x() + upn.x()*v.y() + f_orth.x()*v.z(),
                  left.y()*v.x() + upn.y()*v.y() + f_orth.y()*v.z(),
                  left.z()*v.x() + upn.z()*v.y() + f_orth.z()*v.z());
    }

    return mesh;
}

}  // namespace jpov

#endif  // JPOV_MESH_H_
