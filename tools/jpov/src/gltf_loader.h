// JPOV glTF 2.0 加载器 — glTF/GLB → MeshData + 材质贴图
//
// 把 glTF 2.0 (.gltf / .glb) 解析为 CPU 侧几何和材质的贴图路径，
// 供 Renderer::LoadGltf 注册纹理后构建 PBRMaterial 使用。
// 本 loader 保持纯净：只产出 CPU 侧数据，不接触 GL / 渲染。
//
// 功能范围（聚焦静态 PBR 模型）：
//   - 顶点数据: POSITION / NORMAL / TEXCOORD_0
//   - 索引缓冲: 展开为扁平顶点数组和 index list（triangle list）
//   - 自动推导 tangent（从三角形几何 + UV，与 OBJ loader 一致）
//   - 多 mesh / 多 primitive：LoadGltfScene 遍历整个场景
//   - PBR 材质贴图路径提取: baseColor / normal / metallicRoughness(ORM) /
//     occlusion / emissive
//   - 贴图来源: 外部文件（image.uri 相对路径）与内嵌 bufferView
//     （GLB 单文件内嵌 PNG/JPEG）均支持。内嵌图导出到 /tmp/jpov_gltf_embed/
//     临时文件后由下游 TextureManager 加载，下游接口保持不变。
//
// 明确不支持（超出本轮范围）：
//   - 骨骼动画: skins / joints / weights（tinygltf 可读，本 loader 跳过）
//   - 动画: animations 通道
//   - 节点层级变换 / 场景图（model.nodes 的 translation/rotation/scale 未应用）
//   - 扩展材质: KHR_materials_pbrSpecularGlossiness / KHR_materials_transmission
//   - 顶点颜色: COLOR_0
//   - sparse accessor: tinygltf 内部已展开，本 loader 无需额外处理
//
// 坐标系：
//   glTF 用 Y-up，JPOV 用 Z-up（OBJ loader 同）。本 loader 在顶点阶段
//   将 Y-up → Z-up 变换: 交换 y/z 分量，无缩放。
//
// UV 约定：
//   glTF 规范: TEXCOORD_0 原点 (0,0) = 图片左上角，V 向下增大，
//   与 JPOV 纹理采样（stbi_load 不翻转上传，V=0=顶）一致，故直接透传不翻转。
//
// ORM 解包：
//   glTF 的 metallicRoughnessTexture 是 ORM (Occlusion-Roughness-Metallic)
//   三合一打包（R=AO, G=Roughness, B=Metallic）。本 loader 只提取原图路径，
//   由 Renderer::LoadGltf 在 CPU 拆包为 3 张独立灰度图。

#ifndef JPOV_SRC_GLTF_LOADER_H_
#define JPOV_SRC_GLTF_LOADER_H_

#include <string>
#include <vector>

#include "tools/jpov/interface/mesh.h"

namespace jpov {

// 从 glTF 材质中提取的贴图路径信息。
//
// 所有路径为相对于 glTF 文件所在目录的路径（或空表示无对应贴图）。
// Renderer::LoadGltf 用这些路径经 TextureManager 注册贴图，再填入
// PBRMaterial 的对应 *_tex 字段。
//
// metallic_roughness_tex: glTF 的 metallicRoughnessTexture（ORM 三合一，
//   R=AO / G=Roughness / B=Metallic）。由 Renderer::LoadGltf 在 CPU 拆包为
//   3 张独立灰度图，分别绑到 PBRMaterial 的 ao_tex / roughness_tex /
//   metallic_tex。occlusion_tex / emissive_tex 预留（当前 ORM 已含 AO；
//   若 glTF 单独指定 occlusionTexture 则应优先用它）。
struct GltfMaterialInfo {
    std::string base_color_tex;         // baseColorTexture 路径（或空）
    std::string normal_tex;             // normalTexture 路径（或空）
    std::string metallic_roughness_tex; // metallicRoughnessTexture (ORM) 路径（或空）
    std::string occlusion_tex;          // occlusionTexture 路径（或空）
    std::string emissive_tex;           // emissiveTexture 路径（或空）

    // 常值 fallback（纹理不存在时使用）
    float metallic_factor = 1.0f;
    float roughness_factor = 1.0f;
    float normal_scale = 1.0f;

    // occlusionStrength: glTF occlusionTexture 的强度，ao = mix(1, R, strength)。
    // 由加载端读取，并在 ORM/AO 拆包时烘焙进像素（渲染管线不额外处理，
    // PRBMaterial 不新增字段）。默认 1.0 = 全强度（规范的默认语义）。
    float occlusion_strength = 1.0f;

    // baseColorFactor: 常值 base 色 (RGBA, [0,1])。
    // 仅当无 baseColorTexture 时用（纯色材质，如 poly.pizza 家具）。
    // 默认白色；有纹理时忽略。
    float base_color[4] = {1.0f, 1.0f, 1.0f, 1.0f};

    // emissiveFactor: 常值自发光色 (RGB, [0,1])。
    // 默认黑色（无自发光）。
    float emissive_factor[3] = {0.0f, 0.0f, 0.0f};
};

// 加载 glTF 2.0 (.gltf 或 .glb) 文件中的第一个 mesh/primitve。
//
// 成功: 返回 true。out_mesh 填充顶点数据（positions/normals/uvs + tangents +
//        indices），out_mat 填充贴图路径。
// 失败: 返回 false 并 LOG(ERROR)。out_mesh / out_mat 保持未定义。
//
// Pre-condition: path 非空，指向有效的 .gltf 或 .glb 文件
bool LoadGltf(const std::string& path,
              MeshData* out_mesh,
              GltfMaterialInfo* out_mat);

// 一个 glTF scene 中的单个 primitive：一份 CPU 几何 + 一份材质贴图路径。
// LoadGltfScene 会为场景中每个 (mesh, primitive) 产出一条。
//
// 注意：MeshData 含 std::vector<std::array<..,4>> 骨髑数组（mesh.h），
// 虽然现在可拷贝，但拷贝开销大且易错；LoadGltfScene 仍采用回调逐条
// 交付，避免把 MeshData 放进 std::vector 反复拷贝。
struct GltfMeshEntry {
    MeshData mesh;
    GltfMaterialInfo material;
};

// 逐 primitive 交付回调：每条 (mesh, material) 调用一次。
// 由调用方决定如何处理（如立即 RegisterMesh / RegisterTexture）。
// entry 非 const：调用方可 std::move 走 mesh（MeshData 不易拷贝）。
using GltfMeshEntryCallback = void (*)(const GltfMeshEntry* entry,
                                       void* user_data);

// 加载 glTF 2.0 场景中【所有】mesh/primitive（多 mesh 支持）。
//
// 与 LoadGltf（只取第一个）不同，本函数遍历整个场景，把每个 primitive
// 解析为一条 GltfMeshEntry，并通过 cb 逐条交付（回调模式，避免把含
// 骨髑数组的 MeshData 放进 std::vector 拷贝）。这是 Renderer::LoadGltf
// 的内部数据源；本 loader 保持纯净（无 GL/无渲染），只产出 CPU 侧几何。
//
// 返回 true 表示解析成功（至少交付了一条）；false 表示失败。
bool LoadGltfScene(const std::string& path,
                   GltfMeshEntryCallback cb,
                   void* user_data);

}  // namespace jpov

#endif  // JPOV_SRC_GLTF_LOADER_H_
