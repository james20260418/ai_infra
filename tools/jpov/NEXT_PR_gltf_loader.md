# glTF 2.0 Loader + Pliers 渲染验证

## 目标
JPOV 新增 glTF 2.0 模型加载能力，用 Poly Haven 的 pliers 模型验证端到端链路的几何 + PBR 材质全通。

## 实现计划

### Step 1: 集成 tinygltf（最小侵入）
- 选用 `tinygltf`（header-only，MIT 许可，纯 C++，无外部依赖）
- 放到 `third_party/tinygltf/`，写 BUILD 文件
- 依赖：已有的 `stb_image`（tinygltf 用它解贴图）

### Step 2: GltfLoader 类
- `tools/jpov/src/gltf_loader.h/cc`
- 接口：`bool LoadGltf(path, MeshData* mesh, GltfMaterial* material)`
- 功能：
  - 解析 `.gltf`/`.glb` → 提取顶点数据（POSITION/NORMAL/TEXCOORD_0）
  - 索引缓冲 → 展开为扁平顶点数组（和 OBJ loader 输出格式一致）
  - 自动推导 tangent（复用 OBJ loader 的推导逻辑）
  - 提取 PBR 材质：baseColorTexture → 图片字节、metallicRoughnessTexture → 解包 ORM 通道、normalTexture → 图片字节
  - 提取 emissiveTexture / occlusionTexture（如果存在）

### Step 3: ORM 通道解包
- glTF 的 metallicRoughnessTexture 是 ORM 打包：
  - R = Occlusion
  - G = Roughness
  - B = Metallic
- 在加载时拆成 3 张独立灰度图（PNG），注册到 TextureManager

### Step 4: GltfMeshData 结构
- 类似现有 MeshData，额外包含：
  - 材质 ID
  - 贴图路径（或内存 buffer）
- 兼容现有 RegisterMesh / DrawObject3D 管线

### Step 5: Pliers Gold Test
- 新增 `jpov_gltf_pliers_gold_test`
- 用 pliers 的 `.gltf` + 贴图渲染
- 灯光：三光源对称（同 cube_normal test）
- 验证：渲染链路通 + 输出非平凡效果图
- glTF 材质中 metallicRoughnessTexture 采样到 ORM 通道 → 拆包后分别绑定到 PBRMaterial 的 metallic_tex / roughness_tex / ao_tex

## 风险点
- tinygltf 依赖 stb_image，注意 BUILD 依赖链
- glTF 坐标系差异（Y-up vs Z-up）— 可能需要在 loader 里翻转
- ORM 拆包的性能开销 — 启动时一次性的，可接受
- 多 mesh 场景 — 第一阶段每个 mesh 独立一个 DrawObject3D 调用

## 预计工作量
- Step 1-2: ~4h（tinygltf 集成 + loader 骨架）
- Step 3-4: ~2h（ORM 拆包 + 数据结构）
- Step 5: ~2h（gold test + 调参）
- 总计：~1 个 PR
