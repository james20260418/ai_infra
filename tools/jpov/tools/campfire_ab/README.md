# campfire_ab — 篝火 A/B 基准台

> P0 交付。完整计划见 `tools/jpov/docs/jpov_campfire_repro_plan.md`。

## 这是什么

把「EmberGen 官方渲染（标准答案）」与「JPOV 体渲染」做**同口径量化 + 并排对照**，
用来逐条验收「补齐机制」的进展（P1..P6）。方法学同 Tripo 渲染还原：
**先钉标准答案 → 固定相机/帧 → 逐机制 A/B → 一律像素量化**。

## 标准答案（CC0）

EmberGen 免费 VDB 套件自带的 `smallCampfire.mp4`（200 帧，黑底孤立火 + 烟柱）——
与我们手上这份 CC0 VDB 是同一份资产，故为「同资产的官方标准渲染」。

- 下载页：<https://jangafx.com/software/embergen/download/free-vdb-animations>
- **帧对应**：我们入库的 `campfire.jpovvol` 由源帧 60..139 转得 ⇒
  **资产帧 `k` ↔ 官方帧 `60 + k`**（见计划文档 §1.1）。

`reference/` 里是缩略后的官方参考帧（`--width 576`），供肉眼对照与仓库体积控制。

## 文件

| 文件 | 作用 |
|---|---|
| `fetch_reference.py` | 从官方 mp4 抽参考帧入库（CC0） |
| `ab_metrics.py` | 两张图同口径指标 + 并排图（h/w、面积、均值 RGB、近白占比、竖向亮度剖面） |
| `ab_seq.py` | **逐帧 A/B**：整段序列每帧抓图 → 与官方对应帧比对 → diff 表（CSV）+ 竖排拼图 |
| `run_ab.sh` | 端到端：抓 JPOV 孤立火 → 比对（需已编译 viewer + `xvfb-run`） |
| `reference/` | 官方参考帧（缩略） |
| `baseline/` | P0 时刻的差距快照（单帧 + 逐帧，供 review） |

## 用法

```bash
# 1) 取官方 mp4（CC0）后抽帧（一次性）
python3 tools/jpov/tools/campfire_ab/fetch_reference.py \
  --video /path/to/smallCampfire.mp4 --outdir tools/jpov/tools/campfire_ab/reference \
  --frames 60 80 100 120 140

# 2) 抓 JPOV 孤立火并与官方帧比对（asset 帧 20 ↔ 官方帧 80）
bash tools/jpov/build_volume_fire_viewer.sh
bash tools/jpov/tools/campfire_ab/run_ab.sh \
  /james_pm/fire_hero/assets/prototypes/campfire.jpovvol 20
```

### 逐帧 A/B（diff 曲线）

对整个序列每帧抓图 → 与官方对应帧同口径比对，产出 **diff 表（CSV）+ 竖排拼图**：

```bash
python3 tools/jpov/tools/campfire_ab/ab_seq.py \
  --volume /james_pm/fire_hero/assets/prototypes/campfire.jpovvol \
  --viewer ./output/jpov_volume_fire_viewer/jpov_volume_fire_viewer \
  --refdir tools/jpov/tools/campfire_ab/reference \
  --frames 0:80:10 --outdir /tmp/ab_seq --box 1.22
```

**为什么它是「范式」而不只是个脚本**：

1. **纯数值、无需 vision 模型** → 确定性、可无人值守，直接当 **P1..P6 的自动验收门禁**
   （看各指标是否随机制补齐朝参考单调收敛）。
2. **只有逐帧才看得见的问题**：闪烁、循环接缝、烟随时间的演化 —— 单帧 A/B 查不出。
3. **顺带验证帧对应**：`资产帧 k ↔ 官方帧 60+k` 是假设；映射错了曲线会跳/乱，
   这条工具本身就是验证器。

口径提醒：`h/w` 与**颜色（蓝通道 = 烟）**是**取景无关**的硬指标；`area%` 受取景影响，
只能同侧自比，不能跨侧直接比。

## viewer 的 A/B 模式（本 PR 新增）

`jpov_volume_fire_viewer --ab-capture <out.png> [--volume <jpovvol>] [--ab-frame N] [--ab-box M]`

- **钉相机**：正面（phi=0/theta=0，+Z 看向原点），距离由火盒自动取景；
- **钉帧**：asset 帧下标 = `--ab-frame`（可复现）；
- **纯黑背景 + 无地面/燃料块**：只留孤立火，与官方渲染同底；
- **火盒**：居中于世界原点的立方盒，边 = `--ab-box`（默认由 `run_ab.sh` 按
  sidecar 的 `max(dims)×voxel_m` 推出）。
