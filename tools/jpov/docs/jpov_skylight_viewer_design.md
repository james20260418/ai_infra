# JPOV 天光查看器（skylight viewer）— 设计笔记

> 需求（Danis，2026-09-20）：新建一个 `skylight_viewer` demo，仿 model viewer 构造，
> 用来**肉眼验收天光（`SkyCommand`）**，尤其是新加的**夜色双色**。
> 本文件记录设计骨架与验收路径；实现对照本笔记核对。

---

## 1. 需求逐条对照

| 需求 | 实现 | 位置 |
|---|---|---|
| 仿 model viewer 构造 | 同构三段：主程序只装配 / 场景在 header / 渲染核心在 App header | `demo/jpov_skylight_viewer.cc` / `skylight_scene.h` / `skylight_viewer_app.h` |
| 场景固定三方块（低反/高光/金属） | `MakeBox` 造 1×1×1 方块 ×3，材质见下 §2 | `skylight_scene.h` |
| 大地用灰色 | 40×40 灰 quad（rough=1, albedo 0.35） | `MakeGroundQuad()` / `GroundMaterial()` |
| 视角变换与 model viewer 相同 | 直接复用 `jpov_viewer::ViewConfig` + `ApplyInput` | `view_config.h` |
| sun_dir 0~90° 可调 | 「太阳仰角 °」滑条 [0,90] | `skylight_viewer_app.h` `DrawLightPanel` |
| season 等天光配置都在 | 浊度 / 季节 R / 太阳强度 / 环境光强度 / 夜色强度 | 同上（共 6 滑条） |
| 能调到太阳落山（应全黑）→ 看夜色 | 0° 时白天项被 shader 精确压到 0，只剩夜色 | `sky_renderer.h` 的 daylight 因子 |
| 按「经典城市夜色」配夜色两色 | `kCityNightZenith / kCityNightHorizon` 常量 | `skylight_scene.h` |

## 2. 三方块材质（刻意隔离变量）

三块**尺寸相同**（1×1×1）、**albedo 相同**（0.55 中性灰），把「位置/大小/固有色」
三个变量压掉，只剩 metallic/roughness 的差异：

| 方块 | metallic | roughness | 用途 |
|---|---|---|---|
| 低反（左） | 0 | **1.0** | 纯漫反射 → 反映天光的**平均色与量** |
| 高光（中） | 0 | **0.05** | 只差粗糙度 → 看太阳/月色高光的**颜色与位置**（与低反对照即「高光从哪来」） |
| 金属（右） | **1.0** | 0.15 | 无漫反射、只反射环境 → 反映天光的**低频分布** |

> 为什么不用加载 glTF：model viewer 的目的是「看模型」，天光只是背景；本查看器的目的是
> 「看天光」，被测物必须**材质可控且常量**。每换一个模型就换一组变量，无法标定。

## 3. 光照滑条（6 个）

| 滑条 | 范围 | 说明 |
|---|---|---|
| 太阳仰角 ° | [0, 90] | 0° = 日落（白天项归零 → 纯夜色），90° = 正午 |
| 浊度 turb | [2, 8] | 霾化 + Turb*Loss 强度衰减（见 LIGHT_INTENSITY.md 九） |
| 季节 R | [0.5, 2.0] | **只染白天项**；拉极端可验证「夜色不被季节染色」 |
| 太阳强度 | [0, 10] | 平行光**绝对**强度（滑条所见即所得，不走 PWL 相对曲线） |
| 环境光强度 | [0, 2] | ambient **绝对**强度 |
| 夜色强度 | [0, 4] | **只乘夜色两色**；拉到 0 = 无夜色的对照画面 |

> **色调 vs 亮度分工**：sun/ambient 的 **color** 仍由 sky 自动推导
> （`DirectionalColor()/AmbientColor()`，色调随仰角变）；只有 intensity 由滑条直接给。
> 这是刻意的——色调的物理性交给 sky，亮度的手感交给标定者。
>
> **为什么「夜色强度」不直接改 `sky.intensity`**：`intensity` 是「天光总开关」，
> 同时作用于日夜两层；拿它当夜色的独立旋钮会连带改白天亮度。故夜色强度实现为
> 「把 `kCityNight* × night_scale` 写进 sky 的两个 night 颜色」，`sky.intensity` 恒 1.0。

## 4. 验收路径（Danis 肉眼）

1. 启动 → 默认太阳仰角 20°，可见三方块与灰色地面。
2. 把「太阳仰角 °」滑到 **0** → 白天项精确归零，画面只剩夜色：
   天顶深（冷）、地平明显更亮（暖），即「经典城市夜色」。
3. 把「夜色强度」拉到 **0** → 对照「同一构图、无夜色」（应接近全黑 + 微弱地色）。
4. 把「季节 R」拉极端（如 0.5 或 2.0）→ 白天项染色，**夜色两色不变**（验证语义分离）。
5. 太阳仰角置于 45°，调「太阳强度」/「环境光强度」→ 标定方块受光手感。

## 5. 已知边界（本 PR 不做）

- 无月亮盘（`moon_dir / moon_phase`）、无月光散射、无星星。
- 无 headless 拍摄子命令（`--four_views` 等）——本查看器只为交互肉眼验收而建；
  如需自动出图，后续按 `viewer_capture.h` 同款补。
- 地面是纯色 quad，无距离雾层（与 LIGHT_INTENSITY.md 九·5 的已知局限一致）。
