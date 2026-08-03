// JPOV MeshManager — GPU 网格资源生命周期管理
//
// 管理 CPU MeshData → GPU（VAO/VBO/EBO）的上传、更新与释放。
// 作为 Renderer 的内部组件，不直接暴露给用户；
// 用户通过 JPOV::RegisterMesh / JPOV::UpdateMesh / JPOV::ReleaseMesh 间接使用。
//
// mesh_id:
//   - 外部 mesh_id 为 uint32_t，由 MeshManager 分配（自 1 递增）。
//   - 内部映射到 GPUMesh（持有 VAO + 分离 VBO + EBO）。
//   - 所有 GL 资源在 MeshManager 析构时统一释放。
//
// VBO 布局（按属性分离，属性缺失时对应 VBO = 0）:
//   location 0 = position  (vec3 float)   ← vbo_positions，必有
//   location 1 = normal    (vec3 float)   ← vbo_normals，flags 含 kNormal 才有
//   location 2 = uv        (vec2 float)   ← vbo_uvs，    flags 含 kUV 才有
//   location 3 = joints    (ivec4 int32)  ← vbo_joints， flags 含 kJoints 才有
//   location 4 = weights   (vec4 float)   ← vbo_weights，flags 含 kJoints 才有
//
// 固定 attribute location 为后续骨骼 shader（mesh3d_skinned）预留「口子」：
// 新增 shader 只需声明相同的 layout(location=N)，无需改动本类。

#ifndef JPOV_MESH_MANAGER_H_
#define JPOV_MESH_MANAGER_H_

#include <cstdint>
#include <unordered_map>

#include "tools/jpov/interface/gpumesh.h"
#include "tools/jpov/interface/mesh.h"

namespace jpov {

class MeshManager {
public:
    MeshManager() = default;
    ~MeshManager();

    MeshManager(const MeshManager&) = delete;
    MeshManager& operator=(const MeshManager&) = delete;

    // RegisterMesh: 将 CPU MeshData 上传为 GPU mesh，返回 mesh_id。
    //
    // 按 data.flags 决定创建哪些属性 VBO 与 VAO attribute 绑定。
    // data.Validate() 由调用方保证（JPOV::RegisterMesh 内强制校验）。
    //
    // Pre-condition: GL context 已激活
    // Pre-condition: data.Validate() 已通过（数组对齐、flags 与数据一致）
    uint32_t RegisterMesh(const MeshData& data);

    // UpdateMesh: 更新已有 mesh 的顶点数据。
    //
    // new_data.flags 必须与注册时的 flags 一致（VBO 布局不能变）。
    // 实现策略：删除旧 GL 资源 → 按新数据重建（简单可靠；
    // 顶点数不变时可后续优化为 glBufferSubData）。
    //
    // Pre-condition: mesh_id 已注册
    // Pre-condition: new_data.Validate() 已通过
    // Pre-condition: new_data.flags == 注册时的 flags
    void UpdateMesh(uint32_t mesh_id, const MeshData& new_data);

    // ReleaseMesh: 释放 mesh 的 GL 资源并移除记录。
    //
    // mesh_id 不存在 → 静默忽略（允许重复释放）。
    // Pre-condition: GL context 已激活（或正在析构）
    void ReleaseMesh(uint32_t mesh_id);

    // GetMesh: 获取 mesh_id 对应的 GPU mesh 句柄。
    //
    // 返回 nullptr 表示 mesh_id 不存在。
    const GPUMesh* GetMesh(uint32_t mesh_id) const;

private:
    // 创建 GL 资源（VAO + 按 flags 分离 VBO + EBO），返回填充好句柄的 GPUMesh。
    // Pre-condition: data.Validate() 已通过
    static GPUMesh CreateGLMesh(const MeshData& data);

    // 释放单个 GPUMesh 的全部 GL 资源（VAO + 所有非 0 VBO + EBO）。
    static void DestroyGLMesh(GPUMesh* mesh /*inout*/);

    // id counter（递增分配，0 保留为「无效 mesh_id」）
    uint32_t next_id_ = 1;

    // mesh_id → GPUMesh
    std::unordered_map<uint32_t, GPUMesh> meshes_;
};

}  // namespace jpov

#endif  // JPOV_MESH_MANAGER_H_
