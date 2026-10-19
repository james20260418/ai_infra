# 火焰 puff 正式纹理（V4 / V3.4 / V7.1 / V6.1 / V8）

本目录（`tools/jpov/assets/fire/mp/`）里的 5 张 **12×6 atlas** 是本次形变试验最终保留的
正式 puff 纹理。全部由 **Danis 手绘**素材生成，不含 Astralax / Seedance 等第三方贴图。

> 本 PR **只迁移纹理与说明**，不修改 particle viewer / 粒子系统代码。

## 文件

| 版本 | 文件 | 来源 | 处理要点 |
|---|---|---|---|
| **V4** | `flames_pairs_v4_12x6.png` | `danis5.png`（手绘 5 连图） | 原始笔触 + **M5 柔化**（DoG 亮核 → 向上拖尾 → 宽软底） |
| **V3.4** | `flames_pairs_v34_12x6.png` | F1..F4 = `danis_F5_v2.png`；F5 = `danis_F5_v3.png` 候选 #2 | 灰底抠图；**S1..S5 做 ±15° 旋转平均** |
| **V7.1** | `flames_pairs_v71_12x6.png` | `danis5.png` + F5 = `danis_S5.png` 候选 #4 | 源图 ±15° 旋转平均 |
| **V6.1** | `flames_pairs_v61_12x6.png` | `danis5.png` + S5 = `danis_S5.png` 候选 #4 | 源图 ±15° 旋转平均 |
| **V8** | `flames_pairs_v8_12x6.png` | `danis_F5_v4.png`（可爱风 5 连图） | 灰底抠图；**不加旋转平滑** |

所有图：**3072×1536**，12 列 × 6 行，单格 256×256，RGBA（RGB=白，A=形状掩膜）。

## 通用生成链路

1. **拆素材**：纵向/横向 5 连图按连通域拆成 5 张 256²（每格只保留自己的连通域，避免带进邻居）；
2. **可选柔化 / 旋转平均**：
   - M5：`DoG(L) → 向上带摆动拖尾(26 步, 0.88) → 与宽软底 max 合成`；
   - 旋转平均：`avg_limited(±15°, σ=8°)`（对 F 或对 S，按版本不同）；
3. **中间帧 S**：`S1 = PROFILE(F1,F2,0.5) … S5 = PROFILE(F5,F1,0.5)`；
4. **6 条两段链**（每条 12 帧 = 2 段 × 6 帧，运行时一个寿命走完整块）：

   ```
   S1→S2→S5   S2→S3→S1   S3→S4→S2
   S4→S5→S3   S5→S1→S4   S1→S3→S5
   ```

5. **图集布局**：块按行排列（第 r 条链 = 第 r 行），每块 12 帧；
   写 PNG 时行序翻转：`row_img = tilesY - 1 - row_frame`
   （JPOV `CellUV` 用 `vrow = tilesY-1-row`，GL 上传不翻转 ⇒ viewer 的 frame 0 采样 PNG 末行）。

## 各版本细节

### V4（手绘 + M5）
- 源图：`/james_pm/danis5.png`（5 连图）；
- M5 参数：亮核 `L - GaussianBlur(3)`；拖尾沿 -y 26 步、衰减 0.88、摆动 1.6px；
  宽软底 = `dilate(9px) + GaussianBlur(7.5)`；最终 `max(0.62*base, 1.05*trail)`；
- 覆盖率：alpha mean 0.173 / cov(>0.1) 0.339（偏厚，火焰里可适当减小粒子尺寸）。

### V3.4（F5_v3 抠图 + S 旋转平均）
- F1..F4：`danis_F5_v2.png` 的 #1..#4；
- F5：`danis_F5_v3.png` 的候选 #2；
- **灰底抠图**：边框 60px 拟合二次曲面 `bg(x,y)` → `res=(L-bg)/(1-bg)`
  → `clip((res-0.4)/0.5)` → 30px 边缘羽化（四边 alpha=0）；
- S1..S5 每个再做 `avg_limited(15°, 8°)`；
- 覆盖率：0.165 / 0.316。

### V7.1（F5=候选 #4 + F 旋转平均）
- F1..F4：`danis5.png`；F5：`danis_S5.png` 候选 #4（宽扁）；
- 对 F1..F5 全部做 `avg_limited(15°, 8°)`，再取 S 中点；
- 覆盖率：0.128 / 0.301。

### V6.1（S5=候选 #4 + F 旋转平均）
- F1..F5：`danis5.png`；S5 直接替换为 `danis_S5.png` 候选 #4；
- 对 F 做 `avg_limited(15°, 8°)`；
- 覆盖率：0.127 / 0.289。

### V8（可爱风，不加旋转平滑）
- 源图：`danis_F5_v4.png`（纵向 5 连图）；
- 灰底抠图同 V3.4；阈值 0.4 时正好分成 5 个形状；
- 不做旋转平滑（保留可爱风的软轮廓）；
- 覆盖率：0.196 / 0.341。

## 复现

生成脚本在备份 PR **#196**
（<https://github.com/james20260418/ai_infra/pull/196>，`genVolume/` 目录）：
`morph_util.py`、`rot_avg.py`、`danis_soften.py`、`make_pairs_v4.py`、
`make_pairs_v6_v7.py`、`make_pairs_v61_v71.py`、`make_pairs_v33_v34.py`、`make_pairs_v8.py`。

```bash
python3 genVolume/make_pairs_v4.py                         # V4
python3 genVolume/make_pairs_v6_v7.py --cand 4             # V6 / V7
python3 genVolume/make_pairs_v61_v71.py --cand 4 --half 15 # V6.1 / V7.1
python3 genVolume/make_pairs_v33_v34.py                    # V3.3 / V3.4
python3 genVolume/make_pairs_v8.py                         # V8 / V8s
```

## 版权

- 5 张正式纹理全部来自 **Danis 手绘**素材（`danis5.png`、`danis_F5_v2.png`、
  `danis_F5_v3.png`、`danis_F5_v4.png`），版权归 Danis。
- 不包含 Astralax《In Flames》原图及其派生图，也不包含 Seedance 视频转出的 FS1。
