# 天光：大气质量（airmass）颜色模型

## 1. 问题

直射光的**颜色**原来是一条经验曲线：

```cpp
// 旧
kelvin = 2000 + (5600 - 2000) * clamp(elev / 0.3, 0, 1)   // sin(仰角)
color  = ColorTempToLinear(kelvin)
```

它把"低太阳变红"简化成"色温下降"，与物理无关：

- 对**浊度**没有任何耦合（雾霾天与晴天的夕阳被算成一模一样）；
- 在 7° 时只给出 ~3460 K（淡黄），而真实 atm≈8 倍光程下蓝光已所剩无几；
- 与 `beerLambertAttenuation`（日盘亮度）、`DniFactorFromElevDeg`（方向光强度）
  各写一套仰角曲线，互不相干。

## 2. 模型

```
AM   = 1 / max(sin(elev), 0.02)                 // 大气质量（天顶 = 1）
τ(λ) = τ_R(λ) + τ_A(λ) · turbidity               // 分子散射 + 气溶胶
T(λ) = exp( -τ(λ) · (AM - 1) )                   // 以 AM=1（天顶）为参考
color = ColorTempToLinear(5600) · normalize_lum(T)
```

| 常数 | R | G | B | 说明 |
|---|---|---|---|---|
| `τ_R` | 0.036 | 0.098 | 0.230 | 分子（Rayleigh）∝ λ⁻⁴，蓝衰减最狠 |
| `τ_A` | 0.021 | 0.025 | 0.029 | 气溶胶（Mie），每单位 turbidity |

两个刻意的选择：

1. **以 AM=1 为参考**（`AM-1`）⇒ 正午 T=(1,1,1)，**正午外观零回归**；
   只有低太阳才产生色偏。
2. **只取色度**（除以亮度）⇒ 亮度仍由既有 `beerLambertAttenuation` / DNI 曲线负责，
   本改动不动任何亮度标定。

实现两份、必须同步：`sky_renderer.h::skyAirMassColor()`（GLSL）与
`render_command.h::SkyAirMassColor()`（C++）。后者供 `SunDirectionalColor()`
（物体的方向光颜色）使用。

## 3. 影响范围（诚实说）

- **日月盘 + 光晕**：变暖、变橙（5°/浊度 6 时肉眼可辨）；
- **物体方向光颜色**：低太阳下更暖（阴影处仍由 ambient 决定，所以对比更强）；
- **天空渐变本身几乎不变**：天空的颜色来自 Preetham 模型，它自带瑞利/米氏散射，
  已经是"物理"的；本改动只修正被 Preetham 之外使用的那条经验色温曲线。
  实测整幅平均 R/B 变化 +0.04 … +0.12（15° 最明显）。

- **无新增参数**：复用既有的 `SkyCommand::turbidity`；τ 表是内部常数。
- **算力可忽略**：每像素 3 次 `exp`（天空 pass，非云的那种每像素上千次 hash）；
  实测帧率无可测变化。

## 4. 验收

`--capture-sun-altitude <dir>`：同机位、只改仰角（60/30/15/5°）× 浊度（2.5/6），
出 8 张对照图（含地面与方块，便于看方向光颜色）。

gold：`//tools/jpov/test/object3d/skydome:all` **4/4 通过**（正午 AM=1 零变化；
黄昏那张在容差内）。
