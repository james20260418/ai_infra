# jpov/mocap — 视频 → Mixamo23 骨骼动画（实验性工具链）

> 目标：给一段人像视频（或一组连环画 still），产出一段**符合 Mixamo 23 骨布局**的动作，
> 落成 FBX 后用 `jpov_fbx_viewer` 播放，肉眼/AI 看图验收。
>
> 定位：**实验工具**，不在引擎运行时链路上，不进 `Renderer`。

## 1. 一句话结论

```
视频 ──ffmpeg──▶ 帧 ──MediaPipe PoseLandmarker──▶ 33 点(2D + world 3D)
                     ──mocap_fit（三种算法）──▶ 每帧 23 骨 pose
                     ──Blender──▶ FBX（Mixamo23 骨架 + 关键帧）
                     ──jpov_fbx_viewer --shot──▶ 逐帧图 ──ffmpeg──▶ 对比视频
```

## 2. 目录

| 路径 | 作用 |
|---|---|
| `extract/pose_extract.py` | 视频或图片目录 → `track.json`（MediaPipe，33 点 + 置信度） |
| `fit/mixamo23.py` | Mixamo23 骨架的 Python 镜像（与 `interface/mixamo23_skeleton.h` 同表）+ FK |
| `fit/mocap_fit.py` | **算法核心**：关键点 → 每骨方向 → swing 拟合 → 每帧 pose（3 种算法） |
| `fit/export_fbx_ascii.py` | **pose → FBX（ASCII 7.4），直接写文件**，不经 Blender（推荐） |
| `fit/export_fbx.py` / `export_fbx_template.py` | 早期 Blender 路线，**已弃用**（见 §6.6） |
| `fit/render_check.py` | 把 pose 叠回原视频帧（2D 比对图），算法调试用 |
| `fit/render_fbx_video.sh` | 旧的外部逐帧脚本（保留作对照；新流程用 viewer 的 `--video`） |
| `algo/` | **C++ 动捕算法核心**（GL-free）：方向场注册表 / fit（yaw + root + 钉地）/ 批量报告 CLI |
| `testdata/` | 测试素材（Wikimedia CC 拳击片段 + 拉弓射箭） |

## 3. 三种算法（本次实验的对比对象）

三种算法**共用**同一套 retarget 数学（见 §4），差异只在「每骨世界方向场怎么来」：

| algo | 方向从哪来 | 优点 | 缺点 |
|---|---|---|---|
| `worldz` | 直接信 MediaPipe 的 world landmarks（网络回归的近似米制 3D） | 深度"看起来"连续、无参数 | z 噪声大，画面内方向也被带偏（实测 5.4° 平面内误差）；抖动 30mm |
| `lift2d` | **只用 2D 关键点 + 骨长先验反解深度**：投影长度 u 已知、骨长 L 已知 ⇒ 出平面分量 √(L²−u²)，符号取 world-z 差值 | 画面内方向**精确**（0.0°）、深度"量"是几何推的 | 2D 抖动原样进入（42mm），深度符号会跳 |
| `lift2d_t` | `lift2d` + **零相位** 2D 平滑 + 深度符号时序一致（中值）+ 深度零相位平滑 | 抖动 3.2mm（比 worldz 小 9×），**无滞后** | 平面内方向被平滑轻微带动（0.6°） |

指标（`--algo` 跑同一段拉弓射箭，121 帧 @24fps）：

| algo | dir_err(平面内) | reproj RMS | jitter |
|---|---|---|---|
| worldz | 5.4° | 58.4 px | 29.9 mm |
| lift2d | 0.0° | 58.9 px | 41.8 mm |
| lift2d_t | 0.6° | 58.9 px | **3.2 mm** |

> `reproj RMS` 几乎不随算法变：它被 **骨架比例差** 支配（Mixamo23 是 1.60m 官方比例，
> 被测者比例不同 ⇒ 肩/肘天生偏 50~60px），**不能**当算法好坏的判据。方向误差才是。
> 这条是本次踩到的坑：一开始拿 reproj 当主指标，三种算法看不出差别。

## 4. retarget 数学（与 C++ 侧同一套）

每骨 target：`W(j) = Swing(Rb(j)·ŷ → d_j) · Rb(j)`，即**相对自己 rest 的最小摆动**，
roll 从 bind 继承（T-pose 时零误差）。然后

```
P(j) = B(j)⁻¹ · W(parent)⁻¹ · W(j)      // P = 相对 bind 的 local 旋转
```

与 `interface/skeleton_retarget.h` 的「偏差式」同源，只是这里的"源"是**关键点方向**而不是
另一套骨架。不变量复用同一个：`d_j == Rb(j)·ŷ ⇒ P = identity`（T-pose 不会被动过）。

## 5. 怎么跑

```bash
# 0) 依赖（只需要 numpy；MediaPipe 单独装）
python3 -m venv /tmp/mpvenv && /tmp/mpvenv/bin/pip install mediapipe opencv-python numpy
# 模型（一次）：pose_landmarker_full.task ≈ 9.4MB
curl -L -o /tmp/pose_full.task \
  https://storage.googleapis.com/mediapipe-models/pose_landmarker/pose_landmarker_full/float16/latest/pose_landmarker_full.task

# 1) 提取关键点
/tmp/mpvenv/bin/python extract/pose_extract.py --video testdata/archery/拉弓射箭.mp4 \
    --out /tmp/archery_track.json --overlay-dir /tmp/ov --overlay-every 12

# 2) 拟合（三种算法）
for a in worldz lift2d lift2d_t; do
  /tmp/mpvenv/bin/python fit/mocap_fit.py --track /tmp/archery_track.json \
      --out /tmp/pose_$a.json --algo $a --height 1.78
done

# 3) 出 FBX（直接写 ASCII，无需 Blender）
python3 fit/export_fbx_ascii.py --poses /tmp/pose_lift2d_t.json --out /tmp/lift2d_t.fbx

# 4) 出对比视频
fit/render_fbx_video.sh /tmp/lift2d_t.fbx output/lift2d_t.mp4 24 121
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

## 7. 未做 / 下一步

- **手部朝向**（palm normal）没有解，`LeftHand` 只按前臂方向摆。
- 深度符号在遮挡/自交时仍会翻（`lift2d_t` 靠中值压制）。
- 骨架比例固定为 Mixamo23；被测者高一截/矮一截会让肘膝位置偏（这正是 reproj 58px 的来源）。
- 只导 **旋转** + 根平移；脚做的是「最低脚钉 y=0」的整体 grounding，**不是 IK**：
  走跑会滑，跳跃会被拉回地面（第一期明确边界）。
- C++ 端口已完成（`algo/` + `jpov_mocap_fit` + gtest）；下一步是接 SMPL/GVHMR
  作为新的 `DirectionAlgorithm`，而不是再写一条链路。
