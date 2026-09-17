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
//   location 5 = tangent   (vec3 float)   ← vbo_tangents，flags 含 kTangent 才有
//
// Instancing（per-instance attribute，divisor=1；"真 instanced draw" 专用）:
//   location 6..9  = aInstModel     (mat4，4 个 vec4 slot) ← vbo_instance_xform
//   location 10    = aInstPoseCol   (ivec2: pose_col_a, pose_col_b) ← vbo_instance_pose
//   location 11    = aInstRatio     (float)                          ← 同上 buffer
//   这些是用 UploadInstanceTransforms / UploadInstancePoseSelection 逐实例上传的，
//   **不属于 MeshData**（不是顶点属性），因此不参与 RegisterMesh/UpdateMesh 的 flags 约定。
//   默认关闭（divisor=0 + 禁用）。矩阵按“每实例 4 个 vec4”拆到 4 个连续 location
//   （GL 无 mat4 attribute）。
// 固定 attribute location 为后续骨骼 shader（mesh3d_skinned）预留「口子」：
// 新增 shader 只需声明相同的 layout(location=N)，无需改动本类。

#include <vector>

#ifndef JPOV_MESH_MANAGER_H_
#define JPOV_MESH_MANAGER_H_

#include <cstdint>
#include <unordered_map>

#include "tools/jpov/interface/gpumesh.h"
#include "tools/jpov/interface/id_allocator.h"
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

    // ---- Instancing ----

    // UploadInstanceTransforms: 把一批实例的 model 矩阵传成 per-instance attribute。
    //
    // 每个 matrix 是**列主序** float[16]（与 glUniformMatrix4fv 同一套，见 BuildModelMatrix）。
    // 内部拆成 4 个 vec4 连续填到 location 6..9，并调 glVertexAttribDivisor(.., 1)。
    // 调用方随后用 glDrawElementsInstanced(..., instance_count) 一次画完全批。
    //
    // ⚠️ 为什么走 attribute 而不是逐实例 glUniform：逐实例 uniform 必须**逐实例一次 draw**
    //   （N 个实例 = N 次 draw call）。per-instance attribute（divisor=1）+ Instanced draw
    //   才能把整批压成**一次** draw call —— 这才是 instancing 的意义（见
    //   docs/jpov_crowd_instancing_arch.md §1.2）。
    //
    // 本函数只喂数据，不改 VAO 之外的任何 GL 状态（draw 形式由调用方决定）。
    // 同一个 mesh 可反复调用（复用同一 VBO，容量不够时自动扩容）。
    //
    // Pre-condition: mesh_id 已注册；instance_matrices 非空
    void UploadInstanceTransforms(uint32_t mesh_id,
                                  const std::vector<float>& instance_matrices);

    // UploadInstancePoseSelection: 把一批实例的 **pose 选择** 传成 per-instance attribute。
    //
    // 每实例 3 个量：pose_col_a / pose_col_b（均 = pose_idx * pose_width，整数）
    //   + ratio（float）。布局：
    //     location 10 = aInstPoseCol (ivec2: pose_col_a, pose_col_b)
    //     location 11 = aInstRatio   (float)
    // 一个 float 数组（每实例 3 个 float，按 [col_a, col_b, ratio] 顺序）就够，
    //   两个 attribute 只是把同一 buffer 的同一 stride 切成不同的子区间（ivec2 读前 2 个，
    //   float 读第 3 个）。
    //
    // 为何也走 attribute：pose 选择是**逐实例**差异；逐实例 uniform 会把整批退化成 N 次
    //   draw call，与 instancing 的初衷相违。
    //
    // Pre-condition: mesh_id 已注册；instance_pose 非空且长度为 3 的倍数
    void UploadInstancePoseSelection(uint32_t mesh_id,
                                     const std::vector<float>& instance_pose);

    // DisableInstanceAttributes: 关掉 mesh 的 per-instance 属性（divisor 归 0 + 禁用数组）。
    // 用途：同一份 mesh 被**非 instanced** 路径复用（如 Object3D 逐物体 draw）时，
    // 必须先把 per-instance 属性关掉，否则残留的 divisor=1 会让普通 draw 语义错乱。
    // 无 per-instance 数据时是 no-op。
    // Pre-condition: mesh_id 已注册
    void DisableInstanceAttributes(uint32_t mesh_id);

private:
    // 创建 GL 资源（VAO + 按 flags 分离 VBO + EBO），返回填充好句柄的 GPUMesh。
    // Pre-condition: data.Validate() 已通过
    static GPUMesh CreateGLMesh(const MeshData& data);

    // 释放单个 GPUMesh 的全部 GL 资源（VAO + 所有非 0 VBO + EBO）。
    static void DestroyGLMesh(GPUMesh* mesh /*inout*/);

    // mesh_id 分配：IdAllocator（freelist/LIFO 复用 + live 集防回绕踩踏）。0 = 无效 mesh_id。
    //   原裸 uint32 next_id_++ 不回填空号、回绕会踩 live —— 换复用分配器;已释放 id 立即回到池。
    IdAllocator id_alloc_;

    // mesh_id → GPUMesh
    std::unordered_map<uint32_t, GPUMesh> meshes_;
};

}  // namespace jpov

#endif  // JPOV_MESH_MANAGER_H_
