# 粒子软深度（soft particles）

> 目标：粒子（火/烟/火星）与不透明几何**相交的那条硬切线**要消失。
> 接口：`ParticleRenderCommand::soft_depth_m`（per-command，单位米）。

## 1. 现状（为什么这里是硬切）

粒子 pass 用的是 WBO 半透明目标（`src/particles/particle_renderer.h`）：

- FBO 只有 `accum / reveal` 两个颜色附件，**没有深度附件**；
- 所以 `glDisable(GL_DEPTH_TEST)`，深度可见性靠 FS 手动作：

```glsl
float scene_z = ZView(texture(uSceneDepth, uv).r);   // 不透明场景深度（MRT#1）
float z       = ZView(gl_FragCoord.z);
if (z > scene_z + 0.02) { discard; }                 // 硬 0/1 + 2cm 偏置
```

这条判据的输出只有「留 / 丢」，于是粒子被几何切过的地方就是一条刀切边。

## 2. 为什么不能改成硬件深度测试（也不是性能问题）

| 想法 | 为什么不行 |
|---|---|
| 给 WBO FBO 挂上 `scene_depth` 作深度附件，开 `GL_DEPTH_TEST` | 同一张深度纹理既要当附件又要被 FS 采样 = feedback loop（未定义行为）；另外本 pass 是 **1/2 分辨率**，全分辨率深度附件尺寸对不上 |
| 拷贝一份深度专门给硬件测试用 | 要多一次深度拷贝；而且 FS 里有 `discard`，多数驱动本来就会退化到 late-Z，early-Z 收益≈0 |
| 手动 `discard` 会不会「废掉 GPU 的深度筛选」 | 这个 pass 本来就没有深度附件，early-Z 无从谈起 —— 手动测试不是妥协，是这套结构的唯一解 |

**真实成本在 fill-rate，不在深度测试**：已经靠「1/2 分辨率（片元数 1/4）+ WBO 免排序」压住了。

**软深度本身几乎免费**：深度 fetch 这一行**本来就在取**（硬判据也要用），软深度只是把
「二值比较」换成「减法 + `clamp`」，多 3~4 条 ALU。

## 3. 实现

```glsl
if (uSoftDepthEnabled == 1) {
    a *= clamp((scene_z - z) / uSoftDepthM, 0.0, 1.0);   // 视空间米
    if (a <= 1.0 / 255.0) { discard; }
} else if (z > scene_z + 0.02) {
    discard;                                             // 旧硬判据（A/B 用）
}
```

语义（`d = scene_z − z`，单位：米，正 = 粒子在几何前面）：

| d | 行为 |
|---|---|
| `d <= 0` | 被遮挡，丢弃 |
| `0 < d < soft_depth_m` | `alpha *= d / soft_depth_m` 线性淡出 |
| `d >= soft_depth_m` | 不衰减 |

三个实现要点：

1. **必须乘在 `WeightWbo` 与两个 WBO 输出之前**。WBO 用 `1−Π(1−α)` 解析覆盖率，
   只有把衰减写进每片元的 `α`，与其它粒子的合成才是对的；合成完再乘会破坏代数。
2. **用视空间米，不用窗口深度差**。窗口深度非线性（近处精度高、远处塌陷），
   直接用会导致近处硬、远处烂。`ZView()` 已在 FS 里（硬判据本来就要转）。
3. **关闭时零回归**：`std::optional` 为空 ⇒ 走旧 `+0.02` 硬判据，像素逐字节不变
   （gold 已锁）。

## 4. 接口

```cpp
struct ParticleRenderCommand {
    ...
    // nullopt = 关闭（旧硬判据）；有值 = 淡出过渡距离（米，必须 > 0）
    std::optional<float> soft_depth_m;
};
```

用 `optional` 而不是「0 = 关闭」的哨兵值：0 作为过渡距离本身无意义，
把它当开关就是隐式语义（规则 6.3）。非法值（<= 0）在 `ParticleRenderer::Draw` 里 `CHECK` 崩。

参考取值：火 puff 尺寸 0.15~0.21 m → `soft_depth_m ≈ 0.08`（约半个 puff）。

## 5. 顺手删掉的东西

第三个颜色附件 `att2 = z·α·w`（`FragDepthAccum`）从引入起就没被 `Composite` 采样过 ——
纯带宽浪费（每片元多一个 float 附件 + 每帧 clear）。本次一并删除，WBO 从 3 附件降到 2 附件。

## 6. 已知限制

- **只对不透明场景深度生效**：粒子 vs 粒子（火穿烟）不会软 —— 那是深度剥离的活。
- 深度纹理在 1/2 分辨率 pass 里按归一化 UV **双线性**采样，遮挡物轮廓处会取到
  邻域平均深度，理论上沿轮廓有 1~2px 的软化/晕散。视觉上可接受，需要更硬可改
  `texelFetch` 最近邻。
- 软深度只在**视空间距离**一个维度上淡出，不改变粒子的颜色/亮度衰减。

## 7. 验证

- `//tools/jpov/test/particles:particle_gold_test`：两张 gold（关闭 / `0.08` 开启）逐字节比对，
  外加**非恒真断言** —— 两张渲染图必须有 ≥100 像素差（防 uniform 未绑定 / 淡出被短路时
  gold 静默恒等）。
- viewer：面板「软深度」开关 + 「软深度距离」滑条，实时 A/B。
  headless 复现（同一场景，只差软深度）：

  ```
  ./jpov_particle_viewer --capture /tmp/ab_off --fire 0 --fuel-y -0.24 --fuel-scale 0.30 --soft-depth 0
  ./jpov_particle_viewer --capture /tmp/ab_on  --fire 0 --fuel-y -0.24 --fuel-scale 0.30 --soft-depth 0.08
  ```

  `--fuel-y` 压低燃料块是为了让火焰底部与地面相交，得到更大的观察区。
