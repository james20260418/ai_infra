# JPOV 覆盖率保持的 alpha mip（coverage-preserving alpha mipmap）

> 2026-10-15。治 alpha cutout（`alphaMode=MASK`）资产远景「变稀 / 整片消失」。

## 一、问题

cutout 片元的保留判据是「采样 `alpha >= uAlphaCutoff`」（FS 里 `texture(...).a < cutoff → discard`）。

`glGenerateMipmap` 对 RGBA 做线性盒式平均：叶卡 / 蕾丝这类贴图里**大量透明背景**会把 alpha
平均下来，于是「阈值裁剪后仍可见的纹素占比（**覆盖率**）」随 mip 层级**逐级缩水** ——
远景时镂空资产变稀、甚至**整片消失**。（实测 `leaf_dragonclaw_1.png`：coverage0≈0.323；
普通 mip 到最高层 alpha 趋成均匀 ≈0.32 < 0.5 ⇒ 远景整片被裁光。）

这是 alpha-test foliage 的经典问题。

## 二、解法

在 **mip 生成侧修正 alpha**（不是 shader、不是后处理）：

- 颜色通道照常盒式平均；
- **alpha 通道逐级按覆盖率反解一个阈值后二值化（0/255）**，令每一级
  「alpha≥cutoff 的纹素占比」**恒等于 mip0**。

这样着色器里那个固定 cutoff 在每一级都命中正确比例，覆盖率不缩水。

实现：`tools/jpov/src/coverage_mipmap.h` —— **GL-free 纯函数** `BuildCoverageMipChain()`：

- **level0 原样保留**（近景逐字节零回归；其 alpha 可能带抗锯齿中间值，交原阈值裁剪）。
- 覆盖率按 mip0「alpha >= cutoff 的纹素占比」定义（与着色器判据一致）。
- 层级尺寸按 GL 归约规则 `max(1, floor(尺寸/2))` 递减到 1x1，与 `glGenerateMipmap`
  生成的层级数一致。
- 临界档（同 alpha 值的并列纹素）用**确定性空间哈希**抖散，避免按扫描序聚成一堆；
  **绝不把纯空（alpha==0）纹素变可见**。
- 二值化用「直方图 + 临界档精确截断」⇒ `|可见纹素数 − coverage*N| ≤ 0.5`。

## 三、接入

### 1. TextureManager（底层选项）

`TextureOptions` 新增：

```cpp
struct AlphaCoverage { bool enabled = false; float cutoff = 0.5f; };
AlphaCoverage alpha_coverage;
```

- `cutoff` **必须与材质的 `alpha_cutoff` 一致**（覆盖率反解用）；启用时须 `mipmap=true`（否则 CHECK）。
- 启用时 `TextureManager::UploadRgba` **不调 `glGenerateMipmap`**，改为 CPU 算链 + 逐级 `glTexImage2D`。
- 去重键含覆盖率位（不同 cutoff = 不同 mip 链 = 不同纹理）。
- **默认关 = 零回归。**

### 2. glTF loader（自动，默认生效）

`Renderer::LoadGltf` 组装材质时：若 `alphaMode == MASK` 且 `alpha_cutoff ∈ (0,1)`，
就用 `{mipmap:true, alpha_coverage:{true, alpha_cutoff}}` 注册其 **baseColor 贴图**。
⇒ **所有带 cutout 的 PBR 模型开箱即得正确 mip**，无需逐材质手配。

非 MASK（OPAQUE / BLEND）保持原样（不加 mipmap）。

## 四、验证

- 单测 `//tools/jpov/test/texture:coverage_mipmap_test`（7 例）：level0 逐字节保留；
  每级 alpha 二值；**每级覆盖率恒等 mip0**（含「朴素平均把稀疏结构覆盖率塌到 0」的
  **非恒真对照**）；层级尺寸遵循 GL 归约；全透明 / 全不透明边界；临界档空间铺开。
- 回归：`object3d` / `skeleton` / `vegetation` 本地全绿（含加载 MASK 资产 `lace_skirt`
  的 instanced gold —— 场景够近，mip 不改变构图，零回归）。
- 效果（植被 viewer 远景叶像素数）：dist45 线性 mip 99 → 覆盖率 613（参照无 mip 582）；
  dist65 线性 mip **0（叶子全没）** → 覆盖率 283（参照 280）。

## 五、边界（说清楚，避免过度期待）

- 只对 **alpha cutout（MASK）** 有意义；opaque 不用 alpha，blend 依赖连续 alpha（二值化会破坏）。
- 只取 **baseColor** 的 alpha（glTF 规定 cutout/blend 的 alpha 都来自 baseColorTexture）。
- 治的是「mip 让 cutout 覆盖率缩水」；治不了「叶卡小到亚像素整体闪烁」（那是 LOD / impostor 的活），
  也不做颜色渗色（可另加 alpha-weighted 颜色 mip）。
- 覆盖率保持是**正确性/一致性**，不是美术旋钮 ⇒ 参数就是材质已有的 `alpha_cutoff`，
  loader 自动派生，不额外暴露全局 k 系数。
