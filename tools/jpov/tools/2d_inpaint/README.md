# 2D inpaint — 只给四边，补中间

JPOV 工具链里的 2D 纹理 inpaint：输入一张方形纹理，**四条边原样保留**，中间用样例纹理合成补上。

两个算法，都是在 `assets/flagstone.png`（860×573 的石板照片）上选出来的：

| 方法 | 一句话 | 560 边长耗时 | 适用 |
|---|---|---|---|
| `quilting`（默认） | Efros & Freeman 2001，网格铺块 + 最小误差接缝 | 约 10–15 s | 快，颗粒感真实，边界最干净 |
| `criminisi` | Criminisi 2004，置信度 × 等照度优先级逐块粘贴 | 约 20 s（320 边长） | 结构延拓更好，但会把边缘裂缝/青苔反复复制 |

> 术语对齐：这里的 **2D inpaint** = 给方形纹理的 4 条边、补中间像素。
> 不是点云/网格几何修复，也不是 3D 体积纹理（那叫 solid texture synthesis）。

早期 lab 里并排跑过 5 个方法（harmonic / criminisi / patchmatch / quilting / DIP），只留下这两个：

- **harmonic**（调和延拓）是「只有边」的唯一最小能量解，但 `detail 0.02`，就是一片糊；
- **patchmatch** 丢掉一半对比度（`detail 0.40`），失败原因记录在 lab 里；
- **DIP** 101 s 只买到结构、买不到颗粒（`detail 0.10`）；
- **quilting** 最快、观感最像真石头，边环接缝最好，代价是淡淡的 patch 网格；
- **criminisi** 结构延拓最好（`detail 0.99` vs 原图 1.12），代价是慢、且会复制边缘碎片。

## 快速开始

```bash
cd tools/jpov/tools/2d_inpaint

# 默认：flagstone，560×560，96 px 边环，quilting
python3 -m inpaint.run --output output/quilting.png

# 换成 criminisi
python3 -m inpaint.run --method criminisi --output output/criminisi.png

# 自己的图（会先中心裁成正方形，再缩放到 --canvas）
python3 -m inpaint.run --input path/to/texture.png --canvas 512 --border 96 \
    --output output/mine.png
```

输出是 8-bit PNG，尺寸 `canvas × canvas`；同时打印三个验收指标和耗时。

依赖：`numpy`、`pillow`。`torch` 可选，只有 `quilting --backend torch` 用得到。

## CLI

| 参数 | 默认 | 含义 |
|---|---|---|
| `--input` | `assets/flagstone.png` | 源图；**中心裁成正方形**后缩放到 `--canvas` |
| `--method` | `quilting` | `quilting` / `criminisi` |
| `--canvas` | `560` | 输出边长，必须是 8 的倍数 |
| `--border` | `96` | 四边保留的宽度，必须 `< canvas/2`；中间洞 = `canvas-2*border` |
| `--hint` | `rot180` | 结构先验：源图旋转 180° 再 1/8 降采样；`none` = 用边环均色（无结构） |
| `--backend` | `numpy` | `quilting` 的匹配打分后端：`numpy`（float64，参考实现，确定性）/ `torch`（float32，CUDA 加速）/ `auto`（有 CUDA 才用 torch） |
| `--output` | `output/<method>.png` | 输出 PNG |

`criminisi` 不接受 `--backend torch`：它是贪心算法，一次 1e-6 的优先级差异会级联改变粘贴顺序，
CPU/GPU 输出可以差 20–28%，跨设备像素金标没有意义。它只有一份确定性的 numpy 实现。

## 三个验收指标怎么读

都以**纹理自身的统计**为参照。flagstone 的原始像素（未补）得分是
`seam 1.02 / detail 1.12 / colour 12.1` —— 这就是"正确"的参照，不是 `1.0 / 1.0 / 0`。

- **seam**：补丁与边环交界处的色阶 / 边环内部的色阶。`<= 1` 表示接缝不比正常纹理起伏更明显。
  注意它**结构性偏爱平滑填充**（调和延拓永远赢这个数），所以只看"有没有可见的切边"，不是质量分。
- **detail**：洞内梯度能量 / 边环梯度能量。远小于 1 表示补成了一片糊。
- **colour**：洞内与边环的平均 RGB 偏差（8-bit 级）。接近 0 反而是**被洗向边环颜色**的信号，
  不是"颜色准"。

flagstone 上两个预设的参考值：

| 方法 | seam | detail | colour |
|---|---|---|---|
| quilting（560/96） | 1.31 | 0.85 | 14.4 |
| criminisi（320/56，gold 用例） | 1.11 | 1.14 | 9.5 |

criminisi 在 560/96 上 lab 实测 1.25 / 0.99 / 13.1；gold 用 320/56 是为了把 CPU 耗时控制在 ~20 s。

**预设是在 560/96 上选的。** `quilting` 对边环厚度敏感：同一张图降到 320/56 时 seam 会到 2.35。
换尺寸时请让 `border` 明显大于 patch（建议 ≥ 64），并且看指标而不是只看默认值。

## CPU / GPU 的分工（为什么 CI 不需要显卡）

整条流水线只有**一个原语**碰 GPU：`inpaint/backends.py::score_map`（带权 SSD 的互相关）。

- `numpy`：float64 + FFT，无 GPU，跨机器逐字节稳定 —— **参考实现，gold 用它录制**。
- `torch`：float32 + `conv2d`，有 CUDA 时跑 GPU —— 只是加速器。

本机（RTX 5060 Ti）实测 quilting、560 边长同一预设：**numpy 12.6 s / CUDA 2.6 s，输出逐字节相同**。
320 边长时反而 numpy 更快（0.7 s vs 1.6 s），因为每块 patch 一次 kernel launch，块数少时启动开销占主导 ——
所以 CLI 默认 `numpy`，批量出大图时再显式 `--backend torch`。

两个后端不承诺逐位相同，只要求 **argmin 相同**（算法只消费这个决策）。
`test_inpaint_units.py` 在真实数据上量残差并断言 argmin 一致；CI 没装 torch 时这条测试自动 skip。
唯一的分歧场景是**精确平局**（例如程序化砖墙大量窗口同分），1e-5 的浮点差会翻 argmin，
那是"两个候选一样好"，不能升级成像素契约。

`criminisi` 的 SSD 也走同一个 numpy `score_map`，没有 torch 依赖，所以整条流水线在纯 CPU runner 上能跑完。

## 测试与 gold

```bash
cd tools/jpov/tools/2d_inpaint

# 全部 CPU 测试：单元 + 后端等价（无 torch 自动 skip）+ flagstone gold
python3 -m unittest test_inpaint_units -v
python3 -m unittest test_inpaint_gold -v

# 从仓库根一次跑完（CI 用法）
python3 -m unittest discover -s tools/jpov/tools/2d_inpaint -p 'test_*.py' -v
```

gold 是**像素 + 指标**双层：`gold/*.png` 是接受过的输出（逐像素，容差 max 2 / mean 0.05 /
0.1% 像素），`gold/gold.json` 记录参数和三个指标（容差 0.05 / 0.05 / 0.6）。
参数一改，gold 检查会直接报 "params changed"，不会静默通过。

故意改算法后重新录制：

```bash
python3 test_inpaint_gold.py --update
```

**先看图再提交。** 没被人眼检查过的 gold 只是"当前代码干了什么"的快照，不是 gold。

## 已知边界

- 只处理**方形四边已知**的设定：任意形状 mask、任意位置的洞、非正方形纹理都不支持。
- 中心裁切是固定的：`--input` 的左右或上下边缘会被裁掉，这是 2D inpaint 的定义决定的。
- `quilting` 的签名是**淡淡的网格感**（patch grid 不会消失）；`criminisi` 会把边环里的裂缝和青苔
  反复复制进中间。两个指标都看不见"构图是否合理"，要人眼看。
- 边环必须真的包含洞里需要的材质；样例方法只能抄已知像素，抄不到就只能编。
- `--hint rot180` 是任务原始定义（低分辨率结构草图）。hint 方向与边环天然不一致，
  会轻微把颜色拉离边界；`hint none` 可去掉。
- 这是算法收敛 + gold 定标的 Python 站。**下一步是移植进 JPOV C++/Bazel**，移植版对着同一批
  `gold/*.png` 做跨语言一致性测试（C++ 在 CPU 上就能跑 gold，不需要显卡）。
