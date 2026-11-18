# JPOV 天光查看器（skylight viewer）— 设计笔记

> 需求（Danis，2026-09-20）：新建一个 `skylight_viewer` demo，仿 model viewer 构造，
> 用来**肉眼验收天光（`SkyCommand`）**，尤其是新加的**夜色双色**。
> 本文件记录设计骨架与验收路径；实现对照本笔记核对。

---

## 1. 需求逐条对照（2026-09-21 大改后）

| 需求 | 实现 | 位置 |
|---|---|---|
| 仿 model viewer 构造 | 同构三段：主程序只装配 / 场景在 header / 渲染核心在 App header | `demo/jpov_skylight_viewer.cc` / `skylight_scene.h` / `skylight_viewer_app.h` |
| 场景固定三方块（低反/高光/金属） | `MakeBox` 造 1×1×1 方块 ×3，材质见下 §2 | `skylight_scene.h` |
| 大地用灰色 | 40×40 灰 quad（rough=1, albedo 0.35） | `MakeGroundQuad()` / `GroundMaterial()` |
| 视角变换与 model viewer 相同 | 直接复用 `jpov_viewer::ViewConfig` + `ApplyInput` | `view_config.h` |
| 每方块顶面站一个 T-pose 蓝人 | `mixamo_male` 蒙皮网格 + 单 identity pose（T-pose）；三人同一 mesh/骨架，**一次 instanced draw** | `InstallPerson()` / `BoxTopCenter()` |
| **只暴露五个天光自由度** | 浊度 / 日光季节色温 / 天体方向 / 月色变红 / 夜空偏蓝（**左列** 6 滑条 + 1 个三色 ambient 开关；月亮恒取 `-sun_dir`） | `skylight_viewer_app.h` `DrawLightPanel` |
| 其余参数走默认 | `CreateDefaultSkyCommand` → 其余字段走 `SkyCommand` 默认构造 | `render_command.h` |
| 主平行光由 sky 推导 | 白天 `sun=SunDirectionalColor/Intensity`；夜间 `moon=MoonDirectionalColor/Intensity`（按 `sun_dir.y` 切换） | `skylight_scene.h` `MakeSkyLighting` |
| ambient 由 sky 推导 | `ambient=AmbientColor/Intensity`（含夜间项）；开关打开时改用 `AmbientTricolor()`（[天,天际线,地] 三色梯度，均值与单色对齐） | 同上 |
| 月亮落在反日点 | 查看器固定 `moon_dir = −sun_dir`，故不给月亮独立滑条 | `MakeSkyLighting` |
| 血月 / 蓝夜旋钮 | `moon_season`（月盘+月光）与夜色两色整体乘子，逐条编码在 `MoonSeasonFromSlider` / `NightTintFromSlider` | `skylight_scene.h` |
| 标准天光（含夜色） | 默认构造即含城市夜色 + 月盘，白天/日落后由 daylight 自动过渡 | `render_command.h` 字段默认值 |
| **环境镜面（IBL）开关 + 天光消费量**（2026-10-26 新增） | 左列第 10/11 行：开关 + `[0,1]` 滑条 → `cmds.env_specular`；验收工件 = 第三方块（金属）。说明见 `docs/jpov_env_specular_design.md` | `skylight_viewer_app.h` `DrawLightPanel` |

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

## 3. 交互面板（两列）

左列 = 天光五个自由度 + 三色 ambient 开关；远景雾的旋钮已从面板移除（见 §3.1）。

| 滑条（左列） | 范围 | 自由度 | 说明 |
|---|---|---|---|
| 浊度 turb | [0, 8]（默认 2） | ① | 霾化（天色发白）+ 日/月盘衰减 + **月晕变宽** + 日/月主光衰减 |
| 季节色温 日光 | [−1, +1] | ② | 左=蓝偏 / 右=红偏 / 中=中性；只偏色温，不改亮度；**只染太阳能通道** |
| 天体仰角 ° | [−90, +90] | ③ | 正=太阳当空（白天，太阳主光）；负=月亮升到反向等高（夜间，月亮主光）；0°=两者都在地平线 |
| 天体方位角 ° | [0, 360) | ③ | 绕 +Y；同时定太阳与月亮（月亮在反方向） |
| 月色变红 | [0, 1] | ④ | 0=常月，1=血月（月盘 + 月光一起变红变暗）；→ `moon_season` |
| 夜空偏蓝 | [0, 1] | ⑤ | 0=出厂夜色，1=梦幻蓝且更亮；夜色两色整体乘子 |
| 三色环境光（开关） | 开/关 | — | 开=ambient 按法线仰角在 [天,天际线,地] 间插值（`AmbientTricolor`）；关=原单色（`AmbientColor`）。供对比 |

### 3.1 右列：远景雾（空气透视）—— 面板已移除，只留代码旋钮

远景雾现在是**一块真实体积雾**（froxel 全局介质，见 `docs/jpov_distance_fog_design.md`），
不是屏幕空间后处理。面板控件已于 2026-11-13 整段删除（它会影响 SSR 对比判断），
参数留在 `SkyDegrees`（`skylight_scene.h`）里，由代码/拍摄分支设置：

| 字段 | 含义 |
|---|---|
| `dist_fog_on` | 开关（默认关） |
| `dist_fog_sigma` | σ_t（1/m，默认 0.008 ≈ 125 m 能见度） |
| `dist_fog_top` / `dist_fog_extent` | 内盒顶高 / 水平半尺寸（米），盒底压在 y=0 |
| `dist_fog_radius` | 外侧圆角壳厚度（软边） |
| `dist_fog_atten` | 壳区剖面（`kExponential` 默认 / `kLinear` / `kQuadratic`） |
| `dist_fog_gain` | 雾色增益（乘地平线天光色；天光色是亮度归一化的相对色，必须缩放） |

验收：`--capture-distance-fog <dir>`（7 张 A/B，判据见设计文档 §8）。

## 4. 验收路径（Danis 肉眼）

1. 启动 → 默认天体仰角 45°、方位 45°，可见三方块与灰色地面（天色/光照由 sky 推导）。
2. **昼夜 + 主光切换**：把「天体仰角 °」从 +90° 滑到 −90° → 白天项归零进入夜景，
   主平行光由太阳换成月亮，全程与天色同一条时间轴（无突变）。
3. **月光的强度/颜色**：在负仰角段滑动 → 月亮越高画面越亮（DNI 衰减 + 月亮 N·L 变好），
   方块受光偏暖白（4100K，与月盘同色）；仰角 → 0° 时两侧主光都趋零、只剩夜色 ambient。
4. **颜色旋钮只染自己那一块**：拉「季节色温 日光」→ 天色（白天）与日主光/白天 ambient 变，
   **夜色与月盘不变**；拉「月色变红」→ 月盘与月光一起转红变暗，**天色不变**；
   拉「夜空偏蓝」→ 夜晚天幕整体偏蓝且变亮（含夜间 ambient 色，两者同源），**白天不变**。
5. **季节**：拉「季节色温」左/右极端 → 白天天空与方块受光一起变冷/变暖，**亮度不变**。
6. **天气**：拉「浊度」0→8 → 天色发白、阴影变软；月晕变宽，日/月主光变弱（`TurbSunLoss`）。
7. **月亮位置**：负仰角段 → 月盘升到反向等高；仰角 → 0° 时整盘被压进地平线。

## 5. 已知边界（本 PR 不做）

- **无月相**（`moon_phase`）：月盘是整圆发光盘，不做盈亏/被照亮面几何。
- 无星星；不建模月光驱动的大气散射/月盘环境光（月盘只作为自发光天体 + 主平行光，见 §5.3）。
- 地面是纯色 quad，无距离雾层（与 LIGHT_INTENSITY.md 九·5 的已知局限一致）。

### 5.1 月盘（2026-09-21 引入）

月盘与日盘走 **sky_renderer.h 里同一套共用推导链**（`discDirRemap` /
`beerLambertAttenuation` / `discMaskAndAngle` / `discGlow`），只是数值与色温不同：

| 项 | 日盘 | 月盘 |
|---|---|---|
| 方向 | `sun_dir` | `moon_dir`（独立字段；查看器里恒取 `−sun_dir`，即满月落反日点） |
| 色温 | 随仰角 2000K→5600K | **常量 4100K**（月反射日光，不随仰角变红） |
| 色温乘子 | `daylight_season`（太阳能通道） | **`moon_season`**（独立一把；血月 ≈ (1.0, 0.2, 0.03)） |
| 压盘角 | `sun_set_start_angle/ratio` = 10°/1.4 | `moon_set_*` 同默认值（**各自一份，不共享字段**） |
| 光晕宽度 | 固定 2×盘半径 | **随浊度线性变宽**：turb 2→2×，turb 8→4× |
| 默认亮度 | 2e5（HDR） | **5.0**（感知标尺，与夜色同一套；月盘常亮） |

查看器里的验收要点（月盘）：

1. 月盘默认亮度 5（常亮）→ 在天空中出现；「天体方位角」控它在天球上的位置（月亮在太阳反方向）。
2. 「天体仰角 °」滑到 **0** → 月盘被压盘角重映射**整盘淹没**在地平线下
   （与月亮真仰角 −20° 的帧逐字节相同，即不露半拉盘）。
3. 「浊度」2→8 → **月晕明显变宽**（晕宽倍数 2×→4×，盘心位置不动）；同时月盘
   **变暗**（Beer-Lambert：霾重月光衰减大）。
4. 「月色变红」0→1 → 月盘从暖白 → 铜红且变暗（`moon_season`）；同时它投下的
   **月光也一起变红变暗**（盘与光同源）。“季节色温 日光”则**不**动月盘。

月盘与日盘互不影响：同一帧同时给两者亮度即两盘同现（接口不禁止）；查看器里
因为日月分居天球两侧、且日盘只在仰角高时可见，实际很少同屏。

### 5.2 ✅ 物体光照现在**随天色联动**（2026-09-21 已解除旧边界）

旧版本（绝对量强度滑条年代）的「物体光照不随夜色变化」已**不存在**：现在日光/ambient
一律由 sky **推导**（`SunDirectionalColor/Intensity` + `AmbientColor/Intensity`），
而这三个推导都吃与 shader **同一个 `daylight` 因子**，所以天色 / 环境光色 /
环境光强 / 物体受光**是同一条时间轴**——把太阳仰角滑到 0°，物体也跟着进夜（变暗、
转冷调），不会出现「天已黑、物体还在夕阳下」的解耦。

> 历史遗留的 `night_scale`（夜色强度滑条）也已取消：夜色不再是可单独乘的旋钮，
> 而是标准天光默认构造的一部分（夜里自然淡入）。

### 5.2.1 季节/月色/夜色的染色边界（本次重定义）

`season` 重定义为 **`daylight_season`**，三把旋钮各管一块，互不越界：

| 旋钮 | 染谁 | 不染谁 |
|---|---|---|
| `daylight_season` | 日盘、太阳直射光、白天散射天光、白天 ambient（暮色端） | 夜色（含夜间 ambient）、月盘/月光 |
| `moon_season` | 月盘、月亮直射光（两者必须同色） | 其余全部 |
| 夜色两色 | 夜空颜色（用户给定）＋由它推导的夜间 ambient 色 | 不吃任何 season |

> 之前 `season` 是“混合后统一乘”，会把夜间 ambient 也染上（与“夜色不染”矛盾）；
> 现在改成：暮色端先染 `daylight_season` 再与夜色端插值，夜色端不再受任何 season 影响
> （Unit test `DaylightSeasonTintsOnlySolarChannels` 钉住该边界）。

### 5.3 月光平行光（2026-09-22 接入）

夜间主平行光改用月亮，与太阳**对称**（推导同在 `SkyCommand`）：

- `MoonDirectionalColor()`：常量 **4100K**（与月盘同色；月亮反射日光，且月盘不做低仰角红化），
  再乘 **`moon_season`**（与月盘同一把；血月时盘与光一起红）。**不吃 `daylight_season`**。
- `MoonDirectionalIntensity()`：`基准 × DniFactor(月仰角) × TurbSunLoss(turb) × (1−daylight)`，
  **与太阳共用同一条 DNI 仰角曲线与浊度衰减**；基准默认 **0.22 = 太阳基准 2.2 的 1/10**，
  走**感知标尺**（与月盘亮度同一套）。
- 白天门控：`moon_dir` 与 `sun_dir` 独立，乘 `(1−daylight)` 让月光白天为 0。
- 渲染侧只有**一个**平行光槽（`RenderCommandList::sun`），查看器按 `sun_dir.y` 二选一填入；
  故日月不会同时当主光。
- **未建模月相**：月盘是整圆，真实满月/弦月地面照度差 ~10×。
