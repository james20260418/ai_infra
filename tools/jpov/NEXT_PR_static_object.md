# JPOV 静态 Object 渲染 — 开发任务说明

> 写给下一个 session。完成当前 PR #36 合入后的 next step。

## 一、背景

JPOV 目前 2D 图元已基本齐备（Polyline2D / Rect2D / Circle2D / Text2D / Strip2D / RoundRect2D / FillRect2D / Arc2D / Image2D），3D 只有基础图元（Triangle3D / Strip3D / Line3D / Text3D）。缺少一个关键能力：**把外部 3D 模型（如 .obj / .gltf）加载为 GPU mesh，每帧用 model transform 画到屏幕上**。

这个 PR 定位为 JPOV 基建层的第二步（第一步是 PR #36 的 TextureManager），目标是建立 **Shader 管理** + **Mesh 管理** + **静态 Object 绘制** 三层地基。这三层是后续骨骼动画、体素方块渲染、材质系统的共同基础。

## 二、核心认知：静态 Object = Mesh + 纹理 + Shader 的组合

一个静态 3D 物体本质上是三个基础能力的组合产物：

```
静态 Object =  Mesh（几何数据）
             × Texture（表面贴图，已有 TextureManager）
             × Shader（着色方式）
             × Transform（model matrix，位置/旋转/缩放）
```

我们的任务不是"加一个 DrawObj 函数"，而是**把这三个基础能力分别建好，然后在 DrawMesh3D 里组合起来**。

## 三、任务拆解

### 3.1 Shader 管理器 (`src/shader_manager.h/cc`)

**现状**：Renderer 里 4 个 shader program 硬编码为 `unsigned int` 成员变量，`CompileShaders()` 直接写死 GLSL 源码。

**目标**：Shader 作为可注册、可查询、可复用的资源。一个 key→program 的缓存层，去掉 Renderer 里 `prog_` / `prog_3d_` / `tex_prog_` / `tex_prog_3d_` / `image_prog_` 这些裸成员变量。

```cpp
struct ShaderSource {
    const char* vertex;    // GLSL 源码
    const char* fragment;
};

class ShaderManager {
public:
    // GetOrCreate: 根据名字和源码获取 shader program。
    // 首次调用时编译+链接+缓存，后续直接返回缓存的 program。
    // 编译/链接失败 → LOG(FATAL) crash。
    unsigned int GetOrCreate(const std::string& name,
                             const ShaderSource& source);
    
    // 根据 program ID 获取 uniform location（首次查询缓存）
    int GetUniform(unsigned int program, const std::string& uniform_name);

private:
    struct ShaderProgram {
        unsigned int program;
        std::unordered_map<std::string, int> uniforms;
    };
    std::unordered_map<std::string, ShaderProgram> programs_;
};
```

**具体要求**：
- `CompileShader()` 和 `LinkProgram()` 函数从 `renderer.cc` 的 anonymous namespace 移到 `shader_manager.cc`
- 所有现有 shader（`kVs/kFs`, `kTexVs/kTexFs`, `kImageFs`, `kVs3d/kFs3d`, `kTexVs3d`）在 `Renderer::Init()` 中通过 `ShaderManager::GetOrCreate()` 注册，不要直接赋给成员变量
- `Renderer` 里不再有 `prog_` / `tex_prog_` / `image_prog_` / `prog_3d_` / `tex_prog_3d_` 成员，全部改为运行时从 `shader_mgr_` 获取
- 各 Draw 函数中的 `glUseProgram(xxx)` 改为先用名字取 program 再 use
- **口子**：新增 shader 时只需 `shader_mgr_.GetOrCreate("my_shader", {vs_src, fs_src})`，不需要改 Renderer 成员变量

### 3.2 Mesh 数据抽象 (`interface/mesh_data.h`)

**现状**：JPOV 没有 mesh 概念。DrawTriangle3D 每次 draw call 临时构造顶点数组。

**目标**：定义 CPU 端 mesh 数据结构和 GPU 端对应资源，支持多顶点布局。

**CPU 数据结构**：

```cpp
// 顶点属性标志（位掩码）
enum class MeshVertexFlags : uint8_t {
    kNone     = 0,
    kPosition = 1 << 0,  // 必有
    kNormal   = 1 << 1,
    kUV       = 1 << 2,
    kJoints   = 1 << 3,   // 骨骼蒙皮（本 PR 不实现，预留）
    kWeights  = 1 << 4,   // 骨骼权重（本 PR 不实现，预留）
};

struct MeshData {
    // 声明包含哪些顶点属性
    MeshVertexFlags flags = MeshVertexFlags::kPosition;

    // 必需：顶点位置
    std::vector<Vec3f> positions;

    // 可选（对应 flags 的位）
    std::vector<Vec3f> normals;
    std::vector<Vec2f> uvs;

    // 索引（可选，为空时用 glDrawArrays）
    std::vector<uint32_t> indices;

    // 骨骼专属（flags 包含 kJoints/kWeights 时才有效，本 PR 不填充）
    // 存储格式：ivec4 展平为 4 个 int32 per vertex
    std::vector<int32_t> joint_indices;  // size = 4 × vertex_count
    std::vector<float>   joint_weights;  // size = 4 × vertex_count

    // 验证内部数据一致性（各数组长度与 positions 对齐）
    void Validate() const;
};
```

**设计要求**：
- `Validate()` 必须做严格检查：positions 非空、各可选数组长度要么 0 要么等于 positions.size()、indices 指向有效范围
- **口子**：`MeshVertexFlags` 和 `joint_indices/joint_weights` 字段为骨骼动画预留。本 PR 只实现 `kPosition | kNormal | kUV`，`k joints/kWeights` 只定义枚举、不做渲染逻辑

### 3.3 GPU Mesh 管理（Renderer 内部）

**目标**：Renderer 内部管理 CPU MeshData → GPU VBO/VAO 的上传和生命周期。

```cpp
// Renderer 内部结构
struct GPUMesh {
    unsigned int vao = 0;
    unsigned int vbo = 0;          // interleaved vertex data (positions + normals + uvs)
    unsigned int ebo = 0;          // element buffer (0 if none)
    unsigned int vbo_joints = 0;   // 骨骼索引 VBO (0 if none, 预留)
    unsigned int vbo_weights = 0;  // 骨骼权重 VBO (0 if none, 预留)
    uint32_t vertex_count = 0;
    uint32_t index_count = 0;
    MeshVertexFlags flags = MeshVertexFlags::kNone;
};
```

**Renderer 新增方法**：

```cpp
// 注册 mesh（上传到 GPU，返回 mesh ID）
uint32_t RegisterMesh(const MeshData& data);

// 更新已有 mesh 的顶点数据（为体素方块动态重建预留的口子）
// Pre-condition: mesh_id 已注册
// Pre-condition: new_data.flags 与注册时一致（VBO 布局不能变）
// 实现策略：先做最简单的 Delete old → Create new。
//           未来可优化为 glBufferSubData（在顶点数不超预分配容量时）
void UpdateMesh(uint32_t mesh_id, const MeshData& new_data);

// 释放 mesh
void ReleaseMesh(uint32_t mesh_id);
```

**具体要求**：
- `RegisterMesh` 根据 `flags` 自动决定 VBO 布局（哪些 attribute 存在、stride 多少）
- 顶点数据使用 interleaved 格式（所有属性交错存储在一个 VBO 中），用 `glVertexAttribPointer` 的 stride/offset 区分
- 析构时释放所有 GPUMesh 的 GL 资源
- MinGW 交叉编译必须通过（参考 PR #36 的 GL 头文件 include 顺序：先 `GL/gl.h` 拿常量，再 `#define` 映射函数名）

### 3.4 渲染指令 (`interface/render_command.h/cc`)

**新增**：

```cpp
// 枚举值
kMesh3D,   // 3D 静态 mesh

// 渲染指令结构体
struct Mesh3DCommand {
    uint32_t mesh_id;
    float    transform[16];  // 4×4 model matrix，列主序
    Color    tint;           // 颜色乘数（默认白色=不染色）
    // 可选纹理（0 表示纯色）
    uint32_t texture_id = 0;
};

// RenderCommandList 新增方法
void DrawMesh3D(uint32_t mesh_id, const float transform[16],
                const Color& tint = kColorWhite,
                uint32_t texture_id = 0);
```

**关于 `transform[16]`**：当前 JPOV 没有矩阵类型（现有 `mvp_[16]` 是裸 float 数组）。用 `float[16]` 对齐现有风格，列主序（OpenGL 惯例）。未来统一迁移到 glm 时再改。

**关于 `texture_id`**：0 表示纯色渲染（用 solid color shader），非 0 表示取 TextureManager 中已注册的纹理。这是"静态 Object = Mesh + 纹理 + Shader"组合的体现。

### 3.5 DrawMesh3D 渲染实现

在 `renderer.cc` 中新增 `DrawMesh3D()` 方法：

```
流程：
  1. 根据 mesh_id 查找 GPUMesh
  2. 若 texture_id != 0，查找 GL 纹理对象
  3. 选择 shader：
     - 有纹理 → shader_mgr_.Get("mesh3d_textured")
     - 无纹理 → shader_mgr_.Get("mesh3d_solid")
  4. 绑定 VAO + EBO
  5. 设 MVP = Projection × View × model_transform
  6. 设颜色 uniform / 纹理 uniform
  7. glDrawElements (有索引) 或 glDrawArrays (无索引)
```

**Shader 源码**：
- `mesh3d_solid`：vertex shader 接受 pos+normal+uv（即使不用 uv，保持 layout 兼容），fragment shader 输出 `uColor`
- `mesh3d_textured`：同上但 fragment shader 加 `tex_color * uTint`

**注意**：Mesh3D 的渲染要插入现有的 3D 指令流程。目前 3D 指令在 `Draw3DCommands()` 里处理，使用 MSAA FBO。`kMesh3D` 应该在 `Draw3DCommands` 的分发 switch 中处理，和 `kTriangle3D` / `kStrip3D` 同级。

### 3.6 JPOV 对外 API (`include/jpov/jpov.h/cc`)

```cpp
// 注册 mesh
uint32_t RegisterMesh(const MeshData& data);

// 更新已有 mesh 的顶点数据（体素方块动态重建的口子）
void UpdateMesh(uint32_t mesh_id, const MeshData& new_data);

// 释放 mesh
void ReleaseMesh(uint32_t mesh_id);
```

这些方法直接代理到 `renderer_->RegisterMesh()` / `renderer_->UpdateMesh()` / `renderer_->ReleaseMesh()`。Pre-condition：`Init()` 已调用。

### 3.7 BUILD 文件

- `src/BUILD`：新增 `shader_manager` cc_library，renderer 增加依赖
- `src/BUILD`：renderer 的 deps 中已包含 `:texture_manager`，ShaderManager 不需要额外 GL 依赖（通过 renderer 间接使用 GL context）
- 注意 MinGW 交叉编译：`shader_manager` 在 Windows 下需要 gl_loader 的函数指针 typedef，参考 `texture_manager` 的 BUILD 写法

## 四、Gold Test

### 测试目标

用 Mesh API 画一个彩色立方体（6 面不同颜色），和现有的 `jpov_cube3d_gold_test` gold image 完全一致。这验证了：
1. Mesh 数据构造 → RegisterMesh → DrawMesh3D 整条链路
2. Transform matrix 正确（立方体可以旋转放置）
3. Camera 透视投影正常工作
4. 新 API 产生和旧手写 Triangle3D 相同的像素结果

### 测试设计

构造一个立方体的 `MeshData`（36 个顶点，6 面 × 2 三角形 × 3 顶点，每面不同颜色）：
- 不需要 OBJ 文件——在代码里手写顶点数据，和现有 `jpov_cube3d_gold_test.cc` 一样
- 使用 `flags = kPosition`（不需要法线和 UV，纯色渲染）
- `RegisterMesh` 后调用 `DrawMesh3D` 绘制
- 复用现有 gold image：`cube3d_6faces_1280x720.png`

### 文件

- `test/jpov_mesh3d_gold_test.cc` — 测试代码
- `test/BUILD` — 新增 `jpov_mesh3d_gold_test`（data 复用 `cube3d_6faces_1280x720.png`）

不需要 gold generator（复用现有 gold image）。

## 五、不做的

- **OBJ 解析器**：不属于渲染引擎。用户自己解析 OBJ → 构造 MeshData → RegisterMesh
- **法线 / 光照计算**：本 PR 只做纯色 + 纹理采样。Lighting 是独立的 shader 层面的事
- **PBR 材质**：tint 颜色 + 单纹理足够 MVP
- **关节动画 / 骨骼蒙皮**：只预留 vertex flags + 数据结构字段，不做渲染逻辑
- **Instancing**：每个 DrawMesh3D 一次 draw call，不做 batch
- **glBufferSubData 优化**：UpdateMesh 先做 Delete+Create，不做容量预分配

## 六、口子清单（给下一个 session 的 checklist）

以下口子本 PR 必须留好，供后续功能使用：

- [ ] `MeshVertexFlags::kJoints` 和 `kWeights` 枚举值已定义（即使不实现渲染）
- [ ] `MeshData::joint_indices` 和 `joint_weights` 字段存在、`Validate()` 检查其长度
- [ ] `GPUMesh::vbo_joints` 和 `vbo_weights` 字段存在（初始化为 0）
- [ ] `UpdateMesh()` 方法已实现（Delete+Create 策略）
- [ ] `ShaderManager::GetOrCreate()` 接口干净，后续加骨骼 shader 只需一行注册
- [ ] `Mesh3DCommand::texture_id` 字段存在（默认为 0=纯色）
- [ ] MinGW 交叉编译通过：`bazel build //demo/... //tools/jpov:jpov_demo.exe --config=windows`

## 七、现有文件修改清单

| 文件 | 操作 | 说明 |
|------|------|------|
| `src/shader_manager.h` | **新增** | Shader 管理器 |
| `src/shader_manager.cc` | **新增** | 实现 |
| `interface/mesh_data.h` | **新增** | MeshData + MeshVertexFlags |
| `interface/render_command.h` | 修改 | 新增 `kMesh3D`、`Mesh3DCommand`、`DrawMesh3D()` |
| `interface/render_command.cc` | 修改 | `DrawMesh3D()` 实现 + Clear 中清 mesh3d |
| `src/renderer.h` | 修改 | 新增 GPUMesh/ShaderManager/mesh 管理；移除裸 shader 成员 |
| `src/renderer.cc` | 修改 | DrawMesh3D 实现；CompileShaders 改为用 ShaderManager；所有 draw 函数改用 shader_mgr_ |
| `include/jpov/jpov.h` | 修改 | RegisterMesh/UpdateMesh/ReleaseMesh |
| `include/jpov/jpov.cc` | 修改 | 代理到 renderer |
| `src/BUILD` | 修改 | 新增 shader_manager 库；renderer 加依赖 |
| `test/BUILD` | 修改 | 新增 jpov_mesh3d_gold_test |
| `test/jpov_mesh3d_gold_test.cc` | **新增** | Gold test |

## 八、当前代码参考

- Renderer 头文件：`src/renderer.h`（成员变量列表、所有 Draw 方法）
- Renderer 实现：`src/renderer.cc`（CompileShaders、DrawTriangle3D、Draw3DCommands 分发逻辑）
- 渲染指令接口：`interface/render_command.h/cc`（枚举、结构体、Clear、draw 辅助方法）
- JPOV 头文件：`include/jpov/jpov.h`（RegisterTexture 参考模式）
- TextureManager：`src/texture_manager.h/cc`（资源管理参考模式）
- BUILD 结构：`src/BUILD`（cc_library 写法、select() 跨平台分派）
- 现有 gold test：`test/jpov_cube3d_gold_test.cc`（参考写法 + 复用其 gold image）
- `bazel build //demo/... //tools/jpov:jpov_demo.exe --config=windows` 是 MinGW 交叉编译验证命令
- `bazel test //tools/jpov/test:all --test_output=errors` 是全部 gold test 验证命令
