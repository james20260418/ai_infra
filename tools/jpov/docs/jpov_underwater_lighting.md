# 水下照明衰减（照明侧，第一版）

> 2026-11-26。范围 = Danis 定的「能量基本盘」：**只做衰减**，不做 CSM、不做波面透射率/波光、
> 不改光的方向。下一版（反推水面点 + 局部波法线 + caustic）另开 PR。

## 1. 为什么需要「照明侧」

水体的物理分两半：

| | 在哪 | 做什么 | 状态 |
|---|---|---|---|
| **视线侧** | `src/water/water_medium_math.h`（合成趟） | 相机 → 物体的吸收 + 单次散射注入 | 已有（M1） |
| **照明侧** | 本文件 + `water_underwater_shading.h`（主 pass） | 光从水面进到物体 | 本版 |

只做视线侧 ⇒ 水下物体按「空气中的太阳/环境光」被照亮：5 m 纯水的红通道直射会高估
≈9 倍，物体偏亮偏暖。补上之后「水下发青」是算出来的，不是后期染色。

## 2. 公式（唯一口径）

```
d = level_y − p.y                      // 该片元自己的深度（>0 才衰减）
直射：E_w(λ) = E_air(λ) · (1−R) · exp(−σt(λ) · d/cosθw)
环境：L_w(λ) = L_air(λ) · 0.94 · exp(−K_d(λ) · d)
σt = σa + σs
K_d = σa + (1−g)·σs                    // 折合衰减
```

- **路径长度用 `d/cosθw`**（Snell 后的水内角），与介质 L0 同一个口径；但**方向不折**
  （PBR 仍用空气侧 `L`），所以界面增益里**不含** `cosθa/cosθw`（那项是光束变窄的辐照度，
  与 `N·L` 口径配对时才成立）。
- `(1−R)` 是界面菲涅尔透射（45° ≈ 0.972、60° ≈ 0.94、80° ≈ 0.66）。
- `0.94` 是漫射天空的界面平均透射率（`water_optics.h::kAmbientInterfaceTransmittance`）。
- σ 来自 `WaterMedium`（与 `medium.enabled` 无关：可以只衰减照明、不画水体体积做 A/B）。
- 水上 / 片元在水面之上 ⇒ 增益恒 1（逐字节零回归）。

数量级（默认 σ、太阳 35°）：

| 深度 | 直射 R/G/B | 环境 R/G/B |
|---|---|---|
| 2 m | 0.16 / 0.68 / 0.79 | 0.33 / 0.81 / 0.87 |
| 3.5 m | 0.09 / 0.63 / 0.75 | 0.15 / 0.71 / 0.82 |

## 3. 落在哪些 pass

- **三个实心 PBR**：`object3d` / `instanced` / `skinned`（含 cutout/full 变体）——
  片元最后一行之前乘增益；三份 shader 拷贝拼同一段共享 GLSL。
- **条带（strip）**：折线 / 平滑两条 pass 的 `SkyLightRadiance` 里乘增益（逐片元 `vWorld`）。
- **不做**：水皮（表面 d=0）、水体介质合成趟（已经是物理积分，再乘就双重衰减）、
  无光照的图元/粒子/文字/体积火。

## 4. 开关（`WaterCommand`）

```cpp
bool attenuate_ambient = false;   // 环境光衰减
bool attenuate_sun = false;       // 平行光衰减
```

两个独立、默认 false（零回归）。`attenuate_sun=true` 但没有 `cmds.sun` 时 Renderer 直接 CHECK；
两个都开而 σ 全 0 也 CHECK（那是参数漏填）。

## 5. 验收场景：skylight viewer 场景 5

- 海面 = 场景 4（Ocean Lab）的**同一份默认海面**（谱/介质/风），只把水位抬到 3 m
  ⇒ 海底 −0.5 m，`ViewConfig` 的 target（恒为原点）落在水下、物体在画面中部。
- 相机：`phi=18°, R=5`，水面下 ≈1.45 m（离波面足够远）。
- 内容：海底（object3d）+ 三个方块（低反 / **镜面金属** / 高光）+ 橡树 ×3（instanced，
  一次 draw）+ 蓝人 ×3（skinned，一次 draw）+ 两条竖直条带（**折线 / 平滑各一条**）。
- 面板：天体仰角/方位/浊度 + **ambient 衰减 / 平行光衰减** 两个勾选框 + **水体介质** +
  时间；另有一行只读读数（海底增益的 R/G/B），因为观感差异会被水体注入色吃掉，数字更可信。
- **水下关 medium 是支持的调试组合**（2026-11-27）：没有水体就没有「深海环境」可反射，
  水皮在水下支路把环外（TIR，F 恰好为 1）改成 **α=0 不覆盖**（露出场景），窗内保留 F；
  不会再出现「环外 α=1 ⇒ 黑环」，Renderer 也不再 CHECK。这样可以把
  「视线侧吸收」与「照明侧衰减」拆开单独看。

复现：

```bash
cd output/jpov_skylight_viewer
./jpov_skylight_viewer --underwater            # 交互
./jpov_skylight_viewer --capture-underwater out # 四张 A/B（关/仅环境/仅直射/全开）
```

## 6. 已知缺口（本版故意不做）

1. **环境镜面（IBL specular）不衰减**：它采样天空探针，水下仍映「没折射的天」。
   验收场里的金属方块因此几乎不随开关变化 —— 这是最明显的已知缺口。
2. **不采 CSM**：水下物体仍按空气侧阴影贴图（known_issue §11.1 的老问题）。
3. **没有波面透射率 / caustic**：太阳的入射点、局部波法线、聚焦项都留到下一版；
   这一版的 `(1−R)` 用的是**平面界面**（法线 = +Y）。
4. **点光源不衰减**：只处理了全局 ambient 与平行光（Danis 定的范围）。
5. 不改光的方向（无折射、无图像位移）。

## 7. 测试

- `test/water:water_underwater_shading_test`（7 cases，GL-free）：手算对拍 45°/5 m、
  开关独立、水上恒等、更深更暗、低太阳更暗、太阳在地平线下无直射。
- `WaterSkinV2Gpu.UnderwaterWithoutMediumLeavesTirRingUncovered`：水下 + 无介质时
  环外 α 不得为 1（否则黑环），层色必须为 0。
- 回归：`test/water:all` 9 targets / 93 cases、`test/object3d:all` + `test/skeleton:all`
  30 targets 全绿（关态对既有 gold 逐字节不变）。
