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

bool LoadGltf(const std::string& path,
              MeshData* out_mesh,
              GltfMaterialInfo* out_mat) {
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

    // ---- 2. 定位第一个 mesh 的第一个 primitive ----
    if (model.meshes.empty()) {
        LOG(ERROR) << "LoadGltf: 没有 mesh 数据: " << path;
        return false;
    }
    const tinygltf::Mesh& mesh = model.meshes[0];
    if (mesh.primitives.empty()) {
        LOG(ERROR) << "LoadGltf: mesh 没有 primitive: " << path;
        return false;
    }
    const tinygltf::Primitive& prim = mesh.primitives[0];

    // ---- 3. 提取顶点属性 ----
    // POSITION（必有）
    auto pos_it = prim.attributes.find("POSITION");
    if (pos_it == prim.attributes.end()) {
        LOG(ERROR) << "LoadGltf: primitive 没有 POSITION 属性: " << path;
        return false;
    }
    std::vector<float> pos_flat;
    if (!ReadFloatAccessor(model, pos_it->second, &pos_flat)) {
        LOG(ERROR) << "LoadGltf: 无法读取 POSITION accessor: " << path;
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
                // glTF UV: V 轴翻转（0 = bottom → 1 = top for OpenGL）
                out_mesh->uvs[i] = Vec2f(
                    uv_flat[i * 2 + 0],
                    1.0f - uv_flat[i * 2 + 1]);
            }
            out_mesh->flags = static_cast<MeshVertexFlags>(
                static_cast<uint8_t>(out_mesh->flags) |
                static_cast<uint8_t>(MeshVertexFlags::kUV));
        }
    }

    // ---- 4. 展开索引 ----
    if (prim.indices >= 0) {
        if (!ReadIndexAccessor(model, prim.indices, &out_mesh->indices)) {
            LOG(ERROR) << "LoadGltf: 无法读取 indices accessor: " << path;
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
    // 解析 glTF 文件所在目录（用于构造相对路径）
    std::string base_dir;
    {
        const size_t last_slash = path.find_last_of("/\\");
        if (last_slash != std::string::npos) {
            base_dir = path.substr(0, last_slash + 1);
        }
    }

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
    }

    out_mesh->Validate();
    return true;
}

}  // namespace jpov
