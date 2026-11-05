# 透明层合成：froxel 雾 + N 个 WBO 纹理（多段积分）

> 日期：2026-10-17（设计）｜ 2026-11-05（落地改写）｜ 状态：**合成器已上线，生产者待接**

---

## 1. 一句话

**合成器只认「层」，不认「层是谁生产的」。**

任何一个 WBO 缓冲（加权混合 OIT 的三附件：accum / reveal / depth）都能解出一个
**等效层** `(C_premult, cov, z̄)`；合成器把它们按代表深度排序，与 froxel 雾
**交替积分**（N 层 ⇒ N+1 段雾），最后就地 over 进 HDR。

因此「玻璃 / 广告牌粒子 / 体积火 / 条带」不需要各自写一套与雾的合成逻辑 —— 它们是同一种东西。

---

## 2. 结构：按**趟**分层，不按名词分层

```
FogRenderer::Build(...)            // inject → reduce → scatter（3 趟，全在自建 froxel 纹理上）
        │  产出 FogField（scatter 纹理 + 网格参数）
        ▼
FogCompositeRenderer::Draw(field, scene_depth, camera, view_proj, params, layers)
                                   // 1 趟全屏：雾 + 层 交替积分，over 进 HDR
```

| 类 | 职责 | 是否碰调用方 FBO |
|---|---|---|
| `FogRenderer` | 把雾烘成**屏幕空间场**（τ, S per froxel） | 否（自建 FBO） |
| `FogCompositeRenderer` | 把场与 0..N 个透明层按深度合成 | 是（就地 over） |
| `TransparentLayer(Set)` | 层契约（POD，`src/transparent_layer.h`） | — |

**为什么不在 `FogRenderer` 里顺手合成？** 因为它有一千多行、四种完全不同的数据流
（团降维 / froxel 金字塔 / 全屏积分 / 点光源 tile culling）。合成只依赖「场」和「层」两样东西，
把它拆出来之后：froxel 可以用 dump 验证（已有），合成可以用 A/B 层验证，互不干扰。

**为什么不更细地拆成一个独立 pass？** 因为「雾段」与「层」在同一个像素循环里交替：
把层合成为独立 pass，要么多一次全屏 HDR 往返，要么那个类还是得调雾的积分核 —— 假解耦。

---

## 3. 层契约

```cpp
// src/transparent_layer.h
inline constexpr int kMaxTransparentLayers = 4;

struct TransparentLayer {
    unsigned int accum_tex;   // RGBA16F  Σ(预乘色×w, α×w)
    unsigned int reveal_tex;  // R16F     Π(1−α)
    unsigned int depth_tex;   // R32F     Σ(z_view×α×w)
    float scale;              // 解算分辨率 / 主视口分辨率（1 / 0.5 / 0.25）
};
```

**解算（GPU 内，逐像素）**：

```
T  = reveal                      // 层透射率
cov = 1 − T
C  = accum.rgb / accum.a · cov   // 预乘色（本层贡献的亮度）
z̄  = depth / accum.a             // 代表深度（视空间米）
```

**契约边界（生产者负责，合成器不校验）**：

1. 每个纹理对每个像素只代表**一个**层；
2. 各源沿视线**互不交叠**时合并才精确。

违反 2 时（如火焰粒子一半在玻璃前、一半在后），整个源会被丢到 `z̄` 的一侧。
这是 WBO 的固有边界 —— 要修只能上 MBOIT / 逐像素链表。

---

## 4. 多段积分公式

设层按代表深度升序 `z₁..z_N`，`T_i` 为其透射率、`C_i` 为预乘色；
雾累积 `(τ(z), S(z))` 来自 froxel；`z_op` = 不透明场景深度。front-to-back：

```
L = 0; W = 1; S_prev = 0; τ_prev = 0
for i in 1..N:
    zi  = min(z_i, z_op)
    L  += W · (S(zi) − S_prev)          // 雾段 (z_{i−1} → zi)
    L  += W · exp(−τ(zi)) · C_i         // 层自身（被**前方雾**衰减）
    W  *= T_i                           // 层遮挡后方
    S_prev = S(zi); τ_prev = τ(zi)
L += W · (S(z_op) − S_prev)             // 最后一段雾
a  = W · exp(−(τ(z_op) − τ_prev))       // 背景总透过率
out = L + dst · a                       // GL_ONE / GL_SRC_ALPHA
```

**退化验证**

| N | 结果 |
|---|---|
| 0 | `L = S_op, a = T_op` —— 与旧全雾路径**逐位相同**（gold `diff = 0`） |
| ≥1 | 通用循环 |

**为什么层色要乘 `exp(−τ(zi))`**：层的自发/反射光也要被它**前方**的雾衰减；
而雾段 `S(zi) − S_prev` 本身已含 0→zi 的衰减，所以只乘层累积 `W`，不重复乘雾。

**加性发射层（cov = 0）**也走这条公式，无需特判：「被前方烟压暗」是 `W` 的自然结果。

---

## 5. 本次接了什么 / 没接什么

| 项 | 状态 |
|---|---|
| 烘场 / 合成拆成两个类（`FogRenderer::Build` → `FogField`） | ✅ |
| 层契约 + 4 槽 GLSL + 按代表深度排序 | ✅ |
| `uHasFog=0` 快路径（无雾只有层） | ✅ |
| 纯雾路径逐像素不变 | ✅ gold `diff = 0` |
| 层路径的单元级验收（预乘 over / 排序 / 顺序无关） | ✅ `fog_composite_gpu_test` |
| **生产者接入**（广告牌粒子 / 体积火 / 条带变成层） | ⬜ **未接，下一步** |
| 低分层深度感知上采样（联合双线性） | ⬜ 现在用 NEAREST 取代表深度 |

**为什么这次不把生产者一起接上**：粒子的「加性 / 遮挡双 WBO 层」与条带、体积火都是
2026-10-17 之后各自长出来的链路，各自已经有一套 `Composite()`（见 `src/wbo/`）。
把它们改成「交出 WBO 三附件、由雾的合成趟统一积分」会同时改变三条链路的**画面**
（雾会插进层与层之间），需要重出这些链路各自的 gold —— 属于另一个 PR，
不应该和「改名 + 拆类」混在一起。

在此之前，`TransparentLayerSet` 在 `Renderer` 里恒为空：合成趟退化为纯雾路径，
与旧实现逐位一致。

---

## 6. 验证（2026-11-05）

- `bazel build //tools/jpov/...` 绿；`--config=windows` 交叉编译通过
- `//tools/jpov/test/fog`：`fog` / `fog_pointlight` 两组 gold `max-channel-mean-diff = 0`
- `fog_composite_gpu_test`（headless GL）：单层预乘 over、双层按 z̄ 排序、
  集合顺序无关、空集合零开销 —— 4 条断言各有一处负向验证（见测试文件头）
