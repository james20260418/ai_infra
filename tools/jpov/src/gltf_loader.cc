// JPOV glTF 2.0 加载器实现 — .gltf/.glb → MeshData + 材质贴图
//
// 基于 tinygltf（header-only）解析 glTF JSON / GLB binary。
// 实现在本 cc 文件中通过 #define TINYGLTF_IMPLEMENTATION 包含 tiny_gltf.h，
// 其余文件只需 include 本模块的 gltf_loader.h。
//
// 加载流程：
//   1. tinygltf::TinyGLTF::LoadASCIIFromFile / LoadBinaryFromFile 解析
//   2. 定位第一个 mesh → 第一个 primitive
//   3. 提取 POSITION / NORMAL / TEXCOORD_0 accessor → 顶点数组
//   4. 展开索引缓冲（若有 indices accessor）
//   5. 坐标系变换：glTF Y-up → JPOV Z-up（交换 y/z）
//   6. 推导 tangent（复用 OBJ loader 的 ComputeTangents）
//   7. 提取材质贴图路径（相对于 glTF 文件目录）

#define TINYGLTF_NO_STB_IMAGE_WRITE
#define TINYGLTF_IMPLEMENTATION
// tinygltf 自带 JSON 解析器（nlohmann/json.hpp 内置），无需外部依赖
// stb_image 由 JPOV 已有的 //third_party/stb:stb_image 提供
// TINYGLTF_NO_STB_IMAGE_WRITE 禁用写贴图功能（本 loader 只需加载）

#include "tools/jpov/src/gltf_loader.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <glog/logging.h>

#include "third_party/tinygltf/tiny_gltf.h"

namespace jpov {

// ==================== 辅助函数 ====================

// 从 tinygltf accessor 读取 float 类型数据到 vector。
//
// 返回 true 表示成功读取 count 个 float 到 out。
// 返回 false 表示 accessor 类型不是 FLOAT / bufferView 不存在 / 数据不足。
//
// Pre-condition: model 已成功加载
bool ReadFloatAccessor(const tinygltf::Model& model,
                       int accessor_index,
                       std::vector<float>* out) {
    if (accessor_index < 0 ||
        accessor_index >= static_cast<int>(model.accessors.size())) {
        return false;
    }
    const tinygltf::Accessor& acc = model.accessors[accessor_index];
    if (acc.bufferView < 0 ||
        acc.bufferView >= static_cast<int>(model.bufferViews.size())) {
        return false;
    }
    const tinygltf::BufferView& view = model.bufferViews[acc.bufferView];
    if (view.buffer < 0 ||
        view.buffer >= static_cast<int>(model.buffers.size())) {
        return false;
    }
    const tinygltf::Buffer& buf = model.buffers[view.buffer];
    const size_t count = acc.count;
    const size_t byte_stride = view.byteStride > 0
        ? view.byteStride
        : tinygltf::GetComponentSizeInBytes(acc.componentType) *
          tinygltf::GetNumComponentsInType(acc.type);

    out->resize(count * tinygltf::GetNumComponentsInType(acc.type));
    const unsigned char* src = buf.data.data() + view.byteOffset + acc.byteOffset;
    for (size_t i = 0; i < count; ++i) {
        const void* elem = src + i * byte_stride;
        const int num_comp = tinygltf::GetNumComponentsInType(acc.type);
        for (int j = 0; j < num_comp; ++j) {
            float val = 0.0f;
            switch (acc.componentType) {
                case TINYGLTF_COMPONENT_TYPE_FLOAT:
                    val = reinterpret_cast<const float*>(elem)[j];
                    break;
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
                    val = static_cast<float>(
                        reinterpret_cast<const unsigned char*>(elem)[j]) /
                        255.0f;
                    break;
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
                    val = static_cast<float>(
                        reinterpret_cast<const unsigned short*>(elem)[j]) /
                        65535.0f;
                    break;
                case TINYGLTF_COMPONENT_TYPE_SHORT:
                    val = static_cast<float>(
                        reinterpret_cast<const short*>(elem)[j]);
                    if (val < 0.0f) {
                        val = val / 32768.0f;  // normalize signed short
                    } else {
                        val = val / 32767.0f;
                    }
                    break;
                case TINYGLTF_COMPONENT_TYPE_BYTE:
                    val = std::max(
                        static_cast<float>(
                            reinterpret_cast<const char*>(elem)[j]) / 127.0f,
                        -1.0f);
                    break;
                default:
                    return false;
            }
            (*out)[i * num_comp + j] = val;
        }
    }
    return true;
}

// 从 tinygltf accessor 读取 unsigned int 索引数据到 vector。
//
// 支持 UNSIGNED_BYTE / UNSIGNED_SHORT / UNSIGNED_INT 三种索引类型。
// 返回 false 表示索引格式不支持或数据不可读。
bool ReadIndexAccessor(const tinygltf::Model& model,
                       int accessor_index,
                       std::vector<uint32_t>* out) {
    if (accessor_index < 0 ||
        accessor_index >= static_cast<int>(model.accessors.size())) {
        return false;
    }
    const tinygltf::Accessor& acc = model.accessors[accessor_index];
    if (acc.bufferView < 0 ||
        acc.bufferView >= static_cast<int>(model.bufferViews.size())) {
        return false;
    }
    const tinygltf::BufferView& view = model.bufferViews[acc.bufferView];
    if (view.buffer < 0 ||
        view.buffer >= static_cast<int>(model.buffers.size())) {
        return false;
    }
    const tinygltf::Buffer& buf = model.buffers[view.buffer];
    const size_t count = acc.count;
    out->resize(count);
    const size_t byte_stride = view.byteStride > 0
        ? view.byteStride
        : tinygltf::GetComponentSizeInBytes(acc.componentType);
    const unsigned char* src = buf.data.data() + view.byteOffset + acc.byteOffset;

    for (size_t i = 0; i < count; ++i) {
        const void* elem = src + i * byte_stride;
        switch (acc.componentType) {
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
                (*out)[i] = static_cast<uint32_t>(
                    *reinterpret_cast<const unsigned char*>(elem));
                break;
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
                (*out)[i] = static_cast<uint32_t>(
                    *reinterpret_cast<const unsigned short*>(elem));
                break;
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
                (*out)[i] = *reinterpret_cast<const uint32_t*>(elem);
                break;
            default:
                return false;
        }
    }
    return true;
}

// 从三角形几何 + UV 推导逐顶点切线（与 OBJ loader 等价）。
//
// 本函数直接复用 obj_loader.cc 内 ComputeTangents 的算法逻辑。
// 因为 ComputeTangents 在 obj_loader.cc 中是匿名命名空间的函数，
// 不可跨 TU 调用，故在此独立实现。
//
// 算法与 obj_loader.cc 一致，保证切线推导行为完全相同。
bool ComputeTangentsGltf(MeshData* out) {
    const size_t vcount = out->positions.size();
    if (out->indices.empty() || vcount == 0) {
        return false;
    }

    std::vector<Vec3f> accum(vcount, Vec3f(0, 0, 0));
    const uint32_t num_tris = static_cast<uint32_t>(out->indices.size() / 3);
    for (uint32_t t = 0; t < num_tris; ++t) {
        const uint32_t i0 = out->indices[t * 3 + 0];
        const uint32_t i1 = out->indices[t * 3 + 1];
        const uint32_t i2 = out->indices[t * 3 + 2];
        CHECK_LT(i0, vcount);
        CHECK_LT(i1, vcount);
        CHECK_LT(i2, vcount);

        const Vec3f p0 = out->positions[i0];
        const Vec3f p1 = out->positions[i1];
        const Vec3f p2 = out->positions[i2];
        const Vec2f uv0 = out->uvs[i0];
        const Vec2f uv1 = out->uvs[i1];
        const Vec2f uv2 = out->uvs[i2];

        const Vec3f edge1 = {p1.x() - p0.x(), p1.y() - p0.y(), p1.z() - p0.z()};
        const Vec3f edge2 = {p2.x() - p0.x(), p2.y() - p0.y(), p2.z() - p0.z()};
        const float duv1x = uv1.x() - uv0.x();
        const float duv1y = uv1.y() - uv0.y();
        const float duv2x = uv2.x() - uv0.x();
        const float duv2y = uv2.y() - uv0.y();

        const float det = duv1x * duv2y - duv2x * duv1y;
        if (std::fabs(det) < 1e-8f) {
            continue;
        }
        const float r = 1.0f / det;
        Vec3f tang = {r * (duv2y * edge1.x() - duv1y * edge2.x()),
                      r * (duv2y * edge1.y() - duv1y * edge2.y()),
                      r * (duv2y * edge1.z() - duv1y * edge2.z())};
        const float len = std::sqrt(tang.x() * tang.x() +
                                    tang.y() * tang.y() +
                                    tang.z() * tang.z());
        if (len < 1e-8f) {
            continue;
        }
        const Vec3f tn = {tang.x() / len, tang.y() / len, tang.z() / len};
        accum[i0] = {accum[i0].x() + tn.x(), accum[i0].y() + tn.y(), accum[i0].z() + tn.z()};
        accum[i1] = {accum[i1].x() + tn.x(), accum[i1].y() + tn.y(), accum[i1].z() + tn.z()};
        accum[i2] = {accum[i2].x() + tn.x(), accum[i2].y() + tn.y(), accum[i2].z() + tn.z()};
    }

    out->tangents.resize(vcount);
    bool any_valid = false;
    for (size_t i = 0; i < vcount; ++i) {
        const Vec3f& a = accum[i];
        const float len = std::sqrt(a.x() * a.x() + a.y() * a.y() + a.z() * a.z());
        if (len < 1e-8f) {
            out->tangents[i] = Vec3f(0, 0, 0);
        } else {
            out->tangents[i] = {a.x() / len, a.y() / len, a.z() / len};
            any_valid = true;
        }
    }
    return any_valid;
}

// ==================== LoadGltf 实现 ====================

// 解析单个 primitive → CPU MeshData + 材质贴图路径。
//
// 内部共享逻辑：LoadGltf（取第一个）与 LoadGltfScene（取全部）共用。
// 解析流程（纯 CPU，无 GL）：
//   1. 提取 POSITION / NORMAL / TEXCOORD_0 accessor
//   2. 索引缓冲展开
//   3. Y-up → Z-up 坐标变换
//   4. 推导 tangent
//   5. 提取材质贴图路径（相对于 glTF 文件目录）
bool ParsePrimitive(const tinygltf::Model& model,
                    const tinygltf::Primitive& prim,
                    const std::string& base_dir,
                    MeshData* out_mesh,
                    GltfMaterialInfo* out_mat) {
    // ---- 3. 提取顶点属性 ----
    // POSITION（必有）
    auto pos_it = prim.attributes.find("POSITION");
    if (pos_it == prim.attributes.end()) {
        LOG(ERROR) << "LoadGltf: primitive 没有 POSITION 属性";
        return false;
    }
    std::vector<float> pos_flat;
    if (!ReadFloatAccessor(model, pos_it->second, &pos_flat)) {
        LOG(ERROR) << "LoadGltf: 无法读取 POSITION accessor";
        return false;
    }
    const size_t vcount = pos_flat.size() / 3;

    // 转为 Vec3f 并做坐标系变换：glTF Y-up → JPOV Z-up（交换 y, z）
    out_mesh->positions.resize(vcount);
    for (size_t i = 0; i < vcount; ++i) {
        const float gx = pos_flat[i * 3 + 0];
        const float gy = pos_flat[i * 3 + 1];  // glTF Y
        const float gz = pos_flat[i * 3 + 2];  // glTF Z
        // JPOV: +Y = up, +Z = front
        // glTF Y-up → JPOV: keep X, glTF Y→JPOV Z, glTF Z→JPOV -Y
        out_mesh->positions[i] = Vec3f(gx, -gz, gy);
    }

    out_mesh->flags = MeshVertexFlags::kPosition;

    // NORMAL
    auto nrm_it = prim.attributes.find("NORMAL");
    if (nrm_it != prim.attributes.end()) {
        std::vector<float> nrm_flat;
        if (ReadFloatAccessor(model, nrm_it->second, &nrm_flat) &&
            nrm_flat.size() / 3 == vcount) {
            out_mesh->normals.resize(vcount);
            for (size_t i = 0; i < vcount; ++i) {
                const float nx = nrm_flat[i * 3 + 0];
                const float ny = nrm_flat[i * 3 + 1];
                const float nz = nrm_flat[i * 3 + 2];
                out_mesh->normals[i] = Vec3f(nx, -nz, ny);
            }
            out_mesh->flags = static_cast<MeshVertexFlags>(
                static_cast<uint8_t>(out_mesh->flags) |
                static_cast<uint8_t>(MeshVertexFlags::kNormal));
        }
    }

    // TEXCOORD_0
    auto uv_it = prim.attributes.find("TEXCOORD_0");
    if (uv_it != prim.attributes.end()) {
        std::vector<float> uv_flat;
        if (ReadFloatAccessor(model, uv_it->second, &uv_flat) &&
            uv_flat.size() / 2 == vcount) {
            out_mesh->uvs.resize(vcount);
            for (size_t i = 0; i < vcount; ++i) {
                // glTF 规范: TEXCOORD_0 的 UV 原点 (0,0) 对应图片左上角
                // （upper-left, V=0 = top），与 JPOV 的纹理采样约定一致
                // （stbi_load 不翻转上传，V=0 采样图片顶部，V 向下增大）。
                // 因此 glTF UV 直接透传，无需翻转。
                // （对照: OBJ vt 是 V=0=bottom，所以 OBJ 路径实际需要翻转，
                //   但那是既存行为，本 PR 不改。此处仅对齐 glTF 规范。）
                out_mesh->uvs[i] = Vec2f(
                    uv_flat[i * 2 + 0],
                    uv_flat[i * 2 + 1]);
            }
            out_mesh->flags = static_cast<MeshVertexFlags>(
                static_cast<uint8_t>(out_mesh->flags) |
                static_cast<uint8_t>(MeshVertexFlags::kUV));
        }
    }

    // ---- 4. 展开索引 ----
    if (prim.indices >= 0) {
        if (!ReadIndexAccessor(model, prim.indices, &out_mesh->indices)) {
            LOG(ERROR) << "LoadGltf: 无法读取 indices accessor";
            return false;
        }
    }

    // 验证数据对齐
    out_mesh->Validate();

    // ---- 5. 推导 tangent ----
    if (MeshHasFlag(out_mesh->flags, MeshVertexFlags::kNormal) &&
        MeshHasFlag(out_mesh->flags, MeshVertexFlags::kUV)) {
        const bool derived = ComputeTangentsGltf(out_mesh);
        if (derived) {
            out_mesh->flags = static_cast<MeshVertexFlags>(
                static_cast<uint8_t>(out_mesh->flags) |
                static_cast<uint8_t>(MeshVertexFlags::kTangent));
        }
    }

    // ---- 6. 提取材质贴图路径 ----
    if (prim.material >= 0 &&
        prim.material < static_cast<int>(model.materials.size())) {
        const tinygltf::Material& mat = model.materials[prim.material];

        // baseColorTexture
        if (mat.pbrMetallicRoughness.baseColorTexture.index >= 0) {
            const tinygltf::Texture& tex =
                model.textures[mat.pbrMetallicRoughness.baseColorTexture.index];
            if (tex.source >= 0) {
                out_mat->base_color_tex = base_dir +
                    model.images[tex.source].uri;
            }
        }

        // normalTexture
        if (mat.normalTexture.index >= 0) {
            const tinygltf::Texture& tex =
                model.textures[mat.normalTexture.index];
            if (tex.source >= 0) {
                out_mat->normal_tex = base_dir +
                    model.images[tex.source].uri;
                out_mat->normal_scale = mat.normalTexture.scale;
            }
        }

        // metallicRoughnessTexture (ORM 三合一打包)
        if (mat.pbrMetallicRoughness.metallicRoughnessTexture.index >= 0) {
            const tinygltf::Texture& tex =
                model.textures[mat.pbrMetallicRoughness.metallicRoughnessTexture.index];
            if (tex.source >= 0) {
                out_mat->metallic_roughness_tex = base_dir +
                    model.images[tex.source].uri;
            }
        }

        // occlusionTexture
        if (mat.occlusionTexture.index >= 0) {
            const tinygltf::Texture& tex =
                model.textures[mat.occlusionTexture.index];
            if (tex.source >= 0) {
                out_mat->occlusion_tex = base_dir +
                    model.images[tex.source].uri;
            }
        }

        // emissiveTexture
        if (mat.emissiveTexture.index >= 0) {
            const tinygltf::Texture& tex =
                model.textures[mat.emissiveTexture.index];
            if (tex.source >= 0) {
                out_mat->emissive_tex = base_dir +
                    model.images[tex.source].uri;
            }
        }

        // 常值 fallback
        out_mat->metallic_factor = static_cast<float>(
            mat.pbrMetallicRoughness.metallicFactor);
        out_mat->roughness_factor = static_cast<float>(
            mat.pbrMetallicRoughness.roughnessFactor);

        // baseColorFactor: 常值 base 色（纯色材质用）
        if (mat.pbrMetallicRoughness.baseColorFactor.size() >= 4) {
            for (int i = 0; i < 4; ++i) {
                out_mat->base_color[i] = static_cast<float>(
                    mat.pbrMetallicRoughness.baseColorFactor[i]);
            }
        }

        // emissiveFactor: 常值自发光色
        if (mat.emissiveFactor.size() >= 3) {
            for (int i = 0; i < 3; ++i) {
                out_mat->emissive_factor[i] = static_cast<float>(
                    mat.emissiveFactor[i]);
            }
        }
    }

    out_mesh->Validate();
    return true;
}

// ==================== 节点变换（场景图 TRS）====================

// 从场景图构建 mesh_index → 世界变换矩阵（三维线性部分，无平移；
// 平移会破坏"模型局部空间"语义，DrawObject3D 用 center 定位）。
//
// glTF 场景图: Node 可含 mesh + TRS (translation/rotation/scale)，
// 也可嵌套子节点。模型顶点在 mesh 局部空间，需经引用它的 node 的
// 累积变换(含父节点)才到场景空间。很多第三方模型靠 node scale 放大
// 微小顶点(如 poly.pizza scale=100)。
//
// 本 loader 只应用"旋转 + 缩放"（线性部分），不做平移：
// 平移让调用方 DrawGltfObject(center) 重新定位更自然。
struct Mat3 {
    float m[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};  // 行主序
};

void Mat3MulVec(const Mat3& mat, const std::vector<float>& in,
                std::vector<float>* out) {
    out->resize(in.size());
    for (size_t i = 0; i + 2 < in.size(); i += 3) {
        const float* m = mat.m;
        const float x = in[i], y = in[i + 1], z = in[i + 2];
        (*out)[i]     = m[0] * x + m[1] * y + m[2] * z;
        (*out)[i + 1] = m[3] * x + m[4] * y + m[5] * z;
        (*out)[i + 2] = m[6] * x + m[7] * y + m[8] * z;
    }
}

void Mat3MulMat3(const Mat3& a, const Mat3& b, Mat3& c) {
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            c.m[i * 3 + j] = 0;
            for (int k = 0; k < 3; ++k)
                c.m[i * 3 + j] += a.m[i * 3 + k] * b.m[k * 3 + j];
        }
}

// 把 node 的 TRS 转成 3x3 矩阵（旋转部分 × 缩放）。
void NodeTRS2Mat3(const tinygltf::Node& node, Mat3& m) {
    for (int i = 0; i < 9; ++i) m.m[i] = 0;
    m.m[0] = m.m[4] = m.m[8] = 1.0f;

    float sx = 1.0f, sy = 1.0f, sz = 1.0f;
    if (node.scale.size() >= 3) { sx = node.scale[0]; sy = node.scale[1]; sz = node.scale[2]; }

    // 旋转 (quaternion) → 3x3
    Mat3 rot; for (int i = 0; i < 9; ++i) rot.m[i] = 0;
    rot.m[0] = rot.m[4] = rot.m[8] = 1.0f;
    if (node.rotation.size() >= 4) {
        const float qx = node.rotation[0], qy = node.rotation[1],
                    qz = node.rotation[2], qw = node.rotation[3];
        rot.m[0] = 1 - 2 * (qy * qy + qz * qz);
        rot.m[1] = 2 * (qx * qy - qz * qw);
        rot.m[2] = 2 * (qx * qz + qy * qw);
        rot.m[3] = 2 * (qx * qy + qz * qw);
        rot.m[4] = 1 - 2 * (qx * qx + qz * qz);
        rot.m[5] = 2 * (qy * qz - qx * qw);
        rot.m[6] = 2 * (qx * qz - qy * qw);
        rot.m[7] = 2 * (qy * qz + qx * qw);
        rot.m[8] = 1 - 2 * (qx * qx + qy * qy);
    }

    // M = S · R  (先旋转再缩放: 顶点 v → S * R * v)
    // 缩放是对角的: diag(sx, sy, sz) · R
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            const float s = (i == 0) ? sx : ((i == 1) ? sy : sz);
            m.m[i * 3 + j] = rot.m[i * 3 + j] * s;
        }
}

// 构建 mesh_index → 线性变换矩阵。parent_map 记录 node→parent node。
void BuildMeshTransforms(const tinygltf::Model& model,
                         std::vector<Mat3>* out_trans) {
    out_trans->assign(model.meshes.size(), Mat3{});

    // node_index → parent node index
    std::vector<int> parent(model.nodes.size(), -1);
    for (size_t n = 0; n < model.nodes.size(); ++n)
        for (int ch : model.nodes[n].children)
            if (ch >= 0 && ch < static_cast<int>(model.nodes.size()))
                parent[ch] = static_cast<int>(n);

    for (size_t n = 0; n < model.nodes.size(); ++n) {
        Mat3 acc{};
        int cur = static_cast<int>(n);
        std::vector<Mat3> chain;
        while (cur >= 0) { Mat3 m; NodeTRS2Mat3(model.nodes[cur], m); chain.push_back(m); cur = parent[cur]; }
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            Mat3 t; Mat3MulMat3(acc, *it, t); acc = t;
        }
        const int mesh_id = model.nodes[n].mesh;
        if (mesh_id >= 0 && mesh_id < static_cast<int>(model.meshes.size())) {
            (*out_trans)[mesh_id] = acc;
        }
    }
}

// 解析 glTF 文件 → 场景中所有 primitive，通过 cb 逐条交付。
// 返回 false 表示文件无法加载或解析失败。
bool LoadGltfImpl(const std::string& path,
                  GltfMeshEntryCallback cb,
                  void* user_data) {
    CHECK(!path.empty());

    // ---- 1. 解析 glTF 文件 ----
    tinygltf::TinyGLTF loader;
    tinygltf::Model model;
    std::string err, warn;

    bool ok = false;
    // 根据扩展名选择 ASCII (.gltf) 还是 Binary (.glb)
    if (path.size() >= 4 &&
        path.compare(path.size() - 4, 4, ".glb") == 0) {
        ok = loader.LoadBinaryFromFile(&model, &err, &warn, path);
    } else {
        ok = loader.LoadASCIIFromFile(&model, &err, &warn, path);
    }

    if (!warn.empty()) {
        LOG(WARNING) << "LoadGltf: " << path << " — " << warn;
    }
    if (!ok) {
        LOG(ERROR) << "LoadGltf: 无法加载 " << path << " — " << err;
        return false;
    }

    // 解析 glTF 文件所在目录（用于构造相对路径）
    std::string base_dir;
    {
        const size_t last_slash = path.find_last_of("/\\");
        if (last_slash != std::string::npos) {
            base_dir = path.substr(0, last_slash + 1);
        }
    }

    // ---- 2. 构建场景图节点变换（mesh → 线性变换）----
    // 先于 primitive 遍历构建，供下面应用到顶点。
    std::vector<Mat3> mesh_trans;
    BuildMeshTransforms(model, &mesh_trans);

    // ---- 3. 遍历所有 mesh → 所有 primitive ----
    bool delivered_any = false;
    for (size_t mi = 0; mi < model.meshes.size(); ++mi) {
        const tinygltf::Mesh& mesh = model.meshes[mi];
        for (const tinygltf::Primitive& prim : mesh.primitives) {
            GltfMeshEntry entry;
            if (!ParsePrimitive(model, prim, base_dir,
                                &entry.mesh, &entry.material)) {
                LOG(ERROR) << "LoadGltf: 解析 primitive 失败 (mesh='"
                           << mesh.name << "')";
                return false;
            }
            // 应用 mesh 的节点变换（旋转+缩放，无平移）到 position/normal
            if (mi < mesh_trans.size()) {
                const Mat3& t = mesh_trans[mi];
                std::vector<float> pos_flat, nrm_flat;
                // positions: Vec3f → 数组 → 变换 → 写回
                pos_flat.resize(entry.mesh.positions.size() * 3);
                for (size_t i = 0; i < entry.mesh.positions.size(); ++i) {
                    pos_flat[i*3+0] = entry.mesh.positions[i].x();
                    pos_flat[i*3+1] = entry.mesh.positions[i].y();
                    pos_flat[i*3+2] = entry.mesh.positions[i].z();
                }
                std::vector<float> pos_out;
                Mat3MulVec(t, pos_flat, &pos_out);
                for (size_t i = 0; i < entry.mesh.positions.size(); ++i) {
                    entry.mesh.positions[i] = Vec3f(pos_out[i*3+0], pos_out[i*3+1], pos_out[i*3+2]);
                }
                // normals: 应用线性变换后归一化（忽略非均匀缩放的正确逆转置，够用）
                if (!entry.mesh.normals.empty()) {
                    nrm_flat.resize(entry.mesh.normals.size() * 3);
                    for (size_t i = 0; i < entry.mesh.normals.size(); ++i) {
                        nrm_flat[i*3+0] = entry.mesh.normals[i].x();
                        nrm_flat[i*3+1] = entry.mesh.normals[i].y();
                        nrm_flat[i*3+2] = entry.mesh.normals[i].z();
                    }
                    std::vector<float> nrm_out;
                    Mat3MulVec(t, nrm_flat, &nrm_out);
                    for (size_t i = 0; i < entry.mesh.normals.size(); ++i) {
                        Vec3f n(nrm_out[i*3+0], nrm_out[i*3+1], nrm_out[i*3+2]);
                        const float len = std::sqrt(n.x()*n.x()+n.y()*n.y()+n.z()*n.z());
                        if (len > 1e-8f) n = Vec3f(n.x()/len, n.y()/len, n.z()/len);
                        entry.mesh.normals[i] = n;
                    }
                }
            }
            if (cb) {
                cb(&entry, user_data);
            }
            delivered_any = true;
        }
    }

    if (!delivered_any) {
        LOG(ERROR) << "LoadGltf: 场景没有任何可解析的 primitive: " << path;
        return false;
    }
    return true;
}

bool LoadGltfScene(const std::string& path,
                   GltfMeshEntryCallback cb,
                   void* user_data) {
    CHECK(cb != nullptr);
    return LoadGltfImpl(path, cb, user_data);
}

bool LoadGltf(const std::string& path,
              MeshData* out_mesh,
              GltfMaterialInfo* out_mat) {
    CHECK(out_mesh != nullptr);
    CHECK(out_mat != nullptr);

    // 兼容行为：LoadGltf 只取第一个 primitive。用 user_data 记录是否已取。
    struct Captured {
        MeshData* mesh;
        GltfMaterialInfo* mat;
        bool got = false;
    } cap{out_mesh, out_mat};

    auto first_cb = [](const GltfMeshEntry* entry, void* data) {
        Captured* c = static_cast<Captured*>(data);
        if (!c->got) {
            c->got = true;
            *c->mesh = std::move(entry->mesh);
            *c->mat = entry->material;
        }
    };
    if (!LoadGltfImpl(path, first_cb, &cap) || !cap.got) {
        return false;
    }
    return true;
}

}  // namespace jpov
