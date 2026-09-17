// JPOV MeshManager 实现 — CPU MeshData → GPU (VAO/VBO/EBO) 上传/更新/释放

#include <algorithm>

#include "tools/jpov/src/mesh_manager.h"

// GL 头文件必须最先 include（在 MinGW #define 宏替换之前），否则 GL 常量
// 在 MinGW 路径下不可见。顺序：GL 常量声明 → 再 #define 函数名映射。
//
// Linux/Mesa: 使用标准 GL 符号（libGL 直接导出），VAO 等 GL 3.x 函数声明
//   来自 <GL/glext.h>（配合 GL_GLEXT_PROTOTYPES 导出原型）。
// Windows/MinGW: gl_loader 用 wglGetProcAddress 运行时加载函数指针。
#ifdef _WIN32
#include <GL/gl.h>
#ifndef GLOG_NO_ABBREVIATED_SEVERITIES
#define GLOG_NO_ABBREVIATED_SEVERITIES
#endif
#include "third_party/gl_loader-mingw/gl_loader.h"

// glXxx→gl_Xxx 别名宏已集成在 gl_loader.h 中（#ifdef _WIN32），本文件不再重复定义。

// MinGW 的 GL/gl.h 是 OpenGL 1.1 头，缺少 VAO/缓冲相关常量，手动补齐标准值。
#ifndef GL_ARRAY_BUFFER
#define GL_ARRAY_BUFFER 0x8892
#endif
#ifndef GL_ELEMENT_ARRAY_BUFFER
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#endif
#ifndef GL_STATIC_DRAW
#define GL_STATIC_DRAW 0x88E4
#endif
#ifndef GL_FLOAT
#define GL_FLOAT 0x1406
#endif
#ifndef GL_INT
#define GL_INT 0x1404
#endif
#ifndef GL_UNSIGNED_INT
#define GL_UNSIGNED_INT 0x1405
#endif
#ifndef GL_DYNAMIC_DRAW
#define GL_DYNAMIC_DRAW 0x88E8
#endif
#ifndef GL_FALSE
#define GL_FALSE 0
#endif
// OpenGL 1.5+；gl.glBufferData 的 size 参数类型，MinGW 1.1 头未定义。
// 用指针宽度的有符号整数（对应 GLsizeiptr 标准语义）。
#ifndef GLsizeiptr
#ifdef _WIN64
typedef long long GLsizeiptr;
#else
typedef long GLsizeiptr;
#endif
#endif
#else
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>
#endif

#include <glog/logging.h>

namespace jpov {

// per-instance 实例变换矩阵的起始 attribute location（mat4 占 6..9，见头文件布局说明）。
// 避开 loc0-5（顶点属性：pos/normal/uv/joints/weights/tangent）。
static constexpr unsigned int kInstanceXformLoc = 6;
// pose 选择：vec3(pose_col_a, pose_col_b, ratio) —— 全部 float，*同一个* buffer、同一 stride，
//   不做 int/float 混装。
//   ⚠️ 为何不用 ivec2 + glVertexAttribIPointer：IPointer 读的是**原始整数位模式**，
//   把 float(92.0f) 的位（0x42B80000 = 1119354880）当整数读会得到天文数字的 texel 列，
//   texelFetch 直接出界 → 整批几何塌成空白（踩过这个坑）。统一 float 让宿主/GLSL 两侧
//   的隐式转换规则一致，无隐式位重解释。
//   三个 float 已在同一个 buffer 里，故只需一个 attribute（一个 vec3）。
static constexpr unsigned int kInstancePoseLoc = 10;

MeshManager::~MeshManager() {
    for (auto& kv : meshes_) {
        DestroyGLMesh(&kv.second);
    }
    meshes_.clear();
}

uint32_t MeshManager::RegisterMesh(const MeshData& data) {
    // 校验由 JPOV::RegisterMesh 强制 data.Validate() 保证，这里再防御一次
    data.Validate();

    GPUMesh mesh = CreateGLMesh(data);
    uint32_t id = id_alloc_.Acquire();  // 复用释放的 mesh_id 或开新号（避回绕，见 id_allocator.h）
    meshes_[id] = mesh;

    LOG(INFO) << "MeshManager: registered mesh id=" << id
              << " vertices=" << mesh.vertex_count
              << " indices=" << mesh.index_count
              << " flags=" << static_cast<uint32_t>(mesh.flags);

    return id;
}

void MeshManager::UpdateMesh(uint32_t mesh_id, const MeshData& new_data) {
    auto it = meshes_.find(mesh_id);
    CHECK(it != meshes_.end())
        << "MeshManager::UpdateMesh: mesh_id=" << mesh_id << " 未注册";

    new_data.Validate();

    // VBO 布局不能变：flags 必须与注册时一致
    MeshVertexFlags old_flags = it->second.flags;
    CHECK_EQ(static_cast<uint32_t>(old_flags),
             static_cast<uint32_t>(new_data.flags))
        << "MeshManager::UpdateMesh: 新数据 flags 与注册时不一致"
        << " (registered=" << static_cast<uint32_t>(old_flags)
        << ", new=" << static_cast<uint32_t>(new_data.flags) << ")";

    // Delete → Create 策略（简单可靠；顶点数不变时可后续优化为 glBufferSubData）
    DestroyGLMesh(&it->second);
    it->second = CreateGLMesh(new_data);
}

void MeshManager::ReleaseMesh(uint32_t mesh_id) {
    auto it = meshes_.find(mesh_id);
    if (it == meshes_.end()) {
        return;
    }
    DestroyGLMesh(&it->second);
    meshes_.erase(it);
    id_alloc_.Release(mesh_id);  // 空号回池，供后续 RegisterMesh 立即复用。
}

const GPUMesh* MeshManager::GetMesh(uint32_t mesh_id) const {
    auto it = meshes_.find(mesh_id);
    if (it == meshes_.end()) {
        return nullptr;
    }
    return &it->second;
}

GPUMesh MeshManager::CreateGLMesh(const MeshData& data) {
    data.Validate();

    GPUMesh mesh;
    mesh.flags = data.flags;
    mesh.vertex_count = static_cast<uint32_t>(data.positions.size());
    mesh.index_count = static_cast<uint32_t>(data.indices.size());

    // 缓存模型局部 AABB（阴影 pass 用它算物体世界包围盒，扩展级联覆盖）。
    if (!data.positions.empty()) {
        mesh.bounds_min[0] = mesh.bounds_max[0] = data.positions[0].x();
        mesh.bounds_min[1] = mesh.bounds_max[1] = data.positions[0].y();
        mesh.bounds_min[2] = mesh.bounds_max[2] = data.positions[0].z();
        for (const Vec3f& p : data.positions) {
            mesh.bounds_min[0] = std::min(mesh.bounds_min[0], p.x());
            mesh.bounds_min[1] = std::min(mesh.bounds_min[1], p.y());
            mesh.bounds_min[2] = std::min(mesh.bounds_min[2], p.z());
            mesh.bounds_max[0] = std::max(mesh.bounds_max[0], p.x());
            mesh.bounds_max[1] = std::max(mesh.bounds_max[1], p.y());
            mesh.bounds_max[2] = std::max(mesh.bounds_max[2], p.z());
        }
    }

    glGenVertexArrays(1, &mesh.vao);
    CHECK_NE(mesh.vao, 0u) << "MeshManager: glGenVertexArrays failed";
    glBindVertexArray(mesh.vao);

    // ---- position（必有，location 0）----
    glGenBuffers(1, &mesh.vbo_positions);
    CHECK_NE(mesh.vbo_positions, 0u);
    glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo_positions);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(data.positions.size() * sizeof(Vec3f)),
                 data.positions.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE,
                          sizeof(Vec3f), reinterpret_cast<const void*>(0));

    // ---- normal（可选，location 1）----
    if (MeshHasFlag(data.flags, MeshVertexFlags::kNormal)) {
        glGenBuffers(1, &mesh.vbo_normals);
        CHECK_NE(mesh.vbo_normals, 0u);
        glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo_normals);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(data.normals.size() * sizeof(Vec3f)),
                     data.normals.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE,
                              sizeof(Vec3f), reinterpret_cast<const void*>(0));
    }

    // ---- uv（可选，location 2）----
    if (MeshHasFlag(data.flags, MeshVertexFlags::kUV)) {
        glGenBuffers(1, &mesh.vbo_uvs);
        CHECK_NE(mesh.vbo_uvs, 0u);
        glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo_uvs);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(data.uvs.size() * sizeof(Vec2f)),
                     data.uvs.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE,
                              sizeof(Vec2f), reinterpret_cast<const void*>(0));
    }

    // ---- joints / weights（可选，location 3/4，骨骼蒙皮预留）----
    if (MeshHasFlag(data.flags, MeshVertexFlags::kJoints)) {
        // joint_indices：每顶点 4 个 int32，整体复制
        glGenBuffers(1, &mesh.vbo_joints);
        CHECK_NE(mesh.vbo_joints, 0u);
        glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo_joints);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(data.joint_indices.size() * sizeof(int32_t) * 4),
                     data.joint_indices.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(3);
        glVertexAttribIPointer(3, 4, GL_INT,
                               sizeof(int32_t) * 4,
                               reinterpret_cast<const void*>(0));

        // joint_weights：每顶点 4 个 float，整体复制
        glGenBuffers(1, &mesh.vbo_weights);
        CHECK_NE(mesh.vbo_weights, 0u);
        glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo_weights);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(data.joint_weights.size() * sizeof(float) * 4),
                     data.joint_weights.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(4);
        glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE,
                              sizeof(float) * 4,
                              reinterpret_cast<const void*>(0));
    }

    // ---- tangent（可选，location 5，法线映射 TBN）----
    if (MeshHasFlag(data.flags, MeshVertexFlags::kTangent)) {
        glGenBuffers(1, &mesh.vbo_tangents);
        CHECK_NE(mesh.vbo_tangents, 0u);
        glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo_tangents);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(data.tangents.size() * sizeof(Vec3f)),
                     data.tangents.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(5);
        glVertexAttribPointer(5, 3, GL_FLOAT, GL_FALSE,
                              sizeof(Vec3f), reinterpret_cast<const void*>(0));
    }

    // ---- indices（可选，EBO）----
    if (!data.indices.empty()) {
        glGenBuffers(1, &mesh.ebo);
        CHECK_NE(mesh.ebo, 0u);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(data.indices.size() * sizeof(uint32_t)),
                     data.indices.data(), GL_STATIC_DRAW);
    }

    // ---- per-instance 实例变换（location 6..9，divisor=1）----
    // 提前把 VBO 和 attrib 槽配好（但**暂不启用、不喂数据**）：这样 instanced draw 路径
    // 只需 UploadInstanceTransforms 填数据，无需再碰 VAO 配置。
    // 注：顶点属性数组的启用状态是 VAO 状态；divisor 也是 VAO 状态（GL 3.3 起）。
    //   默认 divisor=0 + 禁用 → 普通 draw 不受影响（零回归）。
    glGenBuffers(1, &mesh.vbo_instance_xform);
    CHECK_NE(mesh.vbo_instance_xform, 0u);
    glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo_instance_xform);
    for (int k = 0; k < 4; ++k) {
        const unsigned int loc = kInstanceXformLoc + static_cast<unsigned int>(k);
        glEnableVertexAttribArray(loc);
        glVertexAttribPointer(loc, 4, GL_FLOAT, GL_FALSE,
                              sizeof(float) * 16,
                              reinterpret_cast<const void*>(sizeof(float) * 4 * k));
        glVertexAttribDivisor(loc, 1);   // 每实例推进一步（instancing 核心）
        glDisableVertexAttribArray(loc); // 默认关：未上传实例数据时不影响普通 draw
    }

    // ---- per-instance pose 选择（location 10=ivec2 pose_col_a/pose_col_b，11=float ratio）----
    // 与实例变换同一个思路：提前配好槽位、默认禁用，由 UploadInstancePoseSelection 启用。
    // 两者共享同一个 buffer（每实例 3 个 float：col_a, col_b, ratio），只是切成不同的子区间。
    glGenBuffers(1, &mesh.vbo_instance_pose);
    CHECK_NE(mesh.vbo_instance_pose, 0u);
    glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo_instance_pose);
    glEnableVertexAttribArray(kInstancePoseLoc);
    // 每实例 3 个 float：[pose_col_a, pose_col_b, ratio]（一个 vec3 属性）。
    glVertexAttribPointer(kInstancePoseLoc, 3, GL_FLOAT, GL_FALSE,
                          sizeof(float) * 3, reinterpret_cast<const void*>(0));
    glVertexAttribDivisor(kInstancePoseLoc, 1);
    glDisableVertexAttribArray(kInstancePoseLoc);

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    GLenum err = glGetError();
    CHECK_EQ(err, GL_NO_ERROR)
        << "MeshManager: GL error after mesh upload, code=" << err;

    return mesh;
}

// ==================== Instancing ====================

void MeshManager::UploadInstanceTransforms(
    uint32_t mesh_id, const std::vector<float>& instance_matrices) {
    auto it = meshes_.find(mesh_id);
    CHECK(it != meshes_.end()) << "UploadInstanceTransforms: mesh_id "
                               << mesh_id << " 未注册";
    GPUMesh& mesh = it->second;
    CHECK(!instance_matrices.empty())
        << "UploadInstanceTransforms: 实例矩阵数组不能为空（叫它干什么？）";
    // 必须是完整的 mat4 数组（每实例 16 个 float）——不是 16 的倍数就是调用方算错步长。
    CHECK_EQ(instance_matrices.size() % 16, 0u)
        << "UploadInstanceTransforms: 数组长度 " << instance_matrices.size()
        << " 不是 16 的倍数（每实例一个 mat4）";
    CHECK_NE(mesh.vbo_instance_xform, 0u)
        << "UploadInstanceTransforms: mesh " << mesh_id
        << " 无实例变换 VBO（RegisterMesh 未初始化？）";

    const size_t bytes = instance_matrices.size() * sizeof(float);
    glBindVertexArray(mesh.vao);
    glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo_instance_xform);
    // GL_DYNAMIC_DRAW：实例变换每帧变。
    //   每帧都用 glBufferData(ptr) 整体重传：整批实例都会重写，留旧内容无意义；
    //   glBufferData 还会在超出容量时自动重新分配存储（glBufferSubData 只能在已有
    //   容量内写，扩容必须走 glBufferData）。
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(bytes),
                 instance_matrices.data(), GL_DYNAMIC_DRAW);
    mesh.instance_xform_capacity_bytes = bytes;
    // 启用 per-instance 属性（divisor 已在 RegisterMesh 设为 1）。
    for (int k = 0; k < 4; ++k) {
        glEnableVertexAttribArray(kInstanceXformLoc + static_cast<unsigned int>(k));
    }
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void MeshManager::DisableInstanceAttributes(uint32_t mesh_id) {
    auto it = meshes_.find(mesh_id);
    CHECK(it != meshes_.end()) << "DisableInstanceAttributes: mesh_id "
                               << mesh_id << " 未注册";
    GPUMesh& mesh = it->second;
    if (mesh.vbo_instance_xform == 0u && mesh.vbo_instance_pose == 0u) {
        return;  // 没配过实例属性，no-op
    }
    glBindVertexArray(mesh.vao);
    for (int k = 0; k < 4; ++k) {
        glDisableVertexAttribArray(kInstanceXformLoc + static_cast<unsigned int>(k));
    }
    glDisableVertexAttribArray(kInstancePoseLoc);
    glBindVertexArray(0);
}

void MeshManager::UploadInstancePoseSelection(
    uint32_t mesh_id, const std::vector<float>& instance_pose) {
    auto it = meshes_.find(mesh_id);
    CHECK(it != meshes_.end()) << "UploadInstancePoseSelection: mesh_id "
                               << mesh_id << " 未注册";
    GPUMesh& mesh = it->second;
    CHECK(!instance_pose.empty())
        << "UploadInstancePoseSelection: 数组不能为空";
    CHECK_EQ(instance_pose.size() % 3, 0u)
        << "UploadInstancePoseSelection: 长度 " << instance_pose.size()
        << " 不是 3 的倍数（每实例 pose_col_a/pose_col_b/ratio）";
    CHECK_NE(mesh.vbo_instance_pose, 0u)
        << "UploadInstancePoseSelection: mesh " << mesh_id
        << " 无 pose 选择 VBO（RegisterMesh 未初始化？）";

    const size_t bytes = instance_pose.size() * sizeof(float);
    glBindVertexArray(mesh.vao);
    glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo_instance_pose);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(bytes),
                 instance_pose.data(), GL_DYNAMIC_DRAW);
    mesh.instance_pose_capacity_bytes = bytes;
    glEnableVertexAttribArray(kInstancePoseLoc);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void MeshManager::DestroyGLMesh(GPUMesh* mesh /*inout*/) {
    if (mesh->vao) {
        glDeleteVertexArrays(1, &mesh->vao);
    }
    // 收集所有非 0 VBO + EBO 一次性 delete
    // GPUMesh 至多 6 个属性 VBO + 1 个实例变换 VBO + 1 个 pose 选择 VBO + 1 个 EBO
    // = 9 个 GL 缓冲对象
    static constexpr int kMaxBuffers = 9;
    unsigned int buffers[kMaxBuffers];
    int n = 0;
    if (mesh->vbo_positions) {
        buffers[n++] = mesh->vbo_positions;
    }
    if (mesh->vbo_normals) {
        buffers[n++] = mesh->vbo_normals;
    }
    if (mesh->vbo_uvs) {
        buffers[n++] = mesh->vbo_uvs;
    }
    if (mesh->vbo_joints) {
        buffers[n++] = mesh->vbo_joints;
    }
    if (mesh->vbo_weights) {
        buffers[n++] = mesh->vbo_weights;
    }
    if (mesh->vbo_tangents) {
        buffers[n++] = mesh->vbo_tangents;
    }
    if (mesh->vbo_instance_xform) {
        buffers[n++] = mesh->vbo_instance_xform;
    }
    if (mesh->vbo_instance_pose) {
        buffers[n++] = mesh->vbo_instance_pose;
    }
    if (mesh->ebo) {
        buffers[n++] = mesh->ebo;
    }
    if (n > 0) {
        glDeleteBuffers(n, buffers);
    }

    *mesh = GPUMesh{};
}

}  // namespace jpov
