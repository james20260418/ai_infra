// JPOV glTF 2.0 保存器实现 — 用 nlohmann::json 组 JSON chunk + 手写 GLB 头。
//
// GLB 容器（glTF 2.0 规范 §4.4.2）：
//   [12 字节头: magic 'glTF', version=2, total_length]
//   [chunk0: length, type='JSON', JSON 字节 + 0x20 空格 pad 到 4 字节对齐]
//   [chunk1: length, type='BIN\0', BIN 字节 + 0x00 pad 到 4 字节对齐]
//
// ⚠️ 对齐：chunk 的 length **不含** pad（pad 只为把下一 chunk 起点对齐到 4 字节），
//   但 total_length 含全部 pad。reader（tinygltf）依赖此约定，否则读回崩。
//
// 坐标系（2026-09-16，Danis）：**本 saver 不做任何坐标变换** —— 写出去的就是进来时的
//   坐标系（与 gltf_loader 原样透传对偶，往返恒等）。loader 与 saver 共用同一条铁律：
//   「加载/保存路径不擅自改方向」，朝哪放由消费侧 up/front 决定。
//   历史：loader 早期做过 f(X,Y,Z) = (X,−Z,Y)，本文件因此有一个互逆的逆映射；该映射
//   与"骨架侧不映射"的不对称曾导致蒙皮坐标系不匹配，两侧已一并移除。

#include "tools/jpov/src/gltf_saver.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <glog/logging.h>
#include "third_party/nlohmann-json/include/nlohmann/json.hpp"

namespace jpov {
namespace {

using nlohmann::json;

// ==================== BIN 缓冲累加器 ====================
//
// 逐 accessor 把 tightly-packed 数据 append 进一个字节缓冲，同时记录
// bufferView 的 byteOffset/byteLength。每个 bufferView 起点按 4 字节对齐
// （glTF accessor 的 componentType 对齐要求；float/ushort/uint 均 <= 4）。
struct BinBuilder {
    std::vector<unsigned char> bytes;

    // append 一段字节，返回写入起点（bufferView byteOffset 用）。
    // Pre-condition: len > 0
    size_t Append(const void* data, size_t len) {
        CHECK_GT(len, 0u) << "BinBuilder::Append: len 必须 > 0";
        // 4 字节对齐（float / uint32 要求）。
        while (bytes.size() % 4 != 0) {
            bytes.push_back(0);
        }
        const size_t offset = bytes.size();
        const unsigned char* p = static_cast<const unsigned char*>(data);
        bytes.insert(bytes.end(), p, p + len);
        return offset;
    }
};

// 把 float 数组 append 进 bin，返回 {byteOffset, byteLength}。
std::pair<size_t, size_t> AppendFloats(BinBuilder* bin,
                                       const std::vector<float>& v) {
    const size_t n = v.size() * sizeof(float);
    const size_t off = bin->Append(v.data(), n);
    return {off, n};
}

// ==================== 图片读取 / mime 判定 ====================

// 按扩展名判定 mimeType（glTF 只允许 image/png 与 image/jpeg）。
std::string MimeTypeOf(const std::string& path) {
    const size_t dot = path.find_last_of('.');
    std::string ext = (dot == std::string::npos) ? "" : path.substr(dot + 1);
    for (char& c : ext) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (ext == "png") {
        return "image/png";
    }
    if (ext == "jpg" || ext == "jpeg") {
        return "image/jpeg";
    }
    return std::string();   // 未知 → 调用方回退（按 png 处理或跳过）
}

bool ReadFileBytes(const std::string& path, std::vector<unsigned char>* out) {
    std::ifstream in(path, std::ifstream::binary);
    if (!in) {
        return false;
    }
    in.seekg(0, std::ifstream::end);
    const std::streamoff size = in.tellg();
    if (size <= 0) {
        return false;
    }
    in.seekg(0, std::ifstream::beg);
    out->resize(static_cast<size_t>(size));
    in.read(reinterpret_cast<char*>(out->data()), size);
    return static_cast<bool>(in);
}

// ==================== JSON 组装 ====================

// 把一个 primitive 的几何写入 BIN/JSON，返回 primitive 的 json 对象。
json WritePrimitive(const MeshData& mesh, BinBuilder* bin, json* accessors,
                    json* buffer_views) {
    CHECK_GT(mesh.VertexCount(), 0u) << "WritePrimitive: 顶点为空";
    const size_t vcount = mesh.VertexCount();

    // 每个 accessor 一个 bufferView（紧密排布，不共享）。
    auto add_accessor = [&](const std::string& type, int component_type,
                            size_t count, size_t byte_offset,
                            size_t byte_length, bool normalized) -> int {
        const int bv_index = static_cast<int>(buffer_views->size());
        buffer_views->push_back({{"buffer", 0},
                                 {"byteOffset", byte_offset},
                                 {"byteLength", byte_length}});
        json acc = {{"bufferView", bv_index},
                    {"componentType", component_type},
                    {"count", count},
                    {"type", type}};
        if (normalized) {
            acc["normalized"] = true;
        }
        const int acc_index = static_cast<int>(accessors->size());
        accessors->push_back(acc);
        return acc_index;
    };

    // POSITION (vec3 float)
    std::vector<float> pos;
    pos.reserve(vcount * 3);
    for (const Vec3f& p : mesh.positions) {
        pos.push_back(p.x());
        pos.push_back(p.y());
        pos.push_back(p.z());
    }
    const std::pair<size_t, size_t> o_pos = AppendFloats(bin, pos);
    const int a_pos = add_accessor("VEC3", 5126, vcount, o_pos.first,
                                   o_pos.second, false);

    json attributes;
    attributes["POSITION"] = a_pos;

    // NORMAL (vec3 float) —— 有才写（与位置同坐标系，原样写出）
    if (!mesh.normals.empty()) {
        std::vector<float> nor;
        nor.reserve(vcount * 3);
        for (const Vec3f& n : mesh.normals) {
            nor.push_back(n.x());
            nor.push_back(n.y());
            nor.push_back(n.z());
        }
        const std::pair<size_t, size_t> o = AppendFloats(bin, nor);
        attributes["NORMAL"] =
            add_accessor("VEC3", 5126, vcount, o.first, o.second, false);
    }

    // TEXCOORD_0 (vec2 float) —— 有才写
    if (!mesh.uvs.empty()) {
        std::vector<float> uv;
        uv.reserve(vcount * 2);
        for (const Vec2f& t : mesh.uvs) {
            uv.push_back(t.x());
            uv.push_back(t.y());
        }
        const std::pair<size_t, size_t> o = AppendFloats(bin, uv);
        attributes["TEXCOORD_0"] =
            add_accessor("VEC2", 5126, vcount, o.first, o.second, false);
    }

    // JOINTS_0 (vec4 ushort) + WEIGHTS_0 (vec4 float) —— 有才写
    if (MeshHasFlag(mesh.flags, MeshVertexFlags::kJoints)) {
        CHECK_EQ(mesh.joint_indices.size(), vcount);
        CHECK_EQ(mesh.joint_weights.size(), vcount);
        std::vector<unsigned short> joints;
        joints.reserve(vcount * 4);
        for (const std::array<int32_t, 4>& ji : mesh.joint_indices) {
            for (int k = 0; k < 4; ++k) {
                CHECK_GE(ji[k], 0) << "WritePrimitive: joint 索引为负";
                CHECK_LE(ji[k], 65535) << "WritePrimitive: joint 索引超 ushort";
                joints.push_back(static_cast<unsigned short>(ji[k]));
            }
        }
        const size_t jn = joints.size() * sizeof(unsigned short);
        const size_t joff = bin->Append(joints.data(), jn);
        attributes["JOINTS_0"] =
            add_accessor("VEC4", 5123, vcount, joff, jn, false);

        std::vector<float> wts;
        wts.reserve(vcount * 4);
        for (const std::array<float, 4>& w : mesh.joint_weights) {
            for (int k = 0; k < 4; ++k) {
                wts.push_back(w[k]);
            }
        }
        const std::pair<size_t, size_t> o = AppendFloats(bin, wts);
        attributes["WEIGHTS_0"] =
            add_accessor("VEC4", 5126, vcount, o.first, o.second, false);
    }

    // 布料 LAG 松弛度（**自定义语义** _JPOV_RELAX，vec2 float = (lag_ratio, max_lag)）—— 有才写。
    //   glTF 2.0 规定应用自定义属性语义必须以下划线开头；不认识该语义的查看器/引擎
    //   会直接忽略（不报错），故对通用资产无害。见 docs/jpov_instance_attr_lag_design.md。
    if (MeshHasFlag(mesh.flags, MeshVertexFlags::kRelax)) {
        CHECK_EQ(mesh.relaxations.size(), vcount);
        std::vector<float> relax;
        relax.reserve(vcount * 2);
        for (const Vec2f& r : mesh.relaxations) {
            relax.push_back(r.x());
            relax.push_back(r.y());
        }
        const std::pair<size_t, size_t> o = AppendFloats(bin, relax);
        attributes["_JPOV_RELAX"] =
            add_accessor("VEC2", 5126, vcount, o.first, o.second, false);
    }

    // indices (uint scalar) —— 有才写；无则非索引绘制（loader 支持）
    json primitive = {{"attributes", attributes}, {"mode", 4}};   // 4 = triangles
    if (!mesh.indices.empty()) {
        const size_t n = mesh.indices.size() * sizeof(uint32_t);
        const size_t off = bin->Append(mesh.indices.data(), n);
        primitive["indices"] =
            add_accessor("SCALAR", 5125, mesh.indices.size(), off, n, false);
    }
    return primitive;
}

// 内嵌一张图片到 BIN，返回 image 的 json 对象；失败返回空 json（调用方可跳过贴图）。
json EmbedImage(const std::string& path, BinBuilder* bin, json* buffer_views) {
    std::vector<unsigned char> bytes;
    if (!ReadFileBytes(path, &bytes)) {
        LOG(ERROR) << "gltf_saver: 无法读取贴图 " << path << "，跳过该贴图";
        return json();
    }
    const std::string mime = MimeTypeOf(path);
    // glTF 只允许 png/jpeg；未知扩展按 png（内容多半还是 png）。
    const std::string use_mime = mime.empty() ? "image/png" : mime;

    const size_t off = bin->Append(bytes.data(), bytes.size());
    const int bv = static_cast<int>(buffer_views->size());
    buffer_views->push_back({{"buffer", 0},
                             {"byteOffset", off},
                             {"byteLength", bytes.size()}});
    return json{{"bufferView", bv}, {"mimeType", use_mime}};
}

// 由图片字节的魔数判定 mime（内嵌图无边路可推断，看内容）。
std::string MimeTypeOfBytes(const std::vector<unsigned char>& b) {
    if (b.size() >= 8 && b[0] == 0x89 && b[1] == 0x50 && b[2] == 0x4E &&
        b[3] == 0x47) {
        return "image/png";
    }
    if (b.size() >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF) {
        return "image/jpeg";
    }
    return "image/png";  // 兜底（未知按 png）
}

// 把**已在内存里的图片字节**内嵌到 BIN，返回 image 的 json。空字节返回空 json。
//   —— 内嵌图（如从别的 glb 读出来的 bufferView）不再重编码：原样写回（保原格式）。
json EmbedImageBytes(const std::vector<unsigned char>& bytes, BinBuilder* bin,
                     json* buffer_views) {
    if (bytes.empty()) {
        return json();
    }
    const std::string use_mime = MimeTypeOfBytes(bytes);
    const size_t off = bin->Append(bytes.data(), bytes.size());
    const int bv = static_cast<int>(buffer_views->size());
    buffer_views->push_back({{"buffer", 0},
                             {"byteOffset", off},
                             {"byteLength", bytes.size()}});
    return json{{"bufferView", bv}, {"mimeType", use_mime}};
}

// ==================== 保存模式（内嵌 vs 外置纹理） ====================

// 贴图落法：内嵌进 BIN，或写成 .gltf 同目录的 sidecar 文件。
enum class TextureStorage { kEmbedded, kExternal };

// 一次保存的上下文。
struct SaveContext {
    TextureStorage storage = TextureStorage::kEmbedded;
    std::string out_dir;   // 外置：sidecar 落盘目录（含尾 '/'），空 = 当前目录
    std::string stem;      // 外置：sidecar 文件名前缀（<stem>_texN.<ext>）
};

// 一次性写文件（失败返回 false）。
bool WriteBytesToFile(const std::string& path,
                      const std::vector<unsigned char>& bytes) {
    std::ofstream f(path, std::ofstream::binary);
    if (!f) {
        return false;
    }
    f.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(f);
}

// 由图片字节魔数判定扩展名（与 MimeTypeOfBytes 同一判定；glTF 只认 png/jpeg）。
std::string ExtensionFromBytes(const std::vector<unsigned char>& b) {
    if (b.size() >= 8 && b[0] == 0x89 && b[1] == 0x50 && b[2] == 0x4E &&
        b[3] == 0x47) {
        return ".png";
    }
    if (b.size() >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF) {
        return ".jpg";
    }
    return ".png";   // 兜底（未知按 png，与 MimeTypeOfBytes 一致）
}

// 把一张贴图落成 glTF image json。
//   - 内嵌：追加进 BIN，返回 {"bufferView", "mimeType"}。
//   - 外置：把原始字节写成 <out_dir><stem>_tex<ordinal>.<ext>，返回 {"uri": "<文件名>"}。
// 空 ref / 读取失败 / 写文件失败 → 返回空 json（调用方跳过该贴图槽）。
// ordinal: 该图在 images 数组中的序号（外置文件名后缀用，保证唯一）。
json EmitImage(const GltfTextureRef& ref, const SaveContext& ctx, int ordinal,
               BinBuilder* bin, json* buffer_views) {
    if (ref.empty()) {
        return json();
    }
    if (ctx.storage == TextureStorage::kEmbedded) {
        return ref.is_embedded()
                   ? EmbedImageBytes(ref.bytes, bin, buffer_views)
                   : EmbedImage(ref.uri, bin, buffer_views);
    }
    // 外置：取原始编码字节（内存内嵌 或 读外部文件），写 sidecar，返回相对 uri。
    std::vector<unsigned char> bytes;
    if (ref.is_embedded()) {
        bytes = ref.bytes;
    } else if (!ReadFileBytes(ref.uri, &bytes)) {
        LOG(ERROR) << "gltf_saver: 无法读取贴图 " << ref.uri << "，跳过该贴图";
        return json();
    }
    if (bytes.empty()) {
        return json();
    }
    const std::string fname =
        ctx.stem + "_tex" + std::to_string(ordinal) + ExtensionFromBytes(bytes);
    if (!WriteBytesToFile(ctx.out_dir + fname, bytes)) {
        LOG(ERROR) << "gltf_saver: 无法写贴图文件 " << ctx.out_dir + fname;
        return json();
    }
    return json{{"uri", fname}};
}

// 组装 glTF JSON 根对象（几何写进 *bin；贴图按 ctx 落法处理）。
// 不含 buffers 段（内嵌/外置的 buffer 描述由调用方补），也不含容器封装。
// Pre-condition: asset.meshes 非空；各 mesh.mesh.Validate() 通过。
json BuildRoot(const GltfSaveAsset& asset, const SaveContext& ctx,
               BinBuilder* bin) {
    CHECK(!asset.meshes.empty()) << "gltf_saver: 资产没有任何 primitive";

    json root;
    root["asset"] = {{"version", "2.0"}, {"generator", "JPOV gltf_saver"}};
    root["scene"] = 0;
    root["scenes"] = json::array({json{{"nodes", json::array()}}});
    root["nodes"] = json::array();
    root["meshes"] = json::array();
    root["accessors"] = json::array();
    root["bufferViews"] = json::array();
    root["materials"] = json::array();
    root["images"] = json::array();
    root["textures"] = json::array();
    root["samplers"] = json::array({json{{"magFilter", 9729},    // LINEAR
                                         {"minFilter", 9987},    // LINEAR_MIPMAP_LINEAR
                                         {"wrapS", 10497},       // REPEAT
                                         {"wrapT", 10497}}});

    // ---- 贴图去重（同一贴图：外部路径 或 内嵌字节 只落一次）----
    std::map<std::string, int> tex_index_by_path;   // 去重键 → texture 索引
    auto texture_for = [&](const GltfTextureRef& ref) -> int {
        if (ref.empty()) {
            return -1;
        }
        const std::string key = ref.is_embedded() ? ("mem:" + ref.key) : ref.uri;
        std::map<std::string, int>::const_iterator it = tex_index_by_path.find(key);
        if (it != tex_index_by_path.end()) {
            return it->second;
        }
        // 按模式落图（内嵌进 BIN / 写成 sidecar 文件）；ordinal = 当前 images 数。
        json img = EmitImage(ref, ctx, static_cast<int>(root["images"].size()),
                             bin, &root["bufferViews"]);
        if (img.is_null() || img.empty()) {
            return -1;
        }
        const int image_index = static_cast<int>(root["images"].size());
        root["images"].push_back(img);
        const int tex_index = static_cast<int>(root["textures"].size());
        root["textures"].push_back(
            json{{"source", image_index}, {"sampler", 0}});
        tex_index_by_path[key] = tex_index;
        return tex_index;
    };

    // ---- 材质 ----
    // 返回 material 索引。-1 表示"无材质"（primitive 不写 material）。
    auto material_for = [&](const GltfMaterialInfo& mi,
                            const std::string& name) -> int {
        const bool has_any_tex = !mi.base_color_tex.empty() ||
                                 !mi.normal_tex.empty() ||
                                 !mi.metallic_roughness_tex.empty() ||
                                 !mi.occlusion_tex.empty() ||
                                 !mi.emissive_tex.empty();
        // 无任何贴图且因子全默认（白/1/1/无发光）→ 不写材质（loader 用默认）。
        const bool default_factors =
            mi.base_color[0] == 1.0f && mi.base_color[1] == 1.0f &&
            mi.base_color[2] == 1.0f && mi.base_color[3] == 1.0f &&
            mi.metallic_factor == 1.0f && mi.roughness_factor == 1.0f &&
            mi.emissive_factor[0] == 0.0f && mi.emissive_factor[1] == 0.0f &&
            mi.emissive_factor[2] == 0.0f;
        // 无任何贴图、因子全默认、且透明模式也默认（Opaque/单面）→ 不写材质。
        const bool default_alpha =
            mi.alpha_mode == AlphaMode::kOpaque && !mi.double_sided;
        if (!has_any_tex && default_factors && default_alpha) {
            return -1;
        }

        json mat;
        mat["name"] = name.empty() ? "material" : name;
        mat["pbrMetallicRoughness"] = json::object();
        mat["pbrMetallicRoughness"]["metallicFactor"] = mi.metallic_factor;
        mat["pbrMetallicRoughness"]["roughnessFactor"] = mi.roughness_factor;

        const int base_tex = texture_for(mi.base_color_tex);
        if (base_tex >= 0) {
            mat["pbrMetallicRoughness"]["baseColorTexture"] =
                json{{"index", base_tex}};
            mat["pbrMetallicRoughness"]["baseColorFactor"] =
                json::array({1.0f, 1.0f, 1.0f, 1.0f});
        } else {
            mat["pbrMetallicRoughness"]["baseColorFactor"] = json::array(
                {mi.base_color[0], mi.base_color[1], mi.base_color[2],
                 mi.base_color[3]});
        }

        const int mr_tex = texture_for(mi.metallic_roughness_tex);
        if (mr_tex >= 0) {
            mat["pbrMetallicRoughness"]["metallicRoughnessTexture"] =
                json{{"index", mr_tex}};
        }

        const int nor_tex = texture_for(mi.normal_tex);
        if (nor_tex >= 0) {
            mat["normalTexture"] = json{{"index", nor_tex},
                                        {"scale", mi.normal_scale}};
        }

        const int occ_tex = texture_for(mi.occlusion_tex);
        if (occ_tex >= 0) {
            mat["occlusionTexture"] =
                json{{"index", occ_tex}, {"strength", mi.occlusion_strength}};
        }

        const int emi_tex = texture_for(mi.emissive_tex);
        if (emi_tex >= 0) {
            mat["emissiveTexture"] = json{{"index", emi_tex}};
        }
        if (mi.emissive_factor[0] != 0.0f || mi.emissive_factor[1] != 0.0f ||
            mi.emissive_factor[2] != 0.0f) {
            mat["emissiveFactor"] = json::array({mi.emissive_factor[0],
                                                 mi.emissive_factor[1],
                                                 mi.emissive_factor[2]});
        }

        // 透明模式（alpha test / cutout）：非 Opaque 时写 alphaMode/alphaCutoff，
        // 否则“存→读”会丢掉 cutout 语义（几何不受影响，但渲染变回不透明）。
        if (mi.alpha_mode == AlphaMode::kMask) {
            mat["alphaMode"] = "MASK";
            mat["alphaCutoff"] = mi.alpha_cutoff;
        }
        // 双面渲染：true 时写 doubleSided（否则单面几何的背面会丢）。
        if (mi.double_sided) {
            mat["doubleSided"] = true;
        }

        const int mat_index = static_cast<int>(root["materials"].size());
        root["materials"].push_back(mat);
        return mat_index;
    };

    // ---- 逐 primitive：几何 + 材质，并建一个 node ----
    for (size_t i = 0; i < asset.meshes.size(); ++i) {
        const GltfSaveMesh& sm = asset.meshes[i];
        const MeshData& mesh = sm.mesh;
        mesh.Validate();

        const std::string base_name =
            asset.name.empty() ? "model" : asset.name;
        const std::string mat_name =
            base_name + "_mat" + std::to_string(i);

        json prim = WritePrimitive(mesh, bin, &root["accessors"],
                                  &root["bufferViews"]);
        const int mat_index = material_for(sm.material, mat_name);
        if (mat_index >= 0) {
            prim["material"] = mat_index;
        }

        const int mesh_index = static_cast<int>(root["meshes"].size());
        root["meshes"].push_back(
            json{{"name", base_name + "_mesh" + std::to_string(i)},
                 {"primitives", json::array({prim})}});

        json node = {{"name", base_name + "_node" + std::to_string(i)},
                     {"mesh", mesh_index}};
        if (asset.skin.has_value()) {
            node["skin"] = 0;
        }
        const int node_index = static_cast<int>(root["nodes"].size());
        root["nodes"].push_back(node);
        root["scenes"][0]["nodes"].push_back(node_index);
    }

    // ---- skin（可选）----
    if (asset.skin.has_value()) {
        const SkeletonType& sk = asset.skin.value();
        sk.Validate();
        const std::vector<std::array<float, 16>> ibm = sk.ComputeInverseBind();

        // IBM accessor：count = 关节数，MAT4 float。
        std::vector<float> ibm_flat;
        ibm_flat.reserve(ibm.size() * 16);
        for (const std::array<float, 16>& m : ibm) {
            for (int k = 0; k < 16; ++k) {
                ibm_flat.push_back(m[k]);
            }
        }
        const std::pair<size_t, size_t> o = AppendFloats(bin, ibm_flat);
        const int bv = static_cast<int>(root["bufferViews"].size());
        root["bufferViews"].push_back({{"buffer", 0},
                                       {"byteOffset", o.first},
                                       {"byteLength", o.second}});
        const int ibm_acc = static_cast<int>(root["accessors"].size());
        root["accessors"].push_back(json{{"bufferView", bv},
                                         {"componentType", 5126},
                                         {"count", sk.joints.size()},
                                         {"type", "MAT4"}});

        // 每关节一个 node（承载 rest 局部变换：translation + rotation）。
        // ⚠️ glTF 的 node 树才是骨骼树；loader 的 LoadGltfSkeleton 按 node 树读
        //   parent + translation(rest_offset) + rotation(bind_rotation)。
        //   故这里必须把骨架写成 node 树，不能省。
        const int skel_node_base = static_cast<int>(root["nodes"].size());
        for (size_t j = 0; j < sk.joints.size(); ++j) {
            const SkeletonJoint& jt = sk.joints[j];
            json n = {{"name", jt.name.empty()
                                   ? ("joint" + std::to_string(j))
                                   : jt.name}};
            n["translation"] = json::array(
                {jt.rest_offset.x(), jt.rest_offset.y(), jt.rest_offset.z()});
            if (!sk.bind_rotation.empty()) {
                const geom::Quaternion<float>& q = sk.bind_rotation[j];
                // glTF 四元数顺序 [x,y,z,w]。
                n["rotation"] = json::array({q.x, q.y, q.z, q.w});
            }
            root["nodes"].push_back(n);
        }
        // 用 parent 关系补 children（glTF 是自顶向下树）。
        for (size_t j = 0; j < sk.joints.size(); ++j) {
            const int p = sk.joints[j].parent;
            if (p == kSkeletonNoParent) {
                continue;
            }
            json& parent_node = root["nodes"][skel_node_base +
                                              static_cast<size_t>(p)];
            if (!parent_node.contains("children")) {
                parent_node["children"] = json::array();
            }
            parent_node["children"].push_back(
                skel_node_base + static_cast<int>(j));
        }
        // 找出根关节作为 skeleton 的根 node。
        std::vector<int> roots;
        for (size_t j = 0; j < sk.joints.size(); ++j) {
            if (sk.joints[j].parent == kSkeletonNoParent) {
                roots.push_back(skel_node_base + static_cast<int>(j));
            }
        }
        const int skeleton_root = roots.empty() ? skel_node_base : roots[0];

        root["skins"] = json::array();
        json skin_json = {{"inverseBindMatrices", ibm_acc},
                          {"joints", json::array()},
                          {"skeleton", skeleton_root}};
        for (size_t j = 0; j < sk.joints.size(); ++j) {
            skin_json["joints"].push_back(skel_node_base +
                                          static_cast<int>(j));
        }
        root["skins"].push_back(skin_json);

        // 骨架根 node 也挂进 scene（否则树不在场景图里）。
        for (size_t r = 0; r < roots.size(); ++r) {
            root["scenes"][0]["nodes"].push_back(roots[r]);
        }
    }

    return root;
}

}  // namespace

bool WriteGlb(const GltfSaveAsset& asset, const std::string& path) {
    CHECK(!path.empty()) << "WriteGlb: 输出路径为空";

    BinBuilder bin;
    json root = BuildRoot(asset, SaveContext{}, &bin);

    // ---- buffer：byteLength = BIN 长度（**不含** chunk pad）。----
    root["buffers"] =
        json::array({json{{"byteLength", bin.bytes.size()}}});

    // ---- 组 GLB ----
    std::string json_text = root.dump();
    // JSON chunk 必须 4 字节对齐，用空格 pad。
    while (json_text.size() % 4 != 0) {
        json_text.push_back(' ');
    }
    std::vector<unsigned char> bin_padded = bin.bytes;
    while (bin_padded.size() % 4 != 0) {
        bin_padded.push_back(0);
    }

    const uint32_t json_len = static_cast<uint32_t>(json_text.size());
    const uint32_t bin_len = static_cast<uint32_t>(bin_padded.size());
    const uint32_t total =
        12 + 8 + json_len + 8 + bin_len;   // 头 + JSON chunk(8+len) + BIN chunk(8+len)

    std::vector<unsigned char> out;
    out.reserve(total);
    auto put_u32 = [&out](uint32_t v) {
        out.push_back(static_cast<unsigned char>(v & 0xFF));
        out.push_back(static_cast<unsigned char>((v >> 8) & 0xFF));
        out.push_back(static_cast<unsigned char>((v >> 16) & 0xFF));
        out.push_back(static_cast<unsigned char>((v >> 24) & 0xFF));
    };
    put_u32(0x46546C67u);   // magic 'glTF'（小端读为 'glTF'）
    put_u32(2u);            // version
    put_u32(total);         // total length
    put_u32(json_len);
    put_u32(0x4E4F534Au);   // 'JSON'
    out.insert(out.end(), json_text.begin(), json_text.end());
    put_u32(bin_len);
    put_u32(0x004E4942u);   // 'BIN\0'
    out.insert(out.end(), bin_padded.begin(), bin_padded.end());

    // 一次性落盘（不半写）。
    std::ofstream f(path, std::ofstream::binary);
    if (!f) {
        LOG(ERROR) << "WriteGlb: 无法打开输出文件 " << path;
        return false;
    }
    f.write(reinterpret_cast<const char*>(out.data()),
            static_cast<std::streamsize>(out.size()));
    if (!f) {
        LOG(ERROR) << "WriteGlb: 写入失败 " << path;
        return false;
    }
    LOG(INFO) << "WriteGlb: " << path << " (" << out.size() << " bytes, "
              << asset.meshes.size() << " primitives"
              << (asset.skin.has_value() ? ", skinned" : "") << ")";
    return true;
}

bool WriteGltf(const GltfSaveAsset& asset, const std::string& path) {
    CHECK(!path.empty()) << "WriteGltf: 输出路径为空";
    CHECK(path.size() >= 5 && path.compare(path.size() - 5, 5, ".gltf") == 0)
        << "WriteGltf: 输出路径必须以 .gltf 结尾，got " << path;

    // 拆出目录（含尾 '/'）与去扩展名的 stem，用于 sidecar 命名。
    const size_t slash = path.find_last_of("/\\");
    const std::string dir =
        (slash == std::string::npos) ? std::string() : path.substr(0, slash + 1);
    const std::string base =
        (slash == std::string::npos) ? path : path.substr(slash + 1);
    const size_t dot = base.find_last_of('.');
    const std::string stem =
        (dot == std::string::npos || dot == 0) ? base : base.substr(0, dot);

    SaveContext ctx;
    ctx.storage = TextureStorage::kExternal;
    ctx.out_dir = dir;
    ctx.stem = stem;

    BinBuilder bin;
    json root = BuildRoot(asset, ctx, &bin);

    // 几何写独立 <stem>.bin；buffers[0] 用 uri 引用（外置）。
    const std::string bin_name = stem + ".bin";
    if (!WriteBytesToFile(dir + bin_name, bin.bytes)) {
        LOG(ERROR) << "WriteGltf: 无法写几何缓冲 " << dir + bin_name;
        return false;
    }
    root["buffers"] =
        json::array({json{{"uri", bin_name}, {"byteLength", bin.bytes.size()}}});

    // 写 .gltf（缩进 2 空格，便于人读与 git diff）。
    std::ofstream f(path, std::ofstream::binary);
    if (!f) {
        LOG(ERROR) << "WriteGltf: 无法打开输出文件 " << path;
        return false;
    }
    const std::string text = root.dump(2);
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!f) {
        LOG(ERROR) << "WriteGltf: 写入失败 " << path;
        return false;
    }
    LOG(INFO) << "WriteGltf: " << path << " (+" << bin_name << ", "
              << asset.meshes.size() << " primitives"
              << (asset.skin.has_value() ? ", skinned" : "") << ")";
    return true;
}

}  // namespace jpov
