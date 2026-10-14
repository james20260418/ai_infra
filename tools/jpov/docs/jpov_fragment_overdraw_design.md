# JPOV 低分辨率半透明粒子 pass（WBO 分级纹理）· 设计

> 日期：2026-10-14 ｜ 状态：**设计 + MVP 实现（1/2 级）** ｜ 关联：PR #176（透明物体 WBO）、`jpov_particle_fire_design.md`。
> 决策人：Danis。

## 0. 一句话
新增一个与 `fire_fog` 平级的 `ParticleRenderer`：把广告牌粒子画进**低分辨率（先 1/2）的 WBO 半透明累积纹理**
（accum/reveal/depth），再**解析合成**进 HDR，随后交给既有 froxel/玻璃合成 → 后处理。

## 1. 要解决的问题（为什么不能靠 early-Z）
半透明多层的 overdraw **在 sprite 栈内无解**：透明片元**不写深度**，所以层与层之间互不遮挡，
**early-Z 只对不透明几何有效**；多层烟灰叠加时，每层都跑 FS，填充 = Σ覆盖。tile culling（灯那种）帮不上核心。

overdraw 账：**粒子数 × 平均屏幕覆盖 × 分辨率**。只有四条线：**降覆盖 / 降数量 / 降分辨率 / 提前剔除**。

## 2. 决策
1. **引擎原语 = 低分辨率半透明 pass**（成本 ∝ 分辨率²：1/2 → 片元 ÷4；1/4 → ÷16）。本版 **先只 1/2**。
2. **复用 PR #176 的 WBO 代数**（McGuire & Bavoil 2013）：与透明物体同套 accum/reveal/depth 语义，
   **顺序无关 → 免"多层烟"排序**。
3. **独立 renderer**：粒子有**自己的低分 WBO 目标**（不挤占 1× 玻璃 FBO）。
4. **密集烟/近景主角火**：将来可换**体渲染**（有界成本，见 fire_fog/fire_hero）。
5. **VRS 关系**：真 GPU 上 VRS 更省更干净，但 **GL3.3 无 VRS** → 多分辨率缓冲是 GL3.3 的对应正解。

## 3. 架构 / 数据流
```
opaque 3D → HDR
  → 透明物体 WBO（#176，1× 玻璃 FBO）
  → 【新】粒子低分 WBO（ParticleRenderer，1/2）: accum RGBA16F / reveal R16F / depth R32F（无深度缓冲）
        FS 手动深度测试（对不透明场景深度）；输出 WBO 三附件（同 #176）
  → 【新】解析合成：C=accum.rgb/accum.a，cov=1−reveal；over 混合进 HDR
  → froxel 雾 + 玻璃合成（#176 路径）
  → bloom → tonemap
```

## 4. 三条硬约束（务必遵守）
1. **跨分辨率/跨集合合并不丢序**：两个 WBO 集合并 = `accum=A+B·reveal_A`、`reveal=reveal_A·reveal_B`，
   **必须先定层序**（固定 1/4→1/2→1× 由远到近，或借 depth per-pixel）。**不能随便叠。**
2. **同一层不跨分辨率**：分辨率按 **emitter/层** 分配（整层 1/4），别把一层拆两半。
3. **每级 = 独立一次 draw**（不同 viewport），不是一次 MRT；+ 低分→高分**上采样接缝**软边（火烟可接受）。

## 5. 将来（TODO）
- **多级分辨率**（1/2、1/4）按 `emitter.render_scale` 路由 + 层序合并。
- **MBOIT（矩方法 OIT）**：每像素 k 个深度矩 → 透射率 T(z) 重建更准 → **"每像素多放内容 → 更精确 Z 顺序"的连续旋钮**（WBO = k1 特例）。做透明累积层的**可演进接口**。
- **软深度检测**（soft particles / depth fade）：靠近平面的片元淡出（同有一张场景深度，易加）。
- overdraw 调试视图（仿 UE "Quad Overdraw"）。

## 6. MVP 实现现状
- `src/particles/particle_renderer.{h,cc}`：1/2 WBO FBO + instanced 广告牌 + WBO FS（深度测试 + 三附件）+ `Composite()`（解析 over 进 HDR）。
- `Renderer`：不透明 3D 后、froxel 前调用（`Begin`/`Draw`/`Composite`）。
- `jpov_particle_viewer`（`build_particle_viewer.sh`）：地面 + 桌子 + 火/烟，验证整条链。
