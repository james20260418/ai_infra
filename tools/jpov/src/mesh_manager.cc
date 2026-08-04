// JPOV MeshManager 实现 — CPU MeshData → GPU (VAO/VBO/EBO) 上传/更新/释放

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
    uint32_t id = next_id_++;
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

    // ---- indices（可选，EBO）----
    if (!data.indices.empty()) {
        glGenBuffers(1, &mesh.ebo);
        CHECK_NE(mesh.ebo, 0u);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(data.indices.size() * sizeof(uint32_t)),
                     data.indices.data(), GL_STATIC_DRAW);
    }

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    GLenum err = glGetError();
    CHECK_EQ(err, GL_NO_ERROR)
        << "MeshManager: GL error after mesh upload, code=" << err;

    return mesh;
}

void MeshManager::DestroyGLMesh(GPUMesh* mesh /*inout*/) {
    if (mesh->vao) {
        glDeleteVertexArrays(1, &mesh->vao);
    }
    // 收集所有非 0 VBO + EBO 一次性 delete
    // GPUMesh 至多 5 个属性 VBO + 1 个 EBO = 6 个 GL 缓冲对象
    static constexpr int kMaxBuffers = 6;
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
    if (mesh->ebo) {
        buffers[n++] = mesh->ebo;
    }
    if (n > 0) {
        glDeleteBuffers(n, buffers);
    }

    *mesh = GPUMesh{};
}

}  // namespace jpov
