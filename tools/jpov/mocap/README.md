# jpov/mocap — 视频 → Mixamo23 骨骼动画（实验性工具链）

> 目标：给一段人像视频（或一组连环画 still），产出一段**符合 Mixamo 23 骨布局**的动作，
> 落成 FBX 后用 `jpov_fbx_viewer` 播放，肉眼/AI 看图验收。
>
> 定位：**实验工具**，不在引擎运行时链路上，不进 `Renderer`。

## 0. 全景（先读这一节）

### 0.1 六个环节

```
视频/连环画 ──①取帧──▶ 帧 ──②关键点──▶ track.json ──③方向场──▶ 每骨世界方向
          ──④拟合──▶ poses.json ──⑤写文件──▶ .fbx ──⑥回放/验收──▶ 图 / 视频 / 指标
```

| # | 环节 | 入口 | 输入 | 输出 | 核心方法 |
|---|---|---|---|---|---|
| ① | 取帧 | `ffmpeg`（`pose_extract.py` 内部调） | `*.mp4/webm`，或一组 still 图片目录 | 统一尺寸/帧率的 RGB 帧 | 按目标 fps 重采样（默认保持源帧率） |
| ② | 关键点 | `extract/pose_extract.py`（MediaPipe PoseLandmarker） | 帧序列 + `pose_landmarker_full.task` | `track.json`：33 点（像素 2D + world 米制 + 置信度/presence） | 视频用 **VIDEO** running mode（有跨帧 tracker）；连环画用 **IMAGE** 模式（逐张独立检测，否则 tracker 会把相邻 still 的手粘错人） |
| ③ | 方向场 | `algo/`（C++）`DirectionAlgorithm` 注册表 | `track.json` + 目标骨架（骨长先验） | 每帧 22 根骨的世界单位方向（缺帧用 `invalid` 表达，不跳号） | 四种算法：`worldz` / `worldz_t` / `lift2d` / `lift2d_t`（见 §3）。**算法只负责方向**，yaw 统一由 ④ 估，保证比对时“算法差异”不混入“朝向处理差异” |
| ④ | 拟合 pose | `jpov_mocap_fit`（`FitFromDirections`） | 方向场 + `track`（估 yaw / 髋位移）+ `--height` | `poses.json`：每帧 23 项 `quat`（相对 bind）+ `root` + `yaw_deg` + `foot_clearance_m` | swing-only 重定向（§4）+ 整体 yaw 落在 **Hips** + 水平根位移 + 最低脚钉 `y=0` + 根位移零相位平滑 |
| ⑤ | 写文件 | `fit/export_fbx_ascii.py` | `poses.json` | ASCII FBX 7.4（骨架 + 逐帧 `Lcl Rotation` 曲线） | **直接写文件，不经 Blender**：`Lcl Translation = rest_offset`、`Lcl Rotation = bind·pose`，bind 与官方 Mixamo 逐骨一致（§6 第 6 条） |
| ⑥ | 回放/验收 | `jpov_fbx_viewer` | `.fbx`（+ 可选目标资产 `.glb`） | 交互窗口 / 单帧 PNG / 逐帧序列 / 双角度 mp4 / 指标 | Viewer 自带 GPU 双帧插值蒙皮；`--view 9` 蓝人模特（不依赖 glb）、`--view 10` 真皮角色、`--view 5` 红骨人 + 真皮对照 |

> 两个入口的 `poses.json` **只有 `quat` 是同一语义**（每骨相对 bind 的局部旋转，可直接喂
> `export_fbx_ascii.py`），其余不同：C++ 是完整产线（整体 yaw + 水平根位移 + 最低脚钉 `y=0`，
> 额外多 `yaw_deg` / `foot_clearance_m` 两个诊断字段），Python 只有旋转 + 髖中心位移（无 yaw、无钉地）。
> 所以：**算法实验可以用 Python，交付 FBX 一律用 C++**。
> 另：两边的 `lift2d_t` 也不是逐位一致（平滑实现不同，主要差在单目不可观测的深度分量上；
> 指标与 2D 叠图一致，别拿两份 JSON 逐骨对比）。

### 0.2 这套东西的基本属性（决定它能干什么、不能干什么）

1. **骨架是固定的**：Mixamo23 = 1 个无名包装根 + **22 根具名骨**（`mixamorig:*`）。
   骨长取 Mixamo 官方比例 × `--height`；**不按被测者逐骨缩骨**，所以肢体比例差会体现在肘/膝落点（§3 的 reproj 58px 就是这个）。
2. **只解旋转 + 根平移**：每骨只给「相对 bind 的最小摆动」（swing-only，绕骨轴 twist 不可观测、**有意为零**）。
   **没有 IK、没有接触检测** ⇒ 走/跑会脚滑，跳跃会被钉地逻辑拉回地面（第一期明确边界）。
3. **单人**：两人贴身时 MediaPipe 的 33 点会混成一个人（不是我们的 bug）。
4. **单位/坐标**：内部一律 **米 + Y-up**；FBX 文件按 **厘米数值 + `UnitScaleFactor=1`** 写（与 Mixamo 一致，ufbx 读出 `unit_meters=0.01`）。
   loader 在**加载边界**统一换算成米 —— 这条是“单位铁律”，跨 loader 混用不会静默错 100 倍。
5. **深度不可全信**：z 来自 MediaPipe world 回归（相机系近似米制），遮挡/自交时符号会翻；`lift2d_t` 用中值压制。
6. **时间契约**：帧精确采样（`LocateClipFrame` 只定位不插值），插值留给渲染端（GPU 双帧），避免双重插值把运动压平。

### 0.3 校验方式（五层，从便宜到贵）

| 层 | 手段 | 看什么 | 命令/位置 |
|---|---|---|---|
| 1 | **数学不变量单测**（纯 CPU） | rest → identity、yaw 落在 Hips、髋位移 → root、最低脚 = 0、`lift2d` 尊重 world-z 符号 | `bazel test //tools/jpov/mocap/algo:mocap_fit_test` |
| 2 | **批量指标** | `dir_err_deg`（FK 骨方向 vs 画面 2D 方向）、`jitter_mm`（关节位置二阶差分 RMS）、`root_y_min/max`、`foot_penetration_mm` | `jpov_mocap_fit --algos all --out-dir …` → `mocap_report.{json,csv}` |
| 3 | **2D 叠图** | 骨节是否压在关节上、四肢朝向对不对（**算法对比的唯一裁判**） | `fit/render_check.py --track … --poses … --video … --frames 0,40,80` |
| 4 | **3D 回放** | 体态 / 朝向 / 重心 / 是否穿地 / 动作连贯 | `jpov_fbx_viewer … --view 9 \| 10 --video … --azimuth-deg 0 --azimuth2-deg 90` |
| 5 | **真皮验收** | 蒙皮是否被拧成麻花、破面、脚悬空（资产 bind 问题只有这一层看得见） | `--view 10`（或 `--view 5` 红骨人对照） |

> ⚠️ `reproj RMS`（重投影误差）**不是**算法好坏的判据：它被骨架比例差支配，四种算法都在 58px 上下。
> 方向误差（`dir_err`）才是；这条是本次实验早期踩过的坑（§3 注）。

### 0.4 交付给 JPOV 用户

见 **§8**：FBX 文件契约、放进哪个资产目录、BUILD `filegroup` 怎么写、用户侧用哪个 loader 读、交付前验收清单。

## 1. 一句话结论

```
视频 ──ffmpeg──▶ 帧 ──MediaPipe PoseLandmarker──▶ 33 点(2D + world 3D)
                     ──方向场（worldz/lift2d…）──▶ 每帧 22 骨世界方向
                     ──FitFromDirections──▶ 每帧 23 项 pose（quat + root + yaw）
                     ──export_fbx_ascii.py──▶ FBX（Mixamo23 骨架 + 关键帧）
                     ──jpov_fbx_viewer --video──▶ 图 / 双角度 mp4
```

（早期“Blender 中转 FBX”路线已弃用：往返会改 bind 朝向，见 §6 第 6 条。）

## 2. 目录

| 路径 | 作用 |
|---|---|
| `extract/pose_extract.py` | 视频或图片目录 → `track.json`（MediaPipe，33 点 + 置信度） |
| `fit/mixamo23.py` | Mixamo23 骨架的 Python 镜像（与 `interface/mixamo23_skeleton.h` 同表）+ FK |
| `fit/mocap_fit.py` | **算法实验的 Python 版**：关键点 → 每骨方向 → swing 拟合（`worldz` / `lift2d` / `lift2d_t` 三种；**不含整体朝向与脚钉地**，见 §0.1 注） |
| `fit/export_fbx_ascii.py` | **pose → FBX（ASCII 7.4），直接写文件**，不经 Blender（推荐） |
| `fit/export_fbx.py` / `export_fbx_template.py` | 早期 Blender 路线，**已弃用**（见 §6 第 6 条） |
| `fit/render_check.py` | 把 pose 叠回原视频帧（2D 比对图），算法调试用 |
| `fit/render_fbx_video.sh` | 旧的外部逐帧脚本（保留作对照；新流程用 viewer 的 `--video`） |
| `algo/` | **C++ 动捕算法核心**（GL-free）：方向场注册表 / fit（yaw + root + 钉地）/ 批量报告 CLI |
| `testdata/` | 测试素材（`archery/拉弓射箭.mp4`，121 帧 @24fps；早期拳击片段未入库） |

## 3. 四种方向场算法（本次实验的对比对象）

四种算法**共用**同一套 retarget 数学（见 §4）与同一个 fit/指标，差异只在「每骨世界方向场怎么来」：

| algo | 方向从哪来 | 优点 | 缺点 |
|---|---|---|---|
| `worldz` | 直接信 MediaPipe 的 world landmarks（网络回归的近似米制 3D） | 深度"看起来"连续、无参数 | z 噪声大，画面内方向也被带偏（实测 5.4° 平面内误差）；抖动 30mm |
| `worldz_t` | `worldz` + 零相位时间平滑 | 抖动降到 4.6mm（时序基线） | 平面内方向误差不变（5.4°），不是可用解，只是对照 |
| `lift2d` | **只用 2D 关键点 + 骨长先验反解深度**：投影长度 u 已知、骨长 L 已知 ⇒ 出平面分量 √(L²−u²)，符号取 world-z 差值 | 画面内方向**精确**（0.003°）、深度"量"是几何推的 | 2D 抖动原样进入（42mm），深度符号会跳 |
| `lift2d_t` | `lift2d` + **零相位** 2D 平滑 + 深度符号时序一致（中值）+ 深度零相位平滑 | 抖动 4.7mm（比 `lift2d` 小 9×），**无滞后**，默认算法 | 平面内方向被平滑轻微带动（0.65°） |

指标（同一段拉弓射箭 121 帧 @24fps，C++ `--algos all` 的 `mocap_report.csv`）：

| algo | dir_err（画面内） | jitter（关节位置） | root_y 区间 |
|---|---|---|---|
| `worldz` | 5.43° | 30.7 mm | [-0.36, -0.01] |
| `worldz_t` | 5.44° | **4.6 mm** | [-0.36, -0.02] |
| `lift2d` | **0.003°** | 42.1 mm | [-0.28, +0.01] |
| `lift2d_t` | 0.65° | **4.7 mm** | [-0.24, +0.03] |

- `dir_err` = FK 骨方向 vs 画面里同骨 2D 方向的夹角。**这才是算法该看的指标**。
- `reproj RMS`（早期 Python 实验里量过，四种算法都 58~59 px）**不能**当判据：它被骨架比例差支配
  （Mixamo23 是 1.60m 官方比例，被测者比例不同 ⇒ 肩/肘天生偏 50~60px）。这条是本次踩过的坑。
- `jitter` = 关节位置二阶差分均方；`lift2d_t` 比 `worldz` 低一个量级，且**零相位无滞后**
  （普通滑动平均会把动作推后 (win−1)/2 帧，24fps 下 7 帧窗 = 0.13s，足够让“拉满”和“起手”错位）。
- 上表是 **C++ 产线**的数（`mocap_report.csv`）。Python 版同量级但**不是逐位一致**（平滑实现不同，
  如 `lift2d_t` 的 jitter：Python 3.2mm / C++ 4.7mm）；对应交付以 C++ 为准。

## 4. retarget 数学（与 C++ 侧同一套）

每骨 target：`W(j) = Swing(Rb(j)·ŷ → d_j) · Rb(j)`，即**相对自己 rest 的最小摆动**，
roll 从 bind 继承（T-pose 时零误差）。然后

```
P(j) = B(j)⁻¹ · W(parent)⁻¹ · W(j)      // P = 相对 bind 的 local 旋转
```

与 `interface/skeleton_retarget.h` 的「偏差式」同源，只是这里的"源"是**关键点方向**而不是
另一套骨架。不变量复用同一个：`d_j == Rb(j)·ŷ ⇒ P = identity`（T-pose 不会被动过）。

## 5. 怎么跑

所有命令都从**仓库根**执行，路径全部相对。

### 5.1 依赖（一次）

```bash
# 提取器只需要 numpy/mediapipe；拟合与回放都是 C++，无需 Python。
# 版本统一由仓库根的 requirements.txt 钉死（mediapipe↔opencv↔numpy 互相钳制）。
python3 -m venv /tmp/mpvenv && /tmp/mpvenv/bin/pip install -r requirements.txt

# MediaPipe 模型（≈9.4MB，一次）
curl -L -o /tmp/pose_full.task \
  https://storage.googleapis.com/mediapipe-models/pose_landmarker/pose_landmarker_full/float16/latest/pose_landmarker_full.task
export POSE_MODEL=/tmp/pose_full.task   # 或每次 --model 指定
```

### 5.2 产线（C++，推荐）

```bash
# ① 关键点：视频 → track.json
/tmp/mpvenv/bin/python tools/jpov/mocap/extract/pose_extract.py \
    --video tools/jpov/mocap/testdata/archery/拉弓射箭.mp4 \
    --out output/archery_track.json --overlay-dir output/ov --overlay-every 12

# ② 拟合成 pose（单条）
./bazel-bin/tools/jpov/mocap/algo/jpov_mocap_fit \
    --track output/archery_track.json --algo lift2d_t \
    --out output/archery_lift2d_t.json --height 1.78

# ②' 或多算法批量比对（写 mocap_report.json / mocap_report.csv）
./bazel-bin/tools/jpov/mocap/algo/jpov_mocap_fit \
    --track output/archery_track.json --algos all \
    --out-dir output/mocap_compare --height 1.78

# ③ 写 FBX（直接 ASCII，不经 Blender）
python3 tools/jpov/mocap/fit/export_fbx_ascii.py \
    --poses output/archery_lift2d_t.json --out output/archery.fbx

# ④ 回放：真皮角色 + 双角度视频（蓝人模特版把 --view 10 换成 9，且不用给 glb）
output/jpov_fbx_viewer/jpov_fbx_viewer output/archery.fbx \
    tools/jpov/assets/models/characters/mixamo_male.glb --view 10 \
    --video output/archery_skin_2angles.mp4 --frames 121 --fps 24 \
    --azimuth-deg 0 --azimuth2-deg 90 --elev-deg 5
```

> `output/jpov_fbx_viewer/` 的二进制由 `./tools/jpov/build_jpov_fbx_viewer.sh` 产出；
> 也可以直接用 `./bazel-bin/tools/jpov/demo/fbx_viewer/jpov_fbx_viewer`（但要在含 `fonts/` 的
> 目录下跑，否则字体 CHECK 会 abort）。

### 5.3 Python 实验链（算法对照用）

> ⚠️ Python 版只有 `worldz` / `lift2d` / `lift2d_t` 三种算法，且**不含整体朝向与钉地**
> （`worldz_t`、yaw、root、脚钉地都只在 C++ 侧）——它的产物不要拿来当交付 FBX。

```bash
# 三种算法各跑一遍（输出字段与 C++ 同构到 quat/root/t 这一层）
for a in worldz lift2d lift2d_t; do
  python3 tools/jpov/mocap/fit/mocap_fit.py --track output/archery_track.json \
      --out output/pose_$a.json --algo $a --height 1.78
done

# 2D 叠图（算法对比的唯一裁判）：骨节压在关节上 / 四肢朝向对不对
python3 tools/jpov/mocap/fit/render_check.py --track output/archery_track.json \
    --poses output/pose_lift2d_t.json --video tools/jpov/mocap/testdata/archery/拉弓射箭.mp4 \
    --frames 0,40,80 --out output/overlay/
```

## 6. 已知坑（都是这次真踩到的）

1. **Blender FBX 的轴向**：导出器把「Z-up→Y-up」的 −90°X 挂在**骨架对象节点**上，
   而 ufbx/JPOV 取的是骨节点的 local translation ⇒ **只有根骨被转、子骨没转**，整个人躺下。
   ⇒ 解法：在 Blender 里先把骨架对象 `rotation_euler=(+90°X)` 并 **apply**，再用
   `axis_up="Z"` 导出（此时文件里的数字已直接是 Y-up）。
2. **单位**：Blender 默认写出"数值是米、却声明 cm"，ufbx 报 `unit_meters=0.01` ⇒ JPOV
   再 ×0.01，角色缩成 1cm。⇒ 解法：几何按 **cm** 写进 Blender（`--unit-scale 100`）。
3. **MediaPipe 双人**：两人贴身时 33 点会**混成一个人**（不是我们的 bug）。单人素材才可靠。
4. **world-z 不可信**：`lift2d*` 存在的理由；但**符号**仍要借它（单目深度歧义无法只靠 2D 解）。
5. **reproj RMS 不是好指标**：见 §3 注。
6. **🔴 Blender 的 FBX 往返会改 bind 朝向（未解决）**：把官方 Mixamo FBX `import` 再
   `export`，逐骨对比 JPOV 实际读到的 bind：
   `RightArm` 的 bind +Y 从 **(-1,0,0)** 变成 **(0.11,0.07,0.99)**（≈123°），
   `RightShoulder` 差 12°，`Hips` 平移从 (0,1.043,0) 变 (0.044,1.019,0.010)。
   **火柴人看不出来**（杆是轴对称的，绕骨轴转多少都一个样），但一上蒙皮（`glb` 蓝人）
   就拧成麻花 —— 这正是「骨架像麻花」的真正来源。已验证 `--rest`/identity pose 下火柴人
   是干净 T-pose、动画帧也连贯 ⇒ **关节位置/偏移是对的，坏的是每骨的 bind 朝向**。
   **已解决**：`export_fbx_ascii.py` 直接写 ASCII FBX 7.4，bind 与官方逐骨一致
   （实测 LeftArm bind +Y = (-0.205706, 0.977363, -0.0494649)，官方 (-0.205706,0.977363,-0.0494657)），
   蓝人 Morph 正常、动作连贯。写文件时踩到的三个格式坑：
   ① FBX 7.x 里「这是根骨」挂在 **NodeAttribute: "LimbNode"** 上，不是 Model 的第三个字段
      （ufbx 只认前者，否则 `bone == nullptr`、整份骨架读不出来）；
   ② `AnimationCurveNode` 到 Model 是 **OP + "Lcl Rotation"**，不能再额外连一条 OO（会解析失败）；
   ③ `AnimationCurve` 的 `KeyAttrFlags`/`KeyAttrDataFloat`/`KeyAttrRefCount` 不能省，
      `AnimationStack` 必须给 `LocalStart/LocalStop/ReferenceStart/ReferenceStop`，
      否则 ufbx 报 `frames=1`（认为时间轴是空的）。
   另外骨名**必须保留 `mixamorig:` 前缀** —— 去掉后 viewer 报「骨名命中 0/23」，
   蓝人直接塌成一条线（这个现场极具误导性，看起来像蒙皮坏了）。
7. **看图的机位**：`jpov_fbx_viewer` 默认方位角 45°，T-pose 会看成「一高一低」而被误判成
   骨架拧了。已加 `--azimuth-deg` / `--elev-deg`（本次新增），AI 看图验收请用
   `--azimuth-deg 0 --elev-deg 5`。

## 6.5 C++ 版（2026-11-04 起，生产路径）

Python 侧保留为**提取器 + 算法实验**；正式拟合与批量比对走 C++：

```bash
# 单条：方向场 → poses.json（字段与 Python 版兼容，可直接喂 export_fbx_ascii.py）
./bazel-bin/tools/jpov/mocap/algo/jpov_mocap_fit \
    --track ../danis/archery_track.json --algo lift2d_t \
    --out ../danis/archery_lift2d_t.json --height 1.78

# 批量：多算法 → JSON / CSV 报告
./bazel-bin/tools/jpov/mocap/algo/jpov_mocap_fit \
    --track ../danis/archery_track.json --algos all \
    --out-dir ../danis/mocap_compare --height 1.78

# FBX（现在会写 Hips 平移 = root motion）
python3 tools/jpov/mocap/fit/export_fbx_ascii.py \
    --poses ../danis/archery_lift2d_t.json --out ../danis/archery.fbx

# 蓝人模特 + 两个角度视频（不需要 glb）
output/jpov_fbx_viewer/jpov_fbx_viewer ../danis/archery.fbx --view 9 \
    --video ../danis/archery_2angles.mp4 --frames 121 --fps 24 \
    --azimuth-deg 0 --azimuth2-deg 90 --elev-deg 5
```

C++ 侧已补：整体朝向（yaw，落在 Hips）、根位移（水平 + 脚钉地）、
程序化蓝人模特（`interface/skeleton_mannequin.h`）、viewer 自带 `--video` / `--shot-dir`。

### 6.5.1 换成真蒙皮角色（`--view 10`）

程序化蓝人模特（`--view 9`）只是骨架比例的可视化代理；要**看资产真皮**（glb 的蒙皮网格）
时用 `--view 10`：镜子里只有 1 个真皮角色，没有红骨人。

```bash
output/jpov_fbx_viewer/jpov_fbx_viewer ../danis/archery.fbx \
    tools/jpov/assets/models/characters/mixamo_male.glb --view 10 \
    --video ../danis/archery_skin_2angles.mp4 --frames 121 --fps 24 \
    --azimuth-deg 0 --azimuth2-deg 90 --elev-deg 5
```

两条与资产相关的约定（都在 viewer 内部处理，命令行不用管）：

1. **朝向按 Q_body 转回源**：Tripo 出的 glb 人体系比 Mixamo 源多转 89°（它的“朝前”是 +X），
   不补这一拍角色会侧对镜头。`--view 10` 把整只角色转回 `−Q_body`，所以
   `--azimuth-deg 0` 仍是**正面**，与 `--view 9` 的镜头语义一致。
   并列对比模式（`--view 5` 等）**不转** —— 那里 Q_body 正是待观察的资产差异。
2. **机位按最终模式重算**：`--view` 在装配之后才定，捕获前会按该模式的包围盒重新取景
   （单角色模式只按角色自己算，不会因为“并列布局”而偏小偏侧）。

可选：挂已蒙皮的衣服（骨序须与 glb 一致），与角色逐帧同步：

```bash
# 目录里放 *.glb（例：yoga_pants_skinned_sample.glb / harness_vest_skinned_sample.glb）
output/jpov_fbx_viewer/jpov_fbx_viewer ../danis/archery.fbx \
    tools/jpov/assets/models/characters/mixamo_male.glb --view 10 \
    --cloth_path ../danis/mocap_cloth --shot ../danis/dressed_f30.png --frame 30
```

## 7. 未做 / 下一步

- **手部朝向**（palm normal）没有解，`LeftHand` 只按前臂方向摆。
- 深度符号在遮挡/自交时仍会翻（`lift2d_t` 靠中值压制）。
- 骨架比例固定为 Mixamo23；被测者高一截/矮一截会让肘膝位置偏（这正是 reproj 58px 的来源）。
- 只导 **旋转** + 根平移；脚做的是「最低脚钉 y=0」的整体 grounding，**不是 IK**：
  走跑会滑，跳跃会被拉回地面（第一期明确边界）。
- C++ 端口已完成（`algo/` + `jpov_mocap_fit` + gtest）；下一步是接 SMPL/GVHMR
  作为新的 `DirectionAlgorithm`，而不是再写一条链路。

## 8. 产出与分发：把动捕结果交给 JPOV 用户

### 8.1 交付物是什么

一次动捕最终给两类东西：

| 产物 | 给谁 | 说明 |
|---|---|---|
| `*.fbx`（ASCII 7.4） | 任何 DCC / 引擎 / JPOV 用户 | 通用格式，带 Mixamo23 骨架 + 逐帧旋转曲线（+ 根平移） |
| `*.json`（poses） | 想二次处理的人 | 数值中间态（每帧 quat + root + yaw），字段见 `docs/jpov_video_mocap_design.md` §2 与 `algo/mocap_io.cc` 的写出代码 |

**FBX 内部契约**（写文件时就固定，用户不用猜）：

- ASCII FBX **7.4**，`UpAxis=1`（Y-up），Y-up 与 JPOV 一致。
- 数值按**厘米**写、`UnitScaleFactor=1`（与官方 Mixamo 相同）⇒ 读到 JPOV 里是 **米**（loader 换算）。
- 骨名**保留 `mixamorig:` 前缀**（去掉后骨名命中 0/22，蓝人直接塌成一条线）。
- 22 根具名骨（= Mixamo23 去掉无名包装根）；`Lcl Translation = rest_offset`，
  `Lcl Rotation = bind · pose`，**bind 与官方逐骨一致**（不经 Blender，§6 第 6 条）。
- `AnimationStack` 带完整时间轴；帧率 = 源视频帧率（默认 24fps）。
- 体量参考：121 帧 ≈ 240 KB（≈ 2 KB/帧）。

### 8.2 放进哪个目录

所有 3D 资产集中在 `tools/jpov/assets/models/`，按用途分层（见该目录 `BUILD` 头注释）：

```
tools/jpov/assets/models/characters/     ← 带骨骼人形 + 动作（Mixamo 系）
    mixamo_male.glb                       （Tripo 真皮角色，蒙皮资产）
    hip_hop_dance.fbx                     （官方 Mixamo 舞蹈，动画源）
    <你的动捕结果>.fbx                     ← 放这里
```

同时在该目录 `BUILD` 里加一个 `filegroup`（否则 Bazel 目标看不到它，测试/工具在 sandbox 里打不开）：

```python
# tools/jpov/assets/models/BUILD
filegroup(
    name = "my_mocap_fbx_data",
    srcs = ["characters/my_mocap.fbx"],
)
```

如果这个 FBX 要当**单测夹具**，就在测试目标里 `data = ["//tools/jpov/assets/models:my_mocap_fbx_data"]`；
只给交互工具用则可以不加（命令行直接给路径）。

### 8.3 JPOV 用户怎么读

代码侧只需要两个 loader（都在 `src/fbx_loader.h`，**同一份文件必须进两个入口**，否则关节顺序可能分叉）：

```cpp
jpov::SkeletonType skeleton;                       // 骨架：米制 rest_offset + bind_rotation
jpov::FBXClip      clip;                           // 动画：fps + 全帧（joint_rotation 相对 bind）
jpov::LoadFbxSkeleton("…/my_mocap.fbx", &skeleton);
jpov::LoadFbxAnimation("…/my_mocap.fbx", &clip);
```

拿到 `SkeletonPose` 序列之后有两种用法：

1. **同名骨架直接播**：自己的角色骨名也是 `mixamorig:*` ⇒ `RegisterSkeleton(type, poses)` 把整包 pose 烘焙成
   atlas，每帧 `cmds->DrawMeshWithSkeleton(mesh_id, skeleton_id, material, instances)`
   （GPU 蒙皮 + 双帧插值，参考 `demo/fbx_viewer` 的 `glb_skin_poses_` 装配）。
2. **跨资产重定向**：目标骨架不同（Tripo glb / 自建骨架）⇒ 用 `jpov::BodyRetargetPose(plan, pose, &out)`
   搬到目标骨架上（骨名对位 + `Q_body` 整体朝向差），细节见 `interface/skeleton_retarget.h`。

只想看效果（不写代码）：

```bash
# 交互窗口
output/jpov_fbx_viewer/jpov_fbx_viewer tools/jpov/assets/models/characters/my_mocap.fbx
# 出视频（真皮角色 + 两角度）
output/jpov_fbx_viewer/jpov_fbx_viewer tools/jpov/assets/models/characters/my_mocap.fbx \
    tools/jpov/assets/models/characters/mixamo_male.glb --view 10 \
    --video output/my_mocap.mp4 --frames 121 --fps 24 \
    --azimuth-deg 0 --azimuth2-deg 90 --elev-deg 5
```

### 8.4 交付前验收清单

按顺序过一遍（前两条最便宜、也最容易抓住 bind 错）：

1. `--rest` 出 T-pose：骨不歪、左右对称（骨架/rest_offset 对）。
2. `--rest --view 10`：真皮 T-pose **不拧麻花**（bind 与资产一致；不一致就是 §6 第 6 条那类问题）。
3. 均匀抽 3~5 帧出两角度图：朝向、重心、四肢朝向对得上原视频。
4. 连续播放整段：无穿地、无脚滑到离谱、首尾帧循环不跳（若要做循环动画）。
5. `bazel test //tools/jpov/mocap/algo:mocap_fit_test`（算法侧不变式）。
6. 验文件本身：骨名带 `mixamorig:`、22 骨、帧率/帧数与源一致（`jpov_fbx_viewer` 启动日志会报 `bones=22 frames=… fps=…`）。

### 8.5 分发形态

- **仓库内**：资产进 `assets/models/`，用户/测试经 Bazel `data` + `jpov::GetModelsDir()` 定位。
- **分发态（exe 旁）**：构建脚本把需要的资产拷到产物目录（参考 `tools/jpov/build_jpov_fbx_viewer.sh`
  拷 `fonts/` 的做法）；或由调用方命令行直接给路径（viewer 就是这么做的）。
- **没有“一键打包器”**：一期就一个 FBX 文件，不搞 zip/manifest；需要时再上（见 §7）。
