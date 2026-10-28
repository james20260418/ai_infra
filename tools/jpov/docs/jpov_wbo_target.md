# JPOV WBO 目标（WboTarget）· 共享件与治理规则

> 日期：2026-10-20 ｜ 状态：**已实现**（PR #208）｜ 决策人：Danis
>
> 关联：`jpov_fragment_overdraw_design.md`（WBO 分级纹理）、`jpov_particle_buffs.md`
> （覆盖集/加法集）、`jpov_strip3d_translucent_design.md`（新条带层，第一个按本规则接入的使用者）。

## 0. 一句话

把「WBO 的 FBO / 附件 / clear / 混合 / 还原」收敛成一个类 `WboTarget`，
把「权重函数」和「全屏解析」收敛成一份 GLSL（`src/wbo/wbo_shader.h`）。

## 1. 重构前：同一套代数被手抄三份

| 使用者 | 分辨率 | 附件（顺序 = attachment 序号） | 过滤 |
|---|---|---|---|
| `ParticleRenderer` | 1/2 | accum RGBA16F / reveal R16F / additive RGBA16F | LINEAR |
| `VolumeFireRenderer` | 1× | accum RGBA16F / reveal R16F / **depth R32F（死）** | LINEAR / NEAREST |
| ~~`Renderer` 玻璃（#176）~~ | 1× | ~~accum / reveal / depth~~ | 已于 2026-10-27 **移除** |

每个使用者各自手写这四件事：

1. 附件格式与过滤；
2. clear 值（`accum=0 / reveal=1 / additive=0 / depth=0`）；
3. 逐 draw buffer 的 blend（`accum ONE,ONE` / `reveal ZERO,1−SRC` / `additive·depth ONE,ONE`）；
4. 用完把 blend **显式还原**（`GL_COLOR_BUFFER_BIT` 的 `glPopAttrib` 不保证覆盖 blend func）。

四件事 × 三份 = 12 个可以悄悄抄错的地方。**而且 gold 只和自己比**（每个 gold 只覆盖自己的渲染器），
抄错了也不会红 —— 只会出现「同一像素，粒子和火看起来不一样」这种极难定位的现象。

真实发生过的同类 bug：

- **PR #204**：bloom pass 里 `glDisable(GL_BLEND)` 之后没恢复 → 后续 tone map / SMAA / 全部 2D·UI
  失去 alpha 混合，字形四边形整块写成不透明白 → UI 糊成白块。
- 粒子曾经挂着一个 `z_view·α·w` 的 depth 附件，`Composite` 从来没采样它 → 纯带宽浪费（已删）。
- 体积火**现在**就挂着同款死 depth 附件（本次一并删）。

## 2. 结构

```
src/wbo/
  wbo_target.h / .cc    WboTarget（4 个附件槽位，按需申请）+ WboTargetSpec
  wbo_shader.h          kWboWeightGlsl（权重，唯一权威副本）
                        kWboCompositeVs / kWboCompositeFs（全屏解析）
```

固定槽位（不随使用者变化）：

| 槽位 | 内容 | 格式 | 谁用 |
|---|---|---|---|
| 0 | accum | RGBA16F | 恒有 |
| 1 | reveal | R16F | 恒有 |
| 2 | additive（加法集） | RGBA16F | 粒子（以及将来的条带发光） |
| 3 | depth（代表深度） | R32F | **当前无使用者**（玻璃一栈已移除；留槽位是给将来的「带深度合成」层） |

未申请的槽位在 `glDrawBuffers` 里是 `GL_NONE`：FS 里对应 `location` 的写入被丢弃（合法），
所以同一个 FS 源码可以复用到不同附件组合的目标上。

## 3. 新加一个 WBO 层时照这几条做

1. **只申请你要读的附件。** 没有任何采样者的附件 = 纯带宽 + 潜在的"以为它有用"的误解。
2. **用 `BindForDraw()` / `EndDraw()` 包住绘制**，不要自己写 blend func。`EndDraw()` 会把四个槽位的
   blend 都回写成引擎默认的 `(SRC_ALPHA, ONE_MINUS_SRC_ALPHA)`。
3. **权重只从 `kWboWeightGlsl` 注入**（`std::string(head) + kWboWeightGlsl + tail`），不要复制函数体。
4. **解析用 `kWboCompositeFs`**（覆盖集 over + 加法集求和）。
5. **同一个目标内所有使用者必须用同一个权重函数。** 加权平均是 `Σw_k c_k / Σw_k`，
   混入两套权重尺度会把平均色整体偏置 —— 这是 "条带不要和体积火共用 FBO" 的原因
   （体积火权重恒 1，条带用深度权重）。
6. **1× 层用 `linear_filter=false`**（NEAREST，省带宽且 1× 上插值没有意义）；
   低分辨率层用 `true`（要靠 LINEAR 上采样）。
7. **跨目标的层序必须显式写死**（粒子 → 体积火 → 条带 → 雾），不靠调用顺序。
   WBO 只保证**同一目标内**的顺序无关；两个目标之间的合成是有序的。

## 4. 契约测试

（**已删除**：`test/wbo:wbo_weight_contract_test` 钉的是「`object3d_renderer.h` 里玻璃 shader 的
内联权重副本 vs `wbo_shader.h` 的权威副本」。2026-10-27 玻璃一栈移除后内联副本不复存在，
权重实现只剩一份，文本契约也就没有可比的第二方了。）

## 5. 剩余状态

四个使用者里三个走 `WboTarget`（粒子 / 体积火 / 条带），槽位 3（代表深度）当前空置。
玻璃那一份手写 FBO 已随玻璃 pass 一起删除，不再有「两套槽位布局」的问题。

## 6. 验证

- `bazel test //tools/jpov/test/... --local_test_jobs=1` → **81 tests / 123 cases 全过**
  （并行跑会因 Xvfb 抢占互踢，属既有环境问题，与本次改动无关）。
- 粒子 gold（逐字节 205,826 B）、体积火 gold（逐字节 + 暖色像素数断言）、玻璃 gold（原阈值）
  全部保持通过 —— 说明重构是**行为保持**的。
- 删除体积火的死 depth 附件**不改变任何输出像素**（无人采样），gold 作证。
