// JPOV glTF 2.0 加载器 — glTF/GLB → MeshData + 材质贴图
//
// 把 glTF 2.0 (.gltf / .glb) 解析为 CPU 侧 MeshData 和材质的贴图路径，
// 供调用方注册纹理后构建 PBRMaterial 使用。
//
// 功能范围（聚焦静态 PBR 模型）：
//   - 顶点数据: POSITION (必有)、NORMAL、TEXCOORD_0
//   - 索引缓冲: 展开为扁平顶点数组和 index list（triangle list），
//     与 OBJ loader 输出格式一致
//   - 自动推导 tangent（从三角形几何 + UV，复用 OBJ loader 的推导逻辑）
//   - PBR 材质贴图路径提取: baseColorTexture、normalTexture、
//     metallicRoughnessTexture（ORM 三通道打包）、occlusionTexture、
//     emissiveTexture
//   - 第一个 mesh 的材质信息（按场景 → mesh → primitive 定位）
//
// 明确不支持（超出本轮范围）：
//   - 骨骼动画: skins / joints / weights（虽然 tinygltf 可读，本 loader 跳过）
//   - 动画: animations 通道
//   - 多 mesh / 多 primitive：只加载第一个 mesh/primitive
//   - 扩展材质: KHR_materials_pbrSpecularGlossiness / KHR_materials_transmission
//   - 顶点颜色: COLOR_0
//   - sparse accessor: tinygltf 内部已展开，本 loader 无需额外处理
//
// 坐标系：
//   glTF 用 Y-up，JPOV 用 Z-up（OBJ loader 同）。本 loader 在顶点阶段
//   将 Y-up → Z-up 变换: 交换 y/z 分量，无缩放。
//
// ORM 解包：
//   glTF 的 metallicRoughnessTexture 是 ORM (Occlusion-Roughness-Metallic)
//   三合一打包（R=Occlusion, G=Roughness, B=Metallic）。本 loader 不在此拆包，
//   只提取原图路径作为 metallic_roughness_tex_。调用方自行决定在 CPU 拆包
//   还是 shader 内按通道采样。
//
// 用法：
//   jpov::MeshData mesh;
//   jpov::GltfMaterialInfo mat_info;
//   if (!jpov::LoadGltf("res/pliers.glb", &mesh, &mat_info)) { ... }
//   mesh.Validate();
//   // 用 mat_info.base_color_tex / mat_info.normal_tex 等注册纹理，
//   // 构建 PBRMaterial，然后 RegisterMesh + DrawObject3D

#ifndef JPOV_SRC_GLTF_LOADER_H_
#define JPOV_SRC_GLTF_LOADER_H_

#include <string>

#include "tools/jpov/interface/mesh.h"

namespace jpov {

// 从 glTF 材质中提取的贴图路径信息。
//
// 所有路径为相对于 glTF 文件所在目录的路径（或空表示无对应贴图）。
// 调用方用 TextureManager::LoadFromFile 注册贴图后用这些路径，
// 然后填入 PBRMaterial 的对应 *_tex 字段。
//
// metallic_roughness_tex: glTF 的 metallicRoughnessTexture（ORM 三合一）。
//   调用方需自行解包 ORM → 3 张独立灰度图（CPU 解包）或
//   在 shader 中按通道采样。
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

}  // namespace jpov

#endif  // JPOV_SRC_GLTF_LOADER_H_
