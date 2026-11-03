# JPOV 水花喷泉 — S5 FLOW flipbook（粒子）

> 日期：2026-11-02 ｜ 状态：已定稿（Danis 验收通过：S5 FLOW 这一版）｜ 工作区：`/james_pm/ai_infra_1`
> 关联：`jpov_particle_fire_design.md`（同一套广告牌粒子内核）、`jpov_particle_embers.md`（加法层先例）

## 0. 一句话

把火焰验收过的 **F5 关键帧 → S5 中点 → 两段链 12 帧** 方法套到水花上：素材是
`/james_pm/danis/water_splash.png` 里横排的 5 个水花（F1..F5）。查看器里是一**座**喷泉，
两层粒子：**水柱核心** + **顶点死亡点 spawn 的水花**，两层共用同一张 **S5 FLOW** 图集。

## 1. 素材与产线

| 步骤 | 做法 | 产物 |
|---|---|---|
| 抽帧 | 按亮度取 alpha，去水印，按 bbox 中心归一到同尺寸画布（最长边 80%） | `F1..F5`（512²，白 RGB + alpha=形状） |
| S5 | `S_i = morph(F_i, F_{i+1}, 0.5)`，`S5 = morph(F5, F1, 0.5)`（回绕） | `S1..S5` |
| 链 | 6 条两段链：`S1→S2→S3 / S2→S3→S4 / S3→S4→S5 / S4→S5→S1 / S5→S1→S2 / S1→S3→S5` | 每块 12 帧（6+6，中点共享不重复） |
| 图集 | 12 列 × 6 行，256px/cell；行序按 JPOV `CellUV`（`vrow = tilesY-1-row`）翻转 | `assets/water/water_s5_flow_12x6.png` |

形变与图集生成的**可复用工具**在 `tools/jpov/genVolume/flipbook_s5/`
（`extract_keyframes.py` / `build_s5_atlas.py` / `morph_flow.py` + README，
说明「为什么绕一层 S、为什么只留 FLOW、行序/引擎契约、粒子侧配套经验」）。

**只保留 S5 FLOW 这一版**（2026-11-02 定稿）：
- `F5 直出`（5 帧）偏硬、抽帧感明显；
- `S5 PROFILE` 会把 C 形/星形/斜带并成一根"主柱"，出图偏糊偏"充气"；
- **S5 FLOW** 的细丝/飞沫保留最好，形变最连贯。

## 2. 结构（两层）

1. **水柱核心**（`MakeWaterCoreConfig`）：窄锥（2°），**体积由小到大**
   （`size_curve` 0.20 → 0.65 → 1.00）。寿命取 `t_apex = v/g`（随「喷射高度」线性缩放，
   保证无论喷多高都死在抛物线顶点），死亡点就是下一层的生成点。
2. **顶点水花**（`MakeWaterSplashConfig`）：`rate = 0`，只在核心死亡点 `SpawnAt`，
   朝上 85° 锥 + 重力 −8 ⇒ 伞形外抛再落回。

两层都 `align = kView`：**俯瞰不穿帮**（`kVertical` 在 70° 俯视时 quad 侧对相机、
会退化成竖片/细线）。

粒子预算 40~200（默认 100，核心占 55%），水花大小默认 0.36。

## 3. 查看器

- 顶部「场景」下拉：**火焰** / **水花喷泉**。选中水花时面板**只画水花自己的控件**
  （水花数量 / 水花大小 / 喷射高度 / 张开 / 水花不透明度 / 水花加法），
  不出现火焰、烟、温度、燃料块、火星、软深度、朝向等火焰侧 UI。
- **水花加法**：勾选 = 加法集（发光叠加，看起来通透）；取消 = 覆盖集（WBA/over，偏"实体水"）。
  两种都保留，可现场 A/B。
- 水花不透明度只乘在**顶点水花**上（水柱核心有自己的 life 渐变，避免把柱体一起压暗）。

## 4. 调参踩过的坑

| 改动 | 结果 |
|---|---|
| 粒子放大到 0.13m + 数量 ~90（双喷泉时期） | 棉花团，水花轮廓消失 |
| `emissive=false` 且只用覆盖混合 | 厚重白烟/白玉；**加法**才通透 |
| 发射锥角开到 2× | 整片糊成蒸汽 |
| 寿命 < `2v/g` | 粒子在最高点就死 → "只喷不落" |
| 顶点 spawn 概率过高（0.30） | 顶点变成一颗"悬停的棉球"，两帧几乎不变 |
| 水滴贴图缺失 → `texture_id=0` | 纯白 quad，明显**方块** |
| 图集不开 mipmap | 3cm 见方的 256px cell 严重闪烁/盐粒 |
| 核心速度区间太宽（2.0~2.6） | 相邻粒子间距忽大忽小 → 柱体"串珠" |
| 扁盒子当水面 | 侧壁露出暗色楔形 → 改**无厚度 quad** |
| 核心漏设 `sheet_time_mode`（默认 kFps） | 每颗粒子按 fps 走完整张 atlas，回不到"随机选一块"的形变链 |

## 5. ⚠️ 顺带发现的引擎 bug（已修复：2026-11-03 `feature/20261103-jpov-particle-velocity-align`）

`src/particles/particle_renderer.h` 顶点着色器的 `kVelocity` 分支原来是：

```glsl
right = normalize(cross(u, n));       // OK
up    = normalize(cross(right, u));   // ❌ 得到的是面片【法线】= 视线方向
```

`right ⟂ u` 且 `right ⟂ n` ⇒ `cross(right,u) = ±n`，quad 侧对相机，投影成一条横线。
宽锥水滴层（速度接近水平、与视线垂直）会整片变成横条 —— 这就是上一轮"外缘高亮圆片/塑料感"的根因。
**已改为** `up = normalize(cross(n, right))`（= 速度在屏幕平面上的投影），面片恒正对相机、
拉伸轴 = 速度轴。配套：
- 火星/余烬层改用 `kVelocity + stretched`（viewer 面板新增「火星速度对齐」「火星拉伸」两项）
- 新增 GPU 像素测试 `test/particles:particle_velocity_align_test`（旧写法下三条断言全红）
- 粒子 gold 的火星层同步换成 `kVelocity + stretch 3`，两张 gold 已重出
本 PR 的水花不复用该分支（一律 `kView`，不需要沿速度拉伸）。

## 6. 已知短板

- 水面**没有反射/折射/涟漪**（object3d 无半透明水体通道），平水面只能靠颜色表达。
- 水花贴图是 5 张**不同的**水花照片（不是同一朵的时间序列），S5 链在形状之间游走。
- 100 粒预算下柱身仍有一点"珠链"；要更连续得加连续柱芯（网格带，或用 kVelocity + 拉伸做纵向流线）。

## 7. 怎么跑

```bash
./tools/jpov/build_particle_viewer.sh
output/jpov_particle_viewer/jpov_particle_viewer                      # 场景 → 水花喷泉
output/jpov_particle_viewer/jpov_particle_viewer --capture /tmp/w --water
# 可选：--water-count/--water-size/--water-opacity/--water-spread/--water-speed/--water-emissive
#       --cam-r/--cam-theta-deg/--cam-phi-deg
```
