# JPOV 子渲染器隔离性审查（Renderer / 子 renderer 之间的资源独立性）

> 结论日期：2026-09-12
> 审查对象：`tools/jpov/src/renderer.{h,cc}` 及其子渲染器（object3d / skeleton / primitives2d /
> primitives3d / font2d / skydome）+ 三个资源管理器（MeshManager / TextureManager / ShaderManager）。
> 触发问题（Danis）：
> 「一个子 renderer 一定不要去依赖其它 renderer 的代码、or 数据结构、or 通道使用上的
> assumption。总 renderer 自己在切换不同子 renderer 时，不需要担心状态的『残留』。」

---

## 0. 一句话结论

**「代码 / 数据结构独立」成立；「通道与状态零残留」目前靠『每个 draw 自己把前置条件置零』
这种*局部*写法维持，不是*帧级*成文保证 —— 有三处缺口，其中一处（TEXTURE12）是真实通道冲突。**

方向是对的（Object3D 作范本、各子 renderer 平级），本文件把现状、缺口、修法落成契约。

---

## 1. 成立的部分（有代码支撑）

### 1.1 无跨子渲染器的代码 / 数据结构依赖

| 子渲染器 | include 了谁 | 说明 |
|---|---|---|
| `Object3DRenderer` | mesh_manager / shader_manager / texture_manager / interface:render_command / interface:camera | **不 include skeleton 任何头**（PR #91 剥离后的既定状态） |
| `SkeletonRenderer` | mesh_manager / shader_manager / texture_manager / interface:render_command / interface:camera / skeleton:skeleton | **不 include object3d 任何头** |

两者的 shader 各自独立成常量：obj3d 持 `kMeshFs3dPBR`（含点光源 + tile culling），
skeleton 持**自己的一份** `kMeshFs3dPBR`（无点光源、无 tile）。
这是 PR #91 明确的设计决策：

> 「一片 shader 字符串不能单测，就不要分开管理」—— 能独立成 program 的就独立，
> 宁复制粘贴换独立可测。

**结论**：符合 Danis 的要求（子 renderer 不依赖其它 renderer 的代码 / 数据结构）。

### 1.2 通道绑定是「前置条件成立式」自洽

关键事实（`object3d_renderer.cc` DrawObject3D，`:418-517`）：

* draw 开始时对 **全部 6 个材质通道 + `uHas*Tex`** 显式置值：
  有纹理 → bind + `uHas*=1`；无纹理 → **`else { uHas*=0; }`**（6 个通道各自都有 else 分支）。
* draw 结束时 `glActiveTexture(GL_TEXTURE0)`，且只在「有纹理」分支里 `glBindTexture`。
* `uTileLightIndices` 显式重绑 TEXTURE0。

`SkeletonRenderer::DrawSkinnedMesh`（`:144-215`）同构：6 个材质通道 + 阴影 7..11 全置零 + 复位 active unit。

**因此不存在**「上一个物体用了 T6，下一个物体假设 T6 还绑着」这类残留依赖。

### 1.3 program 名空间按子渲染器切分

`ShaderManager` 只按 **name** 缓存（`GetOrCreate` 幂等）。现有名字：

```
solid, text, image, solid3d, text3d,
draw_object3d_pbr, draw_object3d_pbr_full,      ← Object3DRenderer
skinned_mesh, skinned_shadow,                    ← SkeletonRenderer
shadow, sky, tonemap, pick,
bloom_prefilter, bloom_downsample, bloom_upsample, bloom_composite
```

各自独立，无重名。

### 1.4 GL 状态有边界成对约定

* 入口 `Renderer::Render` 设置 `BLEND / DEPTH_TEST / CULL_FACE / FrontFace(CCW)`。
* obj3d / skeleton 各自 `glPushAttrib(GL_ENABLE_BIT)` / `glPopAttrib()` 成对。
* 阴影 pass 出口显式 `glDisable(GL_CULL_FACE); glDisable(GL_DEPTH_TEST);`。
* `DrawHighlightPass` 出口复位 `glDepthFunc(GL_LESS); glDepthMask(GL_TRUE);`。

---

## 2. 三处成文契约缺口

### 2.1 🔴 TEXTURE12：真实通道冲突（**未修的隐患**）

事实：

* `SkeletonRenderer::DrawSkinnedMesh` 把 pose atlas 绑到 **`GL_TEXTURE12`**（`skeleton_renderer.cc:222`）。
* `Object3DRenderer` 的既有约定槽位表是 **`0=tile` / `1..6=材质` / `7..11=阴影 5 级联`**。
* **12 槽没有被 obj3d 认领，但对任何「想用 12」的新子渲染器完全不可见。**

这就是 Danis 说的「通道使用上的 assumption」：槽位表**只存在于注释里**，没有任何集中分配点，
也没有任何机制阻止两个子渲染器在各自不知情的情况下用同一个槽。

**附注（同类隐患）**：skeleton 的**阴影 pass** 把 pose atlas 绑到 **`GL_TEXTURE7`**
（`skeleton_renderer.cc:341`），而 TEXTURE7 在 obj3d 的语义是 **cascade-0 阴影贴图槽**。
不出问题的唯一原因是：skeleton 阴影 pass 与 obj3d 主 pass **不会同时活跃**（main pass 的
skeleton 在 T12，shadow pass 的 skeleton 在 T7，obj3d 的 cascade 在主 pass 用 T7+i）。
这是「槽位语义按 pass 局部重新定义」，与 2.1 是同一类问题。

### 2.2 🟡 program 命名无守卫

本仓库唯一一处真正存在「同 program 双名」风险的地方是**蒙皮 program**：

| 用途 | 注册名 | shader 对 |
|---|---|---|
| 主 pass | `skinned_mesh` | `kSkinnedVs` + `SkeletonRenderer::kMeshFs3dPBR` |
| 阴影 pass | `skinned_shadow` | `kSkinnedShadowVs` + `SkeletonRenderer::kShadowFs` |

**今天确实没有重复。** 但：

* `ShaderSource::fragment` 是 `const char*`，`std::string` 成员比较是**指针比较**而非内容比较 ——
  无法用「内容断言」直接挡（指针比较的断言 `kShadowFs != kMeshFs3dPBR` 恒真，抓不到任何东西）。
* 旧的合并方案里，蒙皮 program 曾复用 object3d 的 FS 名，PR #91 剥离后各自独立 ——
  如果这个约定不写下来，下一个人很容易把两个 program 指向同一片 FS 而无人察觉。

### 2.3 🟡 缺「整帧复位」的成文保证

当前正确性依赖 §1.2 描述的「逐 draw 前置条件置零」。这是**局部**写法，
不是可验证的**帧级**保证：

* 任何未来新增的子渲染器，只要忘了给某个 `uHas*Tex` 置零，
  就会静默串上一个子渲染器 / 上一帧残留的状态 —— 且**不会有任何报错**。
* 唯一的兜底是 §2.2 提到的 `GL error after DrawObject3D`（而那条在 llvmpipe 下长期是
  已知无害的 1280，见 SOUL.md 的 JPOV 已知无害告警）。

---

## 3. 修法建议（按「先立契约、再动刀」排序）

### 建议 A：中央 Texture Unit 契约（对应 2.1）

在 `src/` 下新增一份**唯一的槽位表**（建议 `renderer_texture_units.h`，纯常量 + 注释）：

```cpp
namespace jpov::tex_unit {
// 全局唯一的 texture unit 分配表。新增子渲染器**必须先在此认领槽位**。
//
//  0       tile light indices（obj3d 主 pass）
//  1..6    材质通道：baseColor / metallic / roughness / emissive / AO / normal
//  7..11   阴影 cascade 0..4（CSG，obj3d + skeleton 共用同一批级联贴图）
//  12      pose atlas（skeleton **主 pass**）
//  13      pose atlas（skeleton **阴影 pass**）
//  14..    预留（新子渲染器从这里往上认领）
}
```

并要求：

* 每个子渲染器的 `glActiveTexture` 只允许用本表里的名字，不再写裸数字。
* skeleton 阴影 pass 的 pose atlas 从 `7` 改到 `13`（避开与 obj3d cascade-0 的语义重叠）。
* 加一条单测：遍历源文件里的 `glActiveTexture(GL_TEXTURE<N>)`，断言 N 都在表内且不重复认领。

> ⚠️ 改槽位会动到 shader 的 sampler 绑定路径 → **必须单独一轮 + gold 图验证**，
> 不适合与本「先立契约」的 PR 混在一起。

### 建议 B：帧级状态复位（对应 2.3）

在 `Renderer::Render` 的 3D 段入口加一个**幂等的状态基线**（把当前隐式约定显式化），
例如「每帧进入 3D 前对所有已知 program 的 `uHas*Tex` 置零」。代价是一帧几次 `glUniform1i`，
收益是「新增子渲染器忘了置零」不再需要靠人肉 review 兜住。

### 建议 C：program 名去重守卫（对应 2.2，**本 PR 已实现**）

* ✅ **可执行、非恒真**：断言 `ShaderManager` 中已注册 program 的**名字互不相同** ——
  它在「同名二次注册会静默复用旧源码」这一真实行为上，对「同 program 双名」给出 gate。
* ⚠️ **不可用**：断言 `kShadowFs != kMeshFs3dPBR`。这是 `const char*` 指针比较，**恒真**，
  抓不到「两片内容相同的 shader 被分别注册」。期望值是「*内容*不同」，`const char*` 表达不了。
  若要真挡，得把 `ShaderSource` 改成 `std::string`（提供 `operator==` 内容比较）——
  那是接口改动，另议。

---

## 4. 检查清单（新增子渲染器时逐条过）

- [ ] 不 `#include` 其它子渲染器的头（只可依赖 `interface:*` 与三个 manager）。
- [ ] 不依赖其它子渲染器的数据结构（shader 常量各自持有一份，宁复制）。
- [ ] 所用 texture unit 已在 §3 建议 A 的表里**认领**，且不与任何 pass 重叠活跃。
- [ ] 每个 `uHas*Tex` / `uHas*` 开关都有 `else { ...0 } ` 分支（不靠默认值 / 不靠上一帧）。
- [ ] program 注册名**全局唯一**（不与其它子渲染器的语义重名）。
- [ ] 入口 `glPushAttrib`、出口 `glPopAttrib` 成对；不把 GL 状态留给下一个子渲染器。
- [ ] 若涉及深度 / 剔除 / 混合 / depthMask / point size 的改动，退出路径显式复位。
