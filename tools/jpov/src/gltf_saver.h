// JPOV glTF 2.0 保存器 — CPU 资产 → 自洽 .glb
//
// 与 gltf_loader 对称（同路径、同 GL-free 约束）：把「已 apply 好放置变换的 CPU 资产」
// （几何 + 材质贴图 + 可选骨架）写成一个**单文件自洽**的 .glb，可被
// Renderer::LoadGltf / viewer 直接打开。
//
// 用途：模型编辑器「保存按钮」的落盘层。见 docs/jpov_model_editor_save_plan.md。
//
// 产出内容：
//   - JSON chunk：asset / scene / node / mesh / primitive / material / skin /
//     accessor / bufferView / buffer / image / sampler / texture
//   - BIN chunk：所有 accessor 的紧密排布数据
//
// 顶点属性写入约定（与 loader 读回对称）：
//   - POSITION / NORMAL / TEXCOORD_0 → FLOAT（VEC3/VEC3/VEC2）
//   - JOINTS_0 → UNSIGNED_SHORT VEC4（glTF 规范 JOINTS_0 允许 ushort；loader 读 int 侧无碍）
//   - WEIGHTS_0 → FLOAT VEC4
//   - indices → UNSIGNED_INT SCALAR（triangle list）
//   - inverseBindMatrices → FLOAT MAT4（列主序，与 ComputeInverseBind 一致）
//
// 贴图：**一律内嵌**（读图片文件字节 → bufferView + image + mimeType），产出单文件
//   glb —— 用户体验最好（不会"拷了 glb 丢贴图"）。这与 loader 把内嵌图导出成临时
//   文件是反向操作。
//
// 已知边界（记录，不在本 PR 修）：
//   - ORM 重打包是**近似**：loader 会把 metallicRoughnessTexture 的 G/B 拆成两张灰度
//     图、AO 可能来自独立 occlusionTexture 并按 strength 烘焙过；保存时重新打包回一张
//     ORM 只能做到"数值等价但通道来源被合并"（原图通道语义已丢失，无法逐像素还原）。
//     因此保存**优先写独立贴图槽**（baseColor/normal/metallicRoughness/occlusion/emissive），
//     当仅有一张 ORM 且无独立 AO 时按 ORM 写。
//   - node 变换：本保存器写的 node **不带 TRS**（恒等），所有变换已烘进顶点。这与
//     loader 的行为（丢弃旋转/缩放）一致，保证"存→读"几何不变。
//
// Pre-condition（WriteGlb）：GL context 无需激活（纯 CPU 文件 IO）。

#ifndef JPOV_SRC_GLTF_SAVER_H_
#define JPOV_SRC_GLTF_SAVER_H_

#include <optional>
#include <string>
#include <vector>

#include "tools/jpov/interface/mesh.h"
#include "tools/jpov/interface/skeleton_types.h"
#include "tools/jpov/src/gltf_loader.h"   // 复用 GltfMaterialInfo（贴图路径 + 常值）

namespace jpov {

// 一个待保存的 primitive：一份 CPU 几何 + 一份材质信息。
//
// material 直接复用 loader 的 GltfMaterialInfo（贴图**路径** + 常值），使
// "加载 → 编辑 → 保存"用的是同一套材质描述，不引入第二套类型。
struct GltfSaveMesh {
    MeshData mesh;
    GltfMaterialInfo material;
};

// 一个待保存的完整资产。
//
// skin：带骨模型才有。一个 skin 管所有 primitive（与 glTF 的 skin 语义一致）；
//       调用方应保证所有 primitive 的 joint_indices 都以该骨架的关节序索引。
//       写入时 inverseBindMatrices 由 skin->ComputeInverseBind() 现算（骨架是唯一
//       真相，不存第二份 IBM）。
struct GltfSaveAsset {
    std::vector<GltfSaveMesh> meshes;
    std::optional<SkeletonType> skin;

    // 模型名（写入 node/mesh 名；空则用 "model"）。
    std::string name;
};

// 把资产写成一个自洽的 .glb 文件。
//
// 成功返回 true；失败返回 false 并 LOG(ERROR)（不抛异常、不半写：先组完整字节流
// 再一次性落盘）。
//
// Pre-condition: asset.meshes 非空；每个 mesh.mesh.Validate() 通过；path 非空且所在
//   目录可写。
bool WriteGlb(const GltfSaveAsset& asset, const std::string& path);

}  // namespace jpov

#endif  // JPOV_SRC_GLTF_SAVER_H_
