// JPOV MeshData — 顶点级网格数据定义
//
// MeshData 是 CPU 端的网格数据载体，描述"这个 mesh 包含哪些顶点属性"
// （位置 / 法线 / UV / 骨骼蒙皮），每个属性一个独立 vector。
//
// 设计要点：
// - flags 位掩码声明 mesh 包含哪些属性，各属性 vector 的长度必须与
//   positions 一致（Validate() 校验）。
// - 骨骼蒙皮（kJoints）扩展为每个顶点 4 组 joint 索引 + 4 组权重，
//   即每顶点至多被 4 个关节影响。
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

#include <cstdint>
#include <vector>

#include <glog/logging.h>
#include "geom/common/vec.h"

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
};

// 位掩码按位运算辅助：判断 flags 是否包含某属性。
// 用法：HasFlag(mesh.flags, MeshVertexFlags::kNormal)
inline bool MeshHasFlag(MeshVertexFlags flags, MeshVertexFlags flag) {
    return (static_cast<uint8_t>(flags) & static_cast<uint8_t>(flag)) != 0;
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
    std::vector<uint32_t> indices;

    // 骨骼专属（flags 包含 kJoints 时才有效）
    std::vector<int32_t[4]> joint_indices;   // 每个顶点 4 个 joint 索引
    std::vector<float[4]>   joint_weights;   // 每个顶点 4 个 joint 权重

    // 顶点数（positions 长度）。
    // Pre-condition: 已调用 Validate()（或至少 positions 已填充）
    size_t VertexCount() const { return positions.size(); }

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

    const bool joints_declared = MeshHasFlag(flags, MeshVertexFlags::kJoints);
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

}  // namespace jpov

#endif  // JPOV_MESH_H_
