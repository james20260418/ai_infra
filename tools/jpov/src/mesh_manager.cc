// JPOV MeshManager 实现 — CPU MeshData → GPU (VAO/VBO/EBO) 上传/更新/释放

// MinGW: geom/common/math_util.h 用 M_PI，必须在**本 TU 首次 include <cmath> 之前**
// 定义 _USE_MATH_DEFINES（在头文件里定义太晚）。与 fbx_loader.cc 同款保护。
#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES
#endif

#include <algorithm>
#include <cmath>
#include <vector>

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
// 植被风关节表纹理（RGBA32F + 最近邻/边缘夹取）用到的 GL 1.2+ 常量。
// MinGW 的 GL/gl.h 只到 1.1，未声明；本文件不引 <GL/glext.h>（避免与 gl_loader 的
// 函数宏别名冲突），故就地补标准值，语义与 glext.h 一致。
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_RGBA32F
#define GL_RGBA32F 0x8814
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

namespace {

// 关节表在资产里用 7 个 float 存（末位 parent 是 float），这里解码回下标。
int ParentOfJoint(const MeshData::WindJoint& j) {
    return static_cast<int>(std::lround(j.parent));
}

// 两枢轴之间的距离（米）。
float PivotDistance(const float a[3], const float b[3]) {
    const float dx = a[0] - b[0];
    const float dy = a[1] - b[1];
    const float dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// 父链最多走多少**段**。三份 VS 的 `kMaxWindChain = 16` 是**节点**数（含自己），
// 即最多 15 段；这里按段数对齐 —— 超过上限的段 VS 本来就不参与变换，链长若算进去
// 就会让吹倒位移和实际旋转的链不一致。
constexpr int kMaxChainSegments = 15;

}  // namespace

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

    // ---- 次级顶点属性槽（可选，location 6，vec4）----
    //   ★ loc6 是从「per-instance 首槽」腾出来给**顶点属性**的槽位（见
    //   instance_buffer.h 的实例布局表：model 已从 6..9 挪到 7..10）。
    //   三个特性共用这一个槽，布局固定 vec4（MeshData::Validate 保证 kRelax/kWind 互斥）：
    //     - kRelax   （布料）：(lag_ratio, max_lag, base_tint 索引, 0)
    //     - kWind    （植被）：(joint_id,  phase,    base_tint 索引, 0)
    //     - kBaseTint（仅 tint）：(0, 0, base_tint 索引, 0)
    //   ⇒ .z 恒为 tint 索引（0 = 不换色）；kRelax/kWind 可与 kBaseTint 共存。
    //   属性缺失时该槽在本 VAO 上保持 disabled，读到的 aFlex 为默认值 (0,0,0,1)。
    //   ⚠️ 三份 VS 里 `aFlex` 一律声明成 vec4：GL 允许 shader 读少于 buffer 的分量数，
    //   但反过来（buffer 只给 2 个、shader 读 .z）会读到常量 0 —— tint 会静默失效。
    if (MeshHasFlag(data.flags, MeshVertexFlags::kRelax) ||
        MeshHasFlag(data.flags, MeshVertexFlags::kWind) ||
        MeshHasFlag(data.flags, MeshVertexFlags::kBaseTint)) {
        CHECK(!(MeshHasFlag(data.flags, MeshVertexFlags::kRelax) &&
                MeshHasFlag(data.flags, MeshVertexFlags::kWind)))
            << "MeshManager: kRelax 与 kWind 互斥（MeshData::Validate 应已拦下）";
        const size_t vn = data.positions.size();
        std::vector<float> aux(vn * 4, 0.0f);
        for (size_t i = 0; i < vn; ++i) {
            if (!data.relaxations.empty()) {
                aux[i * 4 + 0] = data.relaxations[i].x();   // lag_ratio
                aux[i * 4 + 1] = data.relaxations[i].y();   // max_lag（米）
            }
            if (!data.wind_link.empty()) {
                aux[i * 4 + 0] = data.wind_link[i].x();     // joint_id
                aux[i * 4 + 1] = data.wind_link[i].y();     // phase
            }
            if (!data.base_tint.empty()) {
                // 索引以 float 上传：VS 统一按 float 读，再取整成 int 经 flat varying 传给 FS。
                aux[i * 4 + 2] = static_cast<float>(data.base_tint[i]);
            }
        }
        glGenBuffers(1, &mesh.vbo_aux);
        CHECK_NE(mesh.vbo_aux, 0u);
        glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo_aux);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(aux.size() * sizeof(float)),
                     aux.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(6);
        glVertexAttribPointer(6, 4, GL_FLOAT, GL_FALSE,
                              sizeof(float) * 4, reinterpret_cast<const void*>(0));
    }

    // ---- 植被风关节表纹理（可选，RGBA32F，2N×1：每关节 2 texel）----
    //   同树全部 primitive 共享；VS 用 texelFetch 按 joint_id 取。
    //   texel(2j)   = (pivot.xyz, parent)
    //   texel(2j+1) = (level, phase, soft, 到根的链长)
    if (!data.wind_joints.empty()) {
        const size_t n = data.wind_joints.size();
        // 纹理宽 = 2n；超过 GL_MAX_TEXTURE_SIZE（常见 16384）会变成不完整纹理，
        // texelFetch 读到垃圾（静默错）——宁可此刻崩，不静默出错。
        CHECK_LE(2 * n, 16384u) << "wind_joint 表过大（2N 超纹理宽上限）: N=" << n;
        std::vector<float> tex(8 * n);
        // 每个关节「到根的链长」（米）：cum[k] = cum[parent] + |pivot_k − pivot_parent|。
        // 吹倒位移需要 L = cum[own] + |p − pivot_own|。cum 是**逐关节**的量，在表里算一次
        // 就够（VS 只多一次 length()）；让每个顶点沿父链重算一遍等于每个 pass 每个顶点
        // 多次 sqrt。texel(2j+1) 的第 4 分量原本空着（写 0），正好放它。
        for (size_t k = 0; k < n; ++k) {
            float acc = 0.0f;
            int cur = static_cast<int>(k);
            for (int step = 0; step < kMaxChainSegments; ++step) {
                const int par = ParentOfJoint(data.wind_joints[static_cast<size_t>(cur)]);
                if (par < 0 || par >= static_cast<int>(n) || par == cur) {
                    break;
                }
                acc += PivotDistance(data.wind_joints[static_cast<size_t>(cur)].pivot,
                                     data.wind_joints[static_cast<size_t>(par)].pivot);
                cur = par;
            }
            const MeshData::WindJoint& wj = data.wind_joints[k];
            tex[8 * k + 0] = wj.pivot[0];
            tex[8 * k + 1] = wj.pivot[1];
            tex[8 * k + 2] = wj.pivot[2];
            tex[8 * k + 3] = wj.parent;
            tex[8 * k + 4] = wj.level;
            tex[8 * k + 5] = wj.phase;
            tex[8 * k + 6] = wj.soft;
            tex[8 * k + 7] = acc;   // 到根的链长（吹倒位移的 L 基数）
        }
        glGenTextures(1, &mesh.wind_joint_tex);
        CHECK_NE(mesh.wind_joint_tex, 0u);
        mesh.wind_joint_count = static_cast<uint32_t>(n);
        glBindTexture(GL_TEXTURE_2D, mesh.wind_joint_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F,
                     static_cast<GLsizei>(2 * n), 1, 0,
                     GL_RGBA, GL_FLOAT, tex.data());
        glBindTexture(GL_TEXTURE_2D, 0);
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
    // 收集所有非 0 VBO + EBO 一次性 delete。
    // 属性 VBO 最多 8 个（pos/normal/uv/joint/weight/tangent/relax/wind）
    // + 1 个 EBO = 9；relax 与 wind 互斥，实际 ≤8。
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
    if (mesh->vbo_aux) {
        buffers[n++] = mesh->vbo_aux;
    }
    if (mesh->ebo) {
        buffers[n++] = mesh->ebo;
    }
    if (n > 0) {
        glDeleteBuffers(n, buffers);
    }
    if (mesh->wind_joint_tex) {
        glDeleteTextures(1, &mesh->wind_joint_tex);
    }

    *mesh = GPUMesh{};
}

}  // namespace jpov
