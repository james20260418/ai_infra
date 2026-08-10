// JPOV GPUMesh — CPU MeshData 上传到 GPU 后的句柄
//
// GPUMesh 描述一份已驻留在 GPU 显存中的网格资源：
//   - 一个 VAO + 按属性分离的 VBO（positions/normals/uvs/joints/weights）
//   - 可选 EBO（indexed mesh；无索引时 index_count == 0）
//   - flags 记录该 mesh 声明了哪些顶点属性（与上传时的 MeshData 一致）
//
// 与 MeshData 的关系：
//   - MeshData  是 CPU 侧顶点数组（见 mesh.h），用户直接构造。
//   - GPUMesh   是 MeshData 经 MeshManager::RegisterMesh 上传后的发布结果，
//     由 MeshManager 分配并持有 GL 资源，用户持 mesh_id 引用。
//
// 设计要点（分离 VBO）：
//   - 每种属性独立 VBO，属性缺失时对应句柄为 0（如无法线则 vbo_normals == 0）。
//   - 后续骨骼蒙皮（kJoints）扩展时，vbo_joints / vbo_weights 存放
//     每顶点 4 个 int32 / 4 个 float（与 mesh.h 的 [4] 数组对应）。
//
// 生命周期：由 MeshManager 创建/释放，用户不直接 new/delete，
// 通过 RegisterMesh / UpdateMesh / ReleaseMesh 操作。

#ifndef JPOV_GPUMESH_H_
#define JPOV_GPUMESH_H_

#include <cstdint>

#include "tools/jpov/interface/mesh.h"

namespace jpov {

// GPU 端网格句柄：VAO + 按属性分离的 VBO + EBO。
// 由 MeshManager 分配并负责生命周期，用户只通过 mesh_id 引用。
struct GPUMesh {
    unsigned int vao = 0;
    unsigned int vbo_positions = 0;
    unsigned int vbo_normals = 0;   // 0 = 无法线属性
    unsigned int vbo_uvs = 0;       // 0 = 无 UV 属性
    unsigned int vbo_joints = 0;    // 0 = 无骨骼关节属性（预留）
    unsigned int vbo_weights = 0;   // 0 = 无骨骼权重属性（预留）
    unsigned int ebo = 0;           // 0 = 无索引（non-indexed mesh）
    uint32_t vertex_count = 0;      // positions 顶点数
    uint32_t index_count = 0;       // indices 数量（0 = non-indexed）
    MeshVertexFlags flags = MeshVertexFlags::kPosition;  // 与上传时 MeshData.flags 一致
};

}  // namespace jpov

#endif  // JPOV_GPUMESH_H_
