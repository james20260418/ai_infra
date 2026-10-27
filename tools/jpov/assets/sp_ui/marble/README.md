# sp_ui 板材材质 — 白色大理石水纹 flipbook

> 位置：`tools/jpov/assets/sp_ui/marble/`
> 消费者：`sp_ui`（`SpTheme::plate_texture_id`）+ `jpov_sp_ui_demo`
> 用途：UI 板材（按钮/输入框/下拉/滑条/卡片底）的**基础材质**，一块材质覆盖全部控件，
> 颜色差异靠 `fill_color`（tint）撑。

---

## 产物

| 文件 | 内容 |
|------|------|
| `marble_key_0N.jpg` | 生图产出的 5 张原始关键帧（1024×1024 RGB，未处理） |
| `marble_key_0N.png` | 上者的 PNG 版（`jpov_texture_seamless` 只吃 PNG 输出链） |
| `marble_sync_0N.png` | 周期性（无缝平铺）化后的关键帧 |
| `plate_marble_flipbook_4x4.png` | **最终材质**：4×4 网格、每格 256px、共 16 帧、1024×1024 |

`plate_marble_flipbook_4x4.png` 就是引擎读的那张，帧序与 `CellUV()` 约定一致：
**行主序、帧 0 在图片左上角**（`vrow = tiles_y-1-row` 翻转后的结果）。

---

## 复现步骤

全部在 `tools/jpov/assets/sp_ui/marble/` 下、用相对路径（容器内外都能直接粘）。

```bash
cd tools/jpov/assets/sp_ui/marble

# 1) 生成 5 张关键帧（白色大理石 + 水纹，1 平米、俯视、无缝意图、无文字）
#    用 openclaw 的 image generate；产物是 jpg（provider 忽略 png 请求）
#    —— 已经产出并入库，除非要换风格，否则不需要重跑。

# 2) jpg → png
python3 - <<'PY'
from PIL import Image
for i in range(5):
    Image.open(f"marble_key_0{i}.jpg").convert("RGB").save(f"marble_key_0{i}.png")
PY

# 3) 周期性（无缝）化：四边与对侧混合。第 3 张（02）纹路最重，混合带要给满 256px，
#    其余 128px 就够（用 128 时 02 在 2×2 平铺下能看出一圈接缝）。
bazel build //tools/jpov/tools/texture_tool:jpov_texture_seamless
SEAM=../../../../../bazel-bin/tools/jpov/tools/texture_tool/jpov_texture_seamless
$SEAM marble_key_00.png marble_sync_00.png 128 blend
$SEAM marble_key_01.png marble_sync_01.png 128 blend
$SEAM marble_key_02.png marble_sync_02.png 256 blend
$SEAM marble_key_03.png marble_sync_03.png 128 blend
$SEAM marble_key_04.png marble_sync_04.png 128 blend

# 4) 5 张关键帧 → 16 帧周期 flipbook
python3 make_marble_flipbook.py \
  --out plate_marble_flipbook_4x4.png \
  --frames 16 --tiles 4 --cell 256 \
  --contrast gain:8 --gain-target 0.90 \
  "marble_sync_0[0-4].png"
```

---

## 算法（`make_marble_flipbook.py`）

### 1. 关键帧内插：周期 Catmull-Rom + profile 缓动

N 张关键帧当作**环上的 N 个控制点**（首尾相连 ⇒ 帧序列周期，末帧回到首帧连续），
逐像素做 Catmull-Rom；段内相位 t 先过一条 profile 曲线（`--contrast` 之外还有
`--ease linear|smoothstep|smootherstep`）再喂给插值。

- 与火焰链（`genVolume/make_flipbook.py`、`Gradient`/`Curve` 那套）同构：
  profile 只决定「两个关键姿态之间怎么走」，不改变插值本身。
- 为什么不用线性：线性只过控制点、一阶不连续，慢速播放能看出「一顿一顿」；
  Catmull-Rom 过点且切线连续，观感是连续流动的石纹。

### 2. 周期性：先 seamless 再内插（顺序不能反）

Catmull-Rom 是**逐像素仿射**组合，而仿射组合保持可平铺性 ⇒
**只要每张关键帧都无缝，输出的每一帧也无缝**。

反过来（先内插再 seamless）会让每一帧被独立地揉一遍边缘，
帧与帧之间的边缘处理不一致 ⇒ 平铺接缝会随帧号跳。

### 3. 白平衡 + 对比度（两步都必须「全帧共用同一组参数」）

- **白平衡**：把 5 张的通道均值归到同一目标。生图有一张偏暖（米白），
  不归一的话内插经过它时整条序列泛黄（实测帧均值蓝通道从 0.97 掉到 0.90）。
- **对比度 `gain`**：`out = target + (v − median) × gain`。
  - 源图 50% 的像素挤在 ±0.005 的白平台里（实测 p25..p75 = 0.969..0.973），
    所以**不能用 levels 拉伸** —— `[lo,hi]→[0,1]` 会把平台以上的细节（水纹本身）
    整段剪掉，只剩几条深色石纹。围绕中位数放大才能保留全部结构。
  - `--gain-target 0.90` 让白平台落在 0.9，于是「tint 就是板材亮度」这个直觉成立
    （tint=0.42 的板子 ≈ 0.38），主题颜色不需要按材质平均亮度反着标。
  - 逐帧自适应亮度是错的：那在 flipbook 里就是闪电式闪烁。

---

## 量化结果（当前入库这张）

| 指标 | 值 |
|------|-----|
| 源图每格亮度分位（处理前） | p5=0.945 p50=0.973 p95=0.976，std **0.0147** |
| 最终每格分位 | p5=0.745 p50=0.933 p95=0.973，std **0.104** |
| 帧间平均差（含 15→0 回绕） | 2.1 ~ 25.8 / 255（回绕 4.5，落在同一区间 ⇒ 循环连续） |
| 每帧平均亮度范围 | 0.407 ~ 0.487（全帧共用一组参数，无逐帧跳变） |
| 引擎侧可见性（1280×720 调试台） | 材质 on/off 有 1.5% 像素差 >8/255，最大 130；翻书 t=0 vs t=0.8 有 0.9% 像素差，最大 116 |

**已知限制（诚实记下）**：生图产出的「白大理石」本身是**平坦白板 + 细石纹**
（95% 像素在 2/255 以内），放大对比度后仍然是「少量细纹」，做不到大面积水波。
想要更明显的水纹，得换更脏/更对比的源图，或者改成「程序化水纹叠在石纹上」，
不是调参能解决的。

---

## 水波 flipbook（`make_wave_flipbook.py`）

> 2026-10-27 Danis 反馈：关键帧交叉淡化的 flipbook「动起来了但是确实很丑」——
> 那确实不是水在流，而是两个不同花纹在溶解。所以换成
> **单张周期纹理 + 位移场扭曲**：同一块石头往复位移，特征在屏幕上滑与起伏。

三种生成方式（`--method`，demo 面板上用「水波」下拉切换，也可 `--wave` 指定）：

| method | 位移场 | 观感 |
|--------|--------|------|
| `sine`   | 4 个方向的正弦叠加（整数波数） | 规则水波，最柔和 |
| `ripple` | 4 个中心的同心涟漪（环面距离） | 像水滴落在石面上，中心处最活跃 |
| `noise`  | 两张静态周期值噪声按相位绕圈混合 | 无序流动，最像自然水面 |

```bash
python3 make_wave_flipbook.py --src marble_sync_01.png --method sine \
  --out plate_wave_sine_4x4.png --amp 12 --wavelength 110
python3 make_wave_flipbook.py --src marble_sync_01.png --method ripple \
  --out plate_wave_ripple_4x4.png --amp 10 --wavelength 70
python3 make_wave_flipbook.py --src marble_sync_01.png --method noise \
  --out plate_wave_noise_4x4.png --amp 14 --wavelength 160
```

### 三个必须守住的约束

1. **位移场必须空间周期**（否则平铺接缝会裂）：正弦用整数波数；涟漪用
   **环面距离** `min(|Δ|, P−|Δ|)`；噪声用环绕整数格点。
   只要 `d(x+P) = d(x)`，则 `warped(x+P) = tex(x+P+d) = tex(x+d)` ⇒ 变形后依然可平铺。
2. **相位必须走整数圈**（否则播放会跳）：`phase = f/frames`，
   第 `frames` 帧的位移场逐像素等于第 0 帧。
3. **采样一律环绕双线性**（`sample_wrap`），否则边缘拉丝/黑边。

### 量化（当前入库三张）

| method | 帧间平均差 | 回环（末→首） | 引擎侧 t0 vs t0.8（1920×1080） |
|--------|-----------|--------------|------------------------------|
| sine   | 12.97 ~ 15.01 | 14.43 | 3.66 均值 / **10.6%** 像素 >8，最大 189 |
| ripple | 10.57 ~ 11.82 | 11.14 | 3.20 / 8.5%，最大 153 |
| noise  | 8.98 ~ 12.14  | 11.80 | 2.76 / 6.9%，最大 131 |

回环差都落在区间中段（不是离群值）⇒ 循环连续；三种方式同一时刻互不相同
（sine vs ripple / noise 的 t=0 平均差都是 3.81）。

对比旧做法（关键帧交叉淡化）：帧间差 2.1~25.8 **不均匀**，观感是两种花纹互相淡入淡出。

---

## 引擎侧采样约定（消费者必须遵守）

1. **纹理注册**：`repeat = true`（要平铺）、`mipmap = false`
   —— UI 是 1:1 像素，而 `fract` 平铺在接缝处导数不连续，自动选 LOD 会挑到最粗
   mip，在每条接缝上糊一条线（`tools/jpov/known_issue.md` §8.2）。
2. **帧号由时间算、时间由调用方给**（秒，零点自定）：`SpUi::Begin(..., time_seconds)`。
   引擎不取自己的时钟 ⇒ gold 可复现。
3. **材质尺度**：`SpTheme::plate_material_px` 表示「一格图集对应多少屏幕像素」，
   逐块按尺寸反算 tiling ⇒ 整屏纹理密度一致。字面 `plate_tiling` 会把同一格拉满
   一块大面板（纹路被放大到糊）。
4. **颜色**：`fill_color` 是乘到材质上的 tint；`border_color` 同样是 tint
   （边框带也采样同一张材质，不是一圈纯色塑料）。材质本身中性偏白，
   所以「多种颜色」只需要改 tint，不需要多套材质。
5. **UV 锚定屏幕原点**：`SpTheme::plate_material_px > 0` 时，sp_ui 逐块算
   `tiling = size / material_px` 且 `scroll = −pos / material_px`
   （因为 `uv01 × tiling = (screen − pos)/material_px`，减掉 `pos/material_px`
   就是 `screen/material_px`）。效果：整屏是一整块连续石纹 —— 背景/卡片/控件之间
   纹路接得上，同尺寸控件不会长得一模一样。不锚定的话每块板材都从自己的左上角
   重新铺，3 个电机卡片会是三张相同的石头。
