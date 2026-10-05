// JPOV glTF 2.0 加载器 — glTF/GLB → MeshData + 材质贴图
//
// 把 glTF 2.0 (.gltf / .glb) 解析为 CPU 侧几何和材质的贴图路径，
// 供 Renderer::LoadGltf 注册纹理后构建 PBRMaterial 使用。
// 本 loader 保持纯净：只产出 CPU 侧数据，不接触 GL / 渲染。
//
// 功能范围:
//   - 顶点数据: POSITION / NORMAL / TEXCOORD_0
//   - 骨骼蒙皮输入: JOINTS_0 (VEC4 整数) / WEIGHTS_0 (VEC4 float) → 读进
//     MeshData.joint_indices/joint_weights + 置 kJoints flag（GPU loc3/4 上传见
//     mesh_manager.cc）。仅顶点侧；皮肤(skin 关节树 / inverseBind)读取另行走
//     LoadGltfSkeleton。
//   - 索引缓冲: 展开为扁平顶点数组和 index list（triangle list）
//   - 自动推导 tangent（从三角形几何 + UV，与 OBJ loader 一致）
//   - 多 mesh / 多 primitive：LoadGltfScene 遍历整个场景
//   - PBR 材质贴图路径提取: baseColor / normal / metallicRoughness(ORM) /
//     occlusion / emissive
//   - 贴图来源: 外部文件（image.uri 相对路径）与内嵌 bufferView
//     （GLB 单文件内嵌 PNG/JPEG）均支持。内嵌图**不经临时文件**：以原始编码字节
//     放入 GltfTextureRef.bytes，由下游 TextureManager::FromMemory 直接上 GPU。
//
// 明确不支持（超出本轮范围）：
//   - 动画: animations 通道
//   - 皮肤关节树/逆绑定的【渲染驱动】（本 loader 只把 skins 读成 SkeletonType
//     原始资产，不做坐标变换，见 LoadGltfSkeleton 注释）
//   - 扩展材质: KHR_materials_pbrSpecularGlossiness / KHR_materials_transmission
//   - 顶点颜色: COLOR_0
//   - sparse accessor: tinygltf 内部已展开，本 loader 无需额外处理
//
// 坐标系（铁律，Danis 2026-09-16）：**loader 不改方向**。
//   本 loader 交出的顶点/法线**原样保持资产自身坐标系**（glTF 资产即 Y-up），
//   一律不做任何坐标旋转；loader 甚至不需要知道 "Y-up" 这件事。
//   朝哪放由消费侧决定：DrawObject3D / DrawGltfObject 的 (up, front) 放置参数。
//   资产方向本身不对 → 用 editor 手工改资产，**不要**在加载路径上偷偷转。
//   （历史教训：此处曾做 (x,-z,y) 映射，而骨架侧保持原值 ⇒ 蒙皮顶点与骨架帧
//     不匹配，且 identity 姿态下肤矩阵恒为单位阵 ⇒ 单帧 gold 完全查不出来。）
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
#include "tools/jpov/interface/skeleton_types.h"

namespace jpov {

// 一张 glTF 贴图的**来源**：外部文件，或内嵌（bufferView）字节。
//
// 为什么不一律用文件路径：内嵌图（GLB 单文件）本来就在内存里，旧做法是把它写成一个
//   /tmp 临时 PNG 再让 TextureManager 读回来 —— 把“进程内传数据”降级成“全局可写的磁盘”，
//   既不可靠（同名覆盖）又慢。现改为把**原始编码字节**直接交付上层（内存法）。
//
//   - 外部图（image.uri 非空）：uri = 已并 base_dir 的路径；bytes 空。
//   - 内嵌图（image.bufferView）：bytes = 该 bufferView 的**原始编码字节**（保留原
//     JPEG/PNG 格式，避免重编码膨胀）；uri 空。
//   - key：去重身份（外部 = uri；内嵌 = 内容哈希）。空 key ⇒ 该槽无贴图。
struct GltfTextureRef {
    std::string uri;                   // 外部文件路径（已并 base_dir）；内嵌时为空
    std::vector<unsigned char> bytes;  // 内嵌图原始编码字节（PNG/JPEG…）；外部时为空
    std::string key;                    // 去重身份（外部=uri；内嵌=内容哈希）；空=无贴图

    bool empty() const { return key.empty(); }
    bool is_embedded() const { return !bytes.empty(); }
};

// 从 glTF 材质中提取的贴图**来源** + 常值。
//
// 每张贴图是一个 GltfTextureRef：外部图给路径、内嵌图给字节（.empty() 表示无该贴图）。
// Renderer::LoadGltf 据此:**内嵌→TextureManager::FromMemory（不经文件）**、
//   外部→LoadFromFile；再拆 ORM/occlusion（直接给 FromPixels）。
//
// metallic_roughness_tex: glTF 的 metallicRoughnessTexture（ORM 三合一，
//   R=AO / G=Roughness / B=Metallic）。由 Renderer::LoadGltf 在 CPU 拆包为
//   独立灰度图，分别绑到 PBRMaterial 的 roughness_tex / metallic_tex。
//   occlusion_tex 单独指定时优先用它。（ORM 的 R 不当作 AO。）
struct GltfMaterialInfo {
    GltfTextureRef base_color_tex;         // baseColorTexture（或空）
    GltfTextureRef normal_tex;             // normalTexture（或空）
    GltfTextureRef metallic_roughness_tex; // metallicRoughnessTexture (ORM)（或空）
    GltfTextureRef occlusion_tex;          // occlusionTexture（或空）
    GltfTextureRef emissive_tex;           // emissiveTexture（或空）

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

// ==================== 骨骼（skin / skeleton）读取 ====================

// 读取 glTF 场景中【所有 skin】→ 每个转成一个 SkeletonType（骨骼定义）。
//
// 用途：把 Tripo/Mixamo 导出的带骨骼 GLB（如 male_rig）里的 rig 真读进来，产出
//   一份可直接喂 SkeletonManager 的骨架模板。mesh 顶点侧 JOINTS_0/WEIGHTS_0 由
//   LoadGltf/LoadGltfScene 读进 MeshData（k joint flag），皮肤的关节树/逆绑定由本函数读。
//
// 转换规则（对照业界权威公式 jointMatrix(j)=globalJoint(j)*inverseBind(j)，见
// docs / memory 09-07 bone 调研）：
//   - 每个 skin = 一个 SkeletonType。out.front() 返回第一个（多数单骨架资材只有一个）。
//   - joints 顺序 = skin.joints(node 数组) 顺序；JOINTS_0 以此序号索引。（S0 单骨架场景，
//     out 里至多一个；多 skin 资材少见，仍全部读出由调用方挑。）
//   - 每关节：SkeletonJoint.name = node 名(mixamorig:xxx)。parent = 沿 node 树向上找到的
//     第一个“也在 skin.joints 内有同名 node”的关节（在 skin 里的下标）；找不到 = kSkeletonNoParent。
//   - rest_offset = node.translation（相对父的 rest 平移）。当前 SkeletonJoint 只建模平移；
//     rest 的旋转/朝向在 SkeletonType 不缺它的场景由 inverse_bind 承载（见下）。
//   - inverse_bind = skin.inverseBindMatrices accessor 的 16 float（列主序 MAT4）/关节，
//     与 joints 一一对应，**原样拷自文件，不在此做坐标/自算**。缺省（无 accessor）= 调用方自算，
//     本 loader 不补（返回相应骨架 inverse_bind 为空）。
//
// ⚠️ 坐标系契约：**本 loader 不做任何坐标变换**。mesh 顶点（ParsePrimitive）与骨架
//   （本函数）都原样保持资产自身坐标系直填，两者**天然同帧**（这正是蒙皮能正确工作的前提）。
//   朝哪放由消费侧 up/front 决定；不在加载路径上偷偷转任何东西。
//
// 返回 true 表示文件解析成功（至少 0 个 skin 也算成功，out 可为空）；false 表示无法加载。
bool LoadGltfSkeleton(const std::string& path,
                      std::vector<SkeletonType>* out_skins);

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
