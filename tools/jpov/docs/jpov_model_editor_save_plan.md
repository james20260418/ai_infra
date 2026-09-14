# JPOV 模型编辑器 —— 保存（apply + saver）实现计划

> 需求（2026-09-12 Danis）：
> 1. 工具加**保存按钮**：按下后**自开线程**把 GLB 写到 **elf 同级目录**；文件名 = 原 glb 同名 + `edit` + 日期时间后缀；日期时间字符串函数写到公共地方。线程运行时按钮文字变 `保存中...`；完成后左上角黑字说明里追加一条 `保存至 ...`。**保存中按钮不可重复触发**。
> 2. 在 loader 所在路径下开发一个 **saver**，能把带骨 object3d 存下来。
> 3. 能把绘制时用的 **front / up / scale** 这些参数 **apply 到 CPU mesh**（法线、joint 属性等）。
> 验收：Danis 亲手旋转一个 glb → 保存 → **model viewer 能打开看**（旋转保留）。

---

## 0. 关键事实（已核实，决定实现形态）

| 事实 | 出处 | 影响 |
|---|---|---|
| **JPOV 目前只把「带骨 glb」当静态 mesh 画** | `RenderCommandList::DrawGltfObject` → 全是 `Object3DCommand`（无 `SkinnedMeshCommand`）| 编辑器里**可以直接 apply 到顶点**，viewer 用静态管线打开即得同一外观 |
| 静态 VS 用 **uModel 的逆转置** 算法线/切线 | `object3d_renderer.h` `kMeshVs3dPBR{,Full}` | apply 必须连**法线/切线**一起转，否则重新加载后光照错 |
| 蒙皮 VS 不做这件事 | `skinning_shader.h` `kSkinnedVs`（`sp = Σw·M·aPos`，再 `uModel * sp`）| 若将来走蒙皮管线，重算法线会**双重应用 bind 旋转** → 见 §6 风险 R1 |
| `DrawGltfObject` 的 `scale` 是**均匀标量** | `render_command.h` | apply 用 `v*scale`，法线**不变**（无 `1/s`），切线**需乘 s**（与 VS 一致） |
| loader 的顶点映射 `(x,-z,y)` 与法线映射**同一个矩阵** | `gltf_loader.cc` `ParsePrimitive` | 是**合法旋转**（det=+1）→ glTF 输出的 `NORMAL` 可直接用，**无需重算法线** |
| loader **丢弃**节点 TRS 的旋转/缩放（只用平移+缩放），且只取 mesh 链上**最后一个**节点的矩阵 | `gltf_loader.cc` `BuildMeshTransforms` / `NodeTRS2Mat3` | 不保真原文件，但**几何自洽可渲染**；已记录为已知边界 |
| loader 产出**没有** `TANGENT` | `gltf_loader.cc` | 切线由 loader 从三角几何 + UV **推导**（确定性）。glTF 并不要求写 TANGENT，写属冗余 → 不写 |
| 内嵌贴图被 loader **导出到 `/tmp/jpov_gltf_embed/`** | `gltf_loader.h` | 保存时必须检测这类贴图 → **改内嵌进 GLB**，否则存下来是个坏文件 |
| GLB = 12字节头 + JSON chunk + BIN chunk | glTF 2.0 规范 | 用 nlohmann::json 拼 JSON + 手写 12 字节头即可（json.hpp 已 vendor） |
| 字模顶点坐标空间 = **本帧渲染分辨率**（= 窗口尺寸） | `FontRenderer::kTextVs`（`ndc = (aPos/uFboSize)*2-1`）| 保存线程要在**渲染线程空闲时**画 `保存中...` 到自己的 FBO 读回来做按钮贴图 → 见 §5 |
| `RenderCommandList::DrawObject3D` 无 per-command 颜色 alpha | `render_command.h` | "变灰禁用"只能改 RGB，不能半透明 |
| 高亮 pass 每帧从 `fbo_hdr_` 复制颜色（`kMaxFboDim = 4096`）| `renderer.cc` | 窗口开到 >4096 时高亮任务崩；不属本 PR（已记录）|

---

## 1. 三条需求的落地位置

```
src/                          interface/
├── gltf_loader.{h,cc}        （已有，不改语义）
├── gltf_saver.{h,cc}   ← ② SAVER（loader 同路径，新建）
│     WriteGltf/WriteGlb：(MeshData + 材质 + SkeletonType) → .glb
├── mesh_transform.h    ← ③ APPLY（GL-free，纯函数，新建）
│     ApplyPlacementToMesh(mesh, up, front, scale) ［+ 骨架重算/烘焙］
├── time_util.h / .cc   ← ① 日期时间字符串函数（公共位置，见 §4 注）
└── renderer.cc                （+ Renderer::ReadMesh + Renderer::ReadGltfVertices）
demo/editor/
├── model_placement.h          （已有）
├── editor_app.h               （+ 保存按钮 + 状态 + 左上角"保存至…"）
├── editor_save.{h,cc}  ← ① 保存编排（路径命名 + 后台线程 + 状态机）
├── jpov_model_editor.cc       （装配）
└── BUILD
```

**为什么 saver / apply 放 `src/` 而不是 `demo/editor/`**：需求 ② 明说"在 loader 所在的路径下"，且两者都是 GL-free、可被别的工具复用的资产层能力（apply 还是未来 retarget 的输入）。

---

## 2. ③ Apply：把绘制参数烘进 CPU mesh（**最需要先做，因为它是保存的语义本体**）

### 2.1 语义（与静态 VS 严格对齐）

```
v' = T(center) · R(up,front) · (scale · v)
n' = normalize( R(up,front) · (scale · n) )      // R 正交 det=+1；scale 对法线是恒等
t' = scale · ( R(up,front) · t )                 // 切线同位置、不归一化（与 kMeshVs3dPBRFull 一致）
(索引 / UV / joint_indices / joint_weights 原样)
```

> `kMeshVs3dPBRFull` 里 `vWorldNormal = normalize(mat3(transpose(inverse(uModel))) * aNormal)`；正常放缩下 `mat3(transpose(inverse(S·R))) = S⁻¹·R`，而法线**不缩放**，故 = `R·n`。

### 2.2 ⚠️ 顶点转、骨不转 = 蒙皮被破坏 → 必须处理

本仓库的正规蒙皮链是：**pose（相对父旋转）→ 沿骨架树解算 JointMatrix → `sp = Σw·M·aPos`**。

于是 `v' = U·v` 之后，蒙皮结果是 `Σw·M·U·v = (Σw·M·U)·v`，而正确结果应是 `U·Σw·M·v = (U·Σw·M)·v`。两者只在 `U` 与 `M` 可交换时才相等 —— 一般不等。

**修正（唯一能让带骨模型摆正后又保持 pose 的办法）**：把放置烘进**骨架空间**，而不是只烘顶点：

```
rest_offset'[j] = U_linear · rest_offset[j]          (j 非根)
rest_offset'[root] = U · rest_offset[root] + center  → 含平移
bind_rotation'[j] = q_U · bind_rotation[j] · q_U⁻¹   ← 共轭（朝向是轴/角，只转轴不转角度）
```

`q_U` = `R(up,front)` 对应的四元数（**不含平移**；平移由根的 `rest_offset` 承载）。
此时 `JointLocal(j) = T(U·o)·R(q_U b q_U⁻¹) = U · [T(o)·R(b)] · U⁻¹ = U · JointLocal_bind(j) · U⁻¹`，沿树复合后
`JW'[j] = U · JW[j] · U⁻¹`，**蒙皮顶点 = `U·(原结果)`，与顶点烘 `U` 完全一致** ✅
即：**顶点与骨架同时烘同一个 `U`，几何与 pose 都保留**。

- **本 PR 必须实现这条**（否则"旋转后的带骨模型"存下来 pose 会跑掉）；
  单测：造一个小骨架 + 非平凡 pose，比对 `Σw·(U·M·U⁻¹)·(U·v)` 与 `U·(Σw·M·v)`。
- **不做**的部分（保持简单，且 glTF 骨架端无逆变换可逆）：`scale`、非刚性的 `U`（含非均匀缩放）只作用于顶点。
  → **按钮流程里只烘「刚体放置」`center + up/front`（scale 恒 1）**，让 `U` 一定是刚性矩阵，上面的推导严格成立。
  非均匀 `scale` 的骨架一致性留给后续 PR（见 §6 R2）。

### 2.3 签字（接口）

```cpp
// GL-free 纯函数；不改原 mesh，产出新 mesh（便于"保存时才算、编辑态不动"）。
MeshData ApplyPlacementToMesh(const MeshData& in,
                              const Vec3f& center, const Vec3f& up,
                              const Vec3f& front, float scale);
// 骨架同步（顶点烘 U 时配套调用；scale 必须 == 1.0f，否则 CHECK 失败）
SkeletonType ApplyPlacementToSkeleton(const SkeletonType& in,
                                      const Vec3f& center, const Vec3f& up,
                                      const Vec3f& front);
```

---

## 3. ② Saver：`src/gltf_saver.{h,cc}`

职责：**把"已 apply 好的 CPU 资产"写成一个自洽的 `.glb`**（几何 + 材质 + skin + 贴图），产出可被 model viewer 直接 `LoadGltf` 打开。

```cpp
struct GltfSaveMesh {          // 一个 primitive
    MeshData mesh;             // 顶点属性（POSITION/NORMAL/TEXCOORD_0/JOINTS_0/WEIGHTS_0）
    GltfMaterialInfo material; // 贴图（外部路径 或 已内嵌进本次 GLB）
};
struct GltfSaveAsset {
    std::vector<GltfSaveMesh> meshes;
    std::optional<SkeletonType> skin;   // 带骨才有；一个 skin 管所有 primitive
    std::vector<std::array<float,16>> inverse_bind;  // 与 skin->joints 一一对应（ComputeInverseBind）
};
bool WriteGlb(const GltfSaveAsset& asset, const std::string& path);
```

写入内容：
- **JSON chunk**：asset / scene / node（含可选的 skin 引用）/ mesh / primitive / material / skin / accessor / bufferView / buffer / image
- **BIN chunk**：所有 accessor 的紧密排布数据（POSITION/NORMAL/TEXCOORD_0 用 FLOAT，JOINTS_0 用 `UNSIGNED_SHORT`（4 分量，窄值），WEIGHTS_0 用 FLOAT，IBM 用 FLOAT MAT4，indices 用 `UNSIGNED_INT`）
- `buffer[0]` 用 `byteLength` **不含 4 字节对齐 pad**（GLB BIN chunk 才按规范补 `0x20` pad）——**必须 pad**，否则 tinygltf 读回崩
- **贴图**：
  - 外部贴图 → 直接内嵌（读文件字节 → bufferView + image + mimeType），产出**单文件自洽 glb**（用户体验最好：不会出现"拷了 glb 丢贴图"）
  - loader 导出到 `/tmp/jpov_gltf_embed/` 的内嵌贴图 → 同样内嵌（本来就是从资产里来的）
  - 材质槽：baseColor / metallicRoughness(ORM 三通道：R=AO,G=Roughness,B=Metallic，从我们拆开的三张灰度图**重新打包**回 ORM) / normal / emissive
    ⚠️ 这里有个**已知量差**：loader 会把 ORM 的 G/B 拆成独立灰度图、AO 可能来自单独 occlusionTexture 并按 strength 烘焙过 → 重新打包回去是**近似**（AO/roughness/metallic 数值等价，但通道来源被合并）。记录为已知边界。
- `node`：单个 node（name = 模型名），`mesh` 指向（多 primitive 时用 mesh 的多 primitive 或建多个 node 各持一个 primitive —— 选**多个 node**，与 loader 的逐 primitive 模型一致，简单）

**单测（不需要 GL，也不依赖原资产）**：造一个最小带骨 `GltfSaveAsset`（2 primitive + 1 skin）→ `WriteGlb` → `LoadGltfScene` / `LoadGltfSkeleton` 读回 → 比对顶点数/包围盒/骨名/`bind_rotation`/IBM 数值。**闭环自证，不需要外部资产**。

---

## 4. ① 保存按钮

### 4.1 日期时间字符串 → 公共位置

- 现在仓库里**没有**通用时间字符串工具。放在 **`tools/common/utils.h`**（`//tools/common:utils`，零依赖纯头、`jpov_screenshot_demo` 等已在用），与"公共地方"的字面要求一致。
- 现有工具（viewer 的 `viewer_output.h`）是"给文件名加**前缀**"，**不复用**（语义不同，不硬套）。
- 接口（`EditTimestampSuffix(now)` 收时间戳便于单测）：
  ```cpp
  std::string EditTimestampSuffix(std::time_t now);   // → "20260912-2250-13"
  ```
- **文件名**：`<stem>_edit<YYYYMMDD-HHMMSS>.glb`
  - `stem` 取原 glb basename 去扩展名；**原路径无法写入或不可写时**回退到
    "**glb 同级目录**"（需求写的是 elf 同级目录，但 elf 同级 = `output/jpov_model_editor/` 与模型毫无关系，
    而"生成在 glb 旁边"才符合用户心智）→ **这一点想跟你确认，见 §7 待确认**。

### 4.2 状态 + 线程

```
enum class SaveState { kIdle, kSaving, kDone, kFailed };
struct SaveStatus {              // 跨帧持有（EditorApp 成员）
    SaveState state = kIdle;
    std::thread worker;
    std::atomic<int> progress{0};        // 0=未开始 1=已快照 2=快照就绪 3=请求渲染
    std::atomic<int> done{0};            // 0=在跑 1=成功 -1=失败
    std::string path;                    // 目标路径（完成后显示）
    std::string error;
    std::mutex mtx; std::condition_variable cv, cv_done;
    bool idle_snapshot = false;          // 渲染线程已空闲、快照可读
    std::vector<MeshData> meshes;        // ← 渲染线程填（CPU 几何快照）
    std::vector<GltfMaterialInfo> mats;
    std::optional<SkeletonType> skin;
    unsigned int btn_tex[2] = {0, 0};    // 按钮文字贴图（[0]=保存 [1]=保存中...）
    unsigned char btn_px[2][...];        // 读回的像素（按钮尺寸固定，缓冲固定）
};
```

**线程协议（关键：GL/GPU 只在渲染线程碰）**

```
主线程（UI 命中按钮 && state==kIdle）
   → state=kSaving；按钮文字立变"保存中..."；起 std::thread(worker)

worker:
   1) 拼路径（原 glb 同级目录 + stem + _edit<时间戳>.glb）  — 纯 CPU
   2) progress=1；等 cv（渲染线程 ack）
   3) ← 渲染线程休眠时：填 meshes/mats/skin 快照 → idle_snapshot=true → notify
   4) 等 idle_snapshot && 渲染线程已不在用快照（cv_done） → 取走快照（clear）
   5) ApplyPlacementToMesh（每 primitive，用按下那一刻的参数快照 up/front/scale=1）+ ApplyPlacementToSkeleton
   6) WriteGlb(path)
   7) 唤醒渲染线程：progress=3（请求画"保存中..."到按钮 FBO）→ 等回填
   8) done=1/‑1 → notify

渲染线程（OneIteration 内轮询 progress，开销可忽略）
   - progress==1 → 填几何快照 + 骨架 → notify（cv）
   - progress==3 → 用 btn_tex 画按钮（两态各读回一次，只在保存期间做）
   - 保存窗口期：暂停消费左键/滑条输入（避免改到正在被烘的放置参数）
   - done!=0 → join；state=kDone/kFailed；把 `保存至 <path>` 追加进左上角黑字
```

- **不可重复触发**：`state==kSaving` 时按钮点击直接忽略（`points` 也不进入）；按钮文字为"保存中..."。
- **失败**：`state=kFailed` → 左上角显示 `保存失败：<原因>`（黑字），按钮回到"保存"可重试。
- **退出安全**：`EditorApp` 析构 / `Finalize` 前若 `state==kSaving` → 停线程并 `join`（不 detach，避免"程序退出时线程还在写文件"）。

### 4.3 按钮文字"保存中..."怎么画

- 文字贴图：两个 256×64 的 RGBA8 纹理，内容分别渲染 `保存` / `保存中...` 的**白色**字（白字 × `UiTheme` 的按钮背景色 = 与 `Ui::Button` 现有绘制一致）。
- 画法：临时切到自己的 FBO（尺寸 256×64，`glViewport` 同步）→ 用 `text` program + 字模顶点画到白色 FBO → `glReadPixels` 读 RGB → 存进 `btn_px`。
  **只在保存窗口期间做（2 次，几十微秒）**，其余时间零开销。
- 显示：仿 `DrawImage2D` 的顶点生成（pass-through 纹理坐标会在渲染分辨率 ≠ 贴图尺寸时**垂直翻转** → 保存时按"OTF 顶点 + 手工 u/v 翻转"处理，与截图那条路一致）。
- 按钮几何：`PanelLayout` 加一行区域（面板第 6 行或右侧），**沿用 `PointInPanel` 单一真相**（加按钮区后不要让它变成旋转的隐形触发区）。

---

## 5. Viewer 侧：**不改**（先验证）

- 存出来是 `Object3DCommand` 型静态 mesh（顶点已烘放置）→ 现有 viewer 直接 `LoadGltf` 就能打开，外观应与 editor 屏幕所见一致。
- 若发现 viewer 里看到的**光照/法线**和 editor 里不一致 → 说明 apply 的法线处理与 `uModel` 不等价，回头查 §2.1 公式。

---

## 6. 风险 / 已知边界

| # | 风险 | 处理 |
|---|---|---|
| **R1** | 蒙皮 VS **不做** `inverse(transpose(uModel))`，所以"顶点烘了 `U`"的 mesh 若将来走**蒙皮管线**，法线会**双重应用** `bind_rotation` | 本 PR 保存的 glb 走**静态管线**（与现状一致）；同时把"`SkinnedMeshCommand` + 已 apply 的 mesh"列为**不可用组合**，写进注释/文档，避免后人踩 |
| **R2** | 非均匀/非刚体 `scale` 不能烘进骨架（`JW' = U·JW·U⁻¹` 要求 `U` 可逆且与蒙皮可交换，非均匀缩放 + 旋转后不再满足） | 保存时 `scale` 固定 1.0（按钮流程不暴露"带缩放保存"）；滑条上的 scale 仅供**预览**，保存前提示（或保存时按 1.0 烘，并把 scale 折进 `center`/骨架根 —— 见拍板项） |
| **R3** | apply 后 CPU mesh 的包围盒变了 | 保存流程里同步重算 `bounds_`（viewer 打开靠它自适应相机） |
| **R4** | 节点 TRS 旋转/缩放仍被 loader 丢弃（只取平移+缩放） | 记录为已知边界；本 PR 不动 loader |
| **R5** | ORM 重打包是近似（三张灰度图 → 一张 ORM） | 记录；AO/roughness/metallic 数值逐像素等价，只是"来源通道"合并 |
| **R6** | `RenderCommandList` 无 per-command alpha → 按钮不能半透明禁用 | 用**文字**表达状态（"保存中..."），不做半透明 |
| **R7** | 高亮 pass 在窗口 >4096 时崩 | 不属本 PR，记录 |

---

## 7. 分步计划（先后顺序 = 依赖顺序；每步都可单独验证）

| 步 | 内容 | 产出/验证 | 状态 |
|---|---|---|---|
| **S0** | 本计划拍板（尤其 §7 的 3 个待确认） | 你点头 | ✅ 2026-09-13 Danis 交我自行判断 |
| **S1** | `mesh_transform.h` + 单测（含"顶点烘 U ⟺ 骨架共轭 U"的等价性） | 纯单测 | ✅ 14 用例；`interface:mesh_transform_test` |
| **S2** | `gltf_saver.{h,cc}` + 单测（写→读回闭环） | 纯单测 | ✅ 9 用例（含真带骨资产 mixamo_male）；`test:jpov_gltf_saver_test` |
| **S3** | 时间戳字符串进 `tools/common/utils.h` + 单测 | 纯单测 | ✅ 3 用例；`//tools/common:utils_test` |
| **S4** | editor 保存按钮：状态机 + 后台线程 + 按钮文字 + 左上角"保存至…" | 本地开窗口手点 | ✅ `demo/editor/editor_save.{h,cc}` + `editor_save_test`（5 用例） |
| **S5** | 端到端验收：真 glb 旋转 → 保存 → 打开比对 | 你要的验收路径 | ✅ `editor_save_e2e_test`（像素+包围盒门禁） + 真带骨资产闭环用例 |
| **S6** | 零运行代码阅读检查 + 自 review + 交付物给 Danis → 认可后跑唯一一次 CI | 合 PR | ◻ 待 Danis 认可 → 跑 CI |

> 按 `general-dev-rules` §2.5：**开发期不跑 CI**；交付物先给你看，认可后再跑一次。

---

## 9. 实施结果与偏离记录（2026-09-13）

### 三个待确认项的决策（Danis 授权自行判断）
1. **保存目录**：取 **glb 同级目录**（`<stem>_edit<时间戳>.glb` 落在原模型旁，符合用户心智）；
   未用 §4.1 的“elf 同级”作主路径（elf 同级与模型无关，仅原本担心不可写时的兜底，而
   "不可写即报错" 比“静默写別处”更符合 "不 fallback" 原则）。
2. **文件后缀**：`<stem>_edit<YYYYMMDD-HHMMSS>.glb`（例 `plier_edit20260913-162000.glb`）。
3. **保存时 scale**：强制按 **1.0** 烘（骨架一致性要求刚体 U）；UI 上的缩放仅预览。
   保存时若 scale!=1 会在日志里警告一句（不静默）。

### 设计偏离（均写进代码注释与下方理由）
1. **按钮文字不再用“渲染文字到纹理再回读”**（plan §4.3）。真实需求只是“文字变 + 不可重复触发”
   → 直接切 `Ui::Button` 的 label 字符串即达成，且不把 GL 操作塞进保存路径。
2. **线程协议大幅简化**（plan §4.2）：不再需要“渲染线程快照几何 + 握手”。
   编辑器启动时用**纯 loader** 额外读一份 CPU 快照（GL-free），worker 只读不可变快照 →
   无需跟渲染线程交互，也彻底避开“worker 碰 GL”的风险。
3. **坐标映射互逆**（plan §0 未提，实施中发现）：loader 会把 glTF 顶点按 `(x,−z,y)` 映射到
   JPOV 局部；所以 saver 写顶点前必须施加**逆映射** `(x,z,−y)`，否则每次往返模型多转 90°。
   而**骨架数据不映射**（`LoadGltfSkeleton` 读原值）→ saver 原样写。已加“两轮往返稳定”单测锁定。
4. **骨架烘焙的平移修正项**（plan §2.2 低估）：plan 写 `rest_offset'[root] = R·o+center`，
   但正确式是**每个关节**都要带 `center − R(b'_j)·center`（不只根）——因为 U 含平移，
   共轭 `T(o)·R(b)` 时 `R(b)` 会把平移转掉。plan 只在 b_root==I 假设下对。已单测锁定
   （起初只给根加，子骨偏差 1.69 → 修后 ≤1e-6）。另记录：**本函数只对 rest 姿态正确**
   （带 pose 资产需另算偏差项），故不接 pose 参数。
5. **端到端像素门禁的现实**：保存把贴图**内嵌**后，loader 走 tinygltf 解码 + 重编 PNG，
   与原路径 stb 直解 JPEG 可有 ±1~3 舍入差（实测 48/1M texel），在极小模型陡峭边缘放大到
   个别像素（max 121）。因此 E2E 用**物体包围盒逐像素相等**作强门禁（几何/位置/朝向的铁证），
   像素差用弱阈值（mean<0.2、>2 差像素<2%）。

### 新增/改动文件
- 新增 `interface/mesh_transform.h`（S1） + `interface/mesh_transform_test.cc`（14 用例）
- 新增 `src/gltf_saver.{h,cc}`（S2） + `test/jpov_gltf_saver_test.cc`（9 用例）
- 改 `tools/common/utils.h`（+`EditTimestampSuffix`） + 新增 `tools/common/utils_test.cc`（3 用例）
- 新增 `demo/editor/editor_save.{h,cc}`（S4） + `demo/editor/editor_save_test.cc`（5 用例）
- 新增 `demo/editor/editor_save_e2e_test.cc`（S5，headless 需 DISPLAY）
- 改 `demo/editor/editor_app.h`（+保存按钮行、+状态提示行、+CPU 快照钩子）
- 改 `demo/editor/jpov_model_editor.cc`（启动时读 CPU 快照 + 源路径）
- 改 BUILD（editor/src/interface/test/common）、`build_jpov_model_editor.sh`

### 验证
- 单测：`mesh_transform_test` 14/14、`jpov_gltf_saver_test` 9/9、`utils_test` 3/3、
  `editor_save_test` 5/5、`editor_drag_ownership_test` 全过、`model_placement_test` 全过。
- E2E：`editor_save_e2e_test` 过（包围盒逐像素相同）。
- 负向验证（均正确 FAIL）：① 骨架修正项只给根 → mesh_transform_test FAIL；
  ② saver 漏逆坐标映射 → 往返稳定测试 FAIL；③ 保存控制器去掉“保存中”互斥 → FAIL；
  ④ IBM accessor count 写错 → 新增的文件直查测试 FAIL。
- 全量 `bazel test //... --jobs=1` → **72/72（202 cases）**，零回归。
- 真交互二进制：`build_jpov_model_editor.sh` 编译+打包字体 OK；Xvfb 下启动加载模型 + CPU 快照 OK。
- **CI 未跑**（按 §2.5：等 Danis 认可交付物 → 跑唯一一次）。分支 `feature/20260913-jpov-model-editor-save`（未 push）。


---

## 8. 待你拍板（3 个）

1. **保存目录**：需求写"elf 同级目录"，但 elf 同级 = `output/jpov_model_editor/`，与模型无关；
   我倾向 **glb 同级目录**（`<stem>_edit<时间戳>.glb` 就落在原模型旁边），elf 同级作为兜底。
   要不就严格按需求走 elf 同级？→ **请选一个**。
2. **文件后缀形态**：我拟 `<stem>_edit20260912-225013.glb`；
   你也可以要 `<stem>.edit.20260912-225013.glb` 或 `<stem>_edit_20260912_225013.glb`。
3. **保存时 `scale` 怎么处理**：我拟**保存前把 scale 强制当 1.0**（骨架一致性严格成立；滑条上的 scale 只是预览）。
   若你希望"缩放也存进去"，请确认可接受"带骨模型的 pose 会在缩放后失真"（或我把它整段塞进 `center` 语义另论）。
