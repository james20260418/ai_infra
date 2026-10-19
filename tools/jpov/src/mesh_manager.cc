// JPOV MeshManager 实现 — CPU MeshData → GPU (VAO/VBO/EBO) 上传/更新/释放

// MinGW: geom/common/math_util.h 用 M_PI，必须在**本 TU 首次 include <cmath> 之前**
// 定义 _USE_MATH_DEFINES（在头文件里定义太晚）。与 fbx_loader.cc 同款保护。
#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES
#endif

#include <algorithm>
#include <cmath>
#include <vector>

#include "tools/jpov/interface/wind_deform.h"
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

    // ---- 次级运动槽（可选，location 6，vec2）----
    //   ★ loc6 是本 PR 从「per-instance 首槽」腾出来给**顶点属性**的槽位（见
    //   instance_buffer.h 的实例布局表：model 已从 6..9 挪到 7..10）。
    //   该槽由两个**互斥**特性共用（MeshData::Validate 保证不会同时声明）：
    //     - kRelax（布料）：vec2(lag_ratio, max_lag)
    //     - kWind （植被）：vec2(joint_id, phase)
    //   属性缺失时该槽在本 VAO 上保持 disabled，读到的 aFlex 为默认值。
    if (MeshHasFlag(data.flags, MeshVertexFlags::kRelax)) {
        glGenBuffers(1, &mesh.vbo_relax);
        CHECK_NE(mesh.vbo_relax, 0u);
        glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo_relax);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(data.relaxations.size() * sizeof(Vec2f)),
                     data.relaxations.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(6);
        glVertexAttribPointer(6, 2, GL_FLOAT, GL_FALSE,
                              sizeof(Vec2f), reinterpret_cast<const void*>(0));
    }
    if (MeshHasFlag(data.flags, MeshVertexFlags::kWind)) {
        // 3 float/顶点：(joint_id, phase, L)。L 在载入期算好并平滑（见 wind_deform.h）。
        // ⚠️ stride=12、size=3：object3d 的 VS 仍声明 `vec2 aFlex` —— GL 只取前两个分量，
        //    完全兼容（改动只在「缓冲变大 4 字节/顶点」）。
        // 逐顶点：最近的两个关节 + 1/距离归一化权重。
        //   joint 的 lev/soft 是离散量；硬分配会让相邻顶点分属不同关节 → 变换不连续。
        //   两个关节按距离加权（LBS）后接缝处连续，且不需要改资产格式。
        const size_t vn = data.positions.size();
        const size_t jn = data.wind_joints.size();
        std::vector<float> wind_data(vn * 4, 0.0f);
        std::vector<float> wind_w(vn * 2, 0.0f);
        std::vector<int> vj0(vn, -1);
        std::vector<int> vj1(vn, -1);
        std::vector<float> vw0(vn, 0.0f);
        std::vector<float> vw1(vn, 0.0f);
        const std::vector<float> path_len = BuildWindPathLengths(data);
        for (size_t i = 0; i < vn; ++i) {
            const Vec3f& p = data.positions[i];
            float d0 = 1e18f;
            float d1 = 1e18f;
            int b0 = -1;
            int b1 = -1;
            for (size_t j = 0; j < jn; ++j) {
                const MeshData::WindJoint& wj = data.wind_joints[j];
                const float dx = p.x() - wj.pivot[0];
                const float dy = p.y() - wj.pivot[1];
                const float dz = p.z() - wj.pivot[2];
                const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (d < d0) {
                    d1 = d0;
                    b1 = b0;
                    d0 = d;
                    b0 = static_cast<int>(j);
                } else if (d < d1) {
                    d1 = d;
                    b1 = static_cast<int>(j);
                }
            }
            if (b0 < 0) {
                continue;
            }
            if (b1 < 0) {
                b1 = b0;
                d1 = d0;
            }
            const float inv0 = 1.0f / (d0 + 1.0e-4f);
            const float inv1 = 1.0f / (d1 + 1.0e-4f);
            const float wsum = inv0 + inv1;
            vj0[i] = b0;
            vj1[i] = b1;
            vw0[i] = inv0 / wsum;
            vw1[i] = inv1 / wsum;
        }

        // 近重合顶点（1~2mm 的 UV 缝/硬边）必须共享同一套关节权重：
        //   否则 1.5mm 的缝会在吹倒后拉成几厘米的薄片（willow 实测放大 35~83 倍）。
        //   用空间哈希 + 5mm 邻域把「肉眼同一个点」的顶点归一。
        {
            constexpr float kWeld = 0.005f;
            constexpr float kCell = 0.01f;
            std::map<std::array<int, 3>, std::vector<uint32_t>> buckets;
            std::vector<uint32_t> rep(vn);
            for (size_t i = 0; i < vn; ++i) {
                const Vec3f& p = data.positions[i];
                const std::array<int, 3> base{
                    static_cast<int>(std::floor(p.x() / kCell)),
                    static_cast<int>(std::floor(p.y() / kCell)),
                    static_cast<int>(std::floor(p.z() / kCell))};
                uint32_t found = static_cast<uint32_t>(i);
                bool merged = false;
                for (int dx = -1; dx <= 1 && !merged; ++dx) {
                    for (int dy = -1; dy <= 1 && !merged; ++dy) {
                        for (int dz = -1; dz <= 1 && !merged; ++dz) {
                            const auto it = buckets.find(
                                {base[0] + dx, base[1] + dy, base[2] + dz});
                            if (it == buckets.end()) {
                                continue;
                            }
                            for (uint32_t cand : it->second) {
                                const Vec3f& q = data.positions[cand];
                                const float ddx = p.x() - q.x();
                                const float ddy = p.y() - q.y();
                                const float ddz = p.z() - q.z();
                                if (ddx * ddx + ddy * ddy + ddz * ddz <= kWeld * kWeld) {
                                    found = cand;
                                    merged = true;
                                    break;
                                }
                            }
                        }
                    }
                }
                rep[i] = found;
                if (!merged) {
                    buckets[base].push_back(static_cast<uint32_t>(i));
                }
            }
            for (size_t i = 0; i < vn; ++i) {
                const uint32_t r = rep[i];
                if (r != i && vj0[r] >= 0) {
                    vj0[i] = vj0[r];
                    vj1[i] = vj1[r];
                    vw0[i] = vw0[r];
                    vw1[i] = vw1[r];
                }
            }
        }

        for (size_t i = 0; i < vn; ++i) {
            if (vj0[i] < 0) {
                continue;
            }
            wind_data[i * 4 + 0] = static_cast<float>(vj0[i]);  // j0（与权重同源）
            wind_data[i * 4 + 1] = data.wind_link[i].y();       // phase
            wind_data[i * 4 + 2] = path_len[i];                 // L（备用）
            wind_data[i * 4 + 3] = static_cast<float>(vj1[i]);  // j1
            wind_w[i * 2 + 0] = vw0[i];
            wind_w[i * 2 + 1] = vw1[i];
        }
        glGenBuffers(1, &mesh.vbo_wind);
        CHECK_NE(mesh.vbo_wind, 0u);
        glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo_wind);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(wind_data.size() * sizeof(float)),
                     wind_data.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(6);
        glVertexAttribPointer(6, 4, GL_FLOAT, GL_FALSE,
                              sizeof(float) * 4, reinterpret_cast<const void*>(0));

        // 两关节混合权重（loc3，vec2）。⚠️ 蒙皮路径把 loc3 用作 aJoint，
        //   但植被 mesh 不会被蒙皮路径绘制（静态实例专属），同一 VAO 不会同时走两条路径。
        glGenBuffers(1, &mesh.vbo_wind_w);
        CHECK_NE(mesh.vbo_wind_w, 0u);
        glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo_wind_w);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(wind_w.size() * sizeof(float)),
                     wind_w.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE,
                              sizeof(float) * 2, reinterpret_cast<const void*>(0));
    }

    // ---- 植被风关节表纹理（可选，RGBA32F，2N×1：每关节 2 texel）----
    //   同树全部 primitive 共享；VS 用 texelFetch 按 joint_id 取。
    //   texel(2j)   = (pivot.xyz, parent)
    //   texel(2j+1) = (level, phase, soft, 0)
    if (!data.wind_joints.empty()) {
        const size_t n = data.wind_joints.size();
        // 纹理宽 = 2n；超过 GL_MAX_TEXTURE_SIZE（常见 16384）会变成不完整纹理，
        // texelFetch 读到垃圾（静默错）——宁可此刻崩，不静默出错。
        CHECK_LE(2 * n, 16384u) << "wind_joint 表过大（2N 超纹理宽上限）: N=" << n;
        std::vector<float> tex(8 * n);
        for (size_t k = 0; k < n; ++k) {
            const MeshData::WindJoint& wj = data.wind_joints[k];
            tex[8 * k + 0] = wj.pivot[0];
            tex[8 * k + 1] = wj.pivot[1];
            tex[8 * k + 2] = wj.pivot[2];
            tex[8 * k + 3] = wj.parent;
            tex[8 * k + 4] = wj.level;
            tex[8 * k + 5] = wj.phase;
            tex[8 * k + 6] = wj.soft;
            tex[8 * k + 7] = 0.0f;
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
    // 属性 VBO 最多 9 个（pos/normal/uv/joint/weight/tangent/relax/wind/wind_w）
    // + 1 个 EBO = 10；relax 与 wind 互斥，实际 ≤9。
    static constexpr int kMaxBuffers = 10;
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
    if (mesh->vbo_relax) {
        buffers[n++] = mesh->vbo_relax;
    }
    if (mesh->vbo_wind) {
        buffers[n++] = mesh->vbo_wind;
    }
    if (mesh->vbo_wind_w) {
        buffers[n++] = mesh->vbo_wind_w;
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
