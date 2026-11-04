# JPOV 视频动捕（video → Mixamo23）· 调研 + 设计 + 实验记录

> 2026-11-03。目标：一段人像视频 → 一段**Mixamo 23 骨**动作，落成 FBX 可看可对比。
> 代码在 `tools/jpov/mocap/`（实验工具，不进运行时链路）。本文记录**为什么这么做**
> 与**实测结论**，避免下一轮重新踩。

---

## 1. 调研：业界怎么做的

主流是 **SMPL 系**（人体参数化模型）+ 视频反解：

```
video ──(检测/跟踪: ViTPose, YOLO, SAM)──▶ 2D 关键点
      ──(3D 人体反解: WHAM / GVHMR / 4D-Humans / TRAM / HybrIK)──▶ SMPL 参数
      ──(retarget: Blender / Rokoko / 商业)──▶ Mixamo / UE mannequin
```

关键事实（调研 + 社区实测）：

| 事实 | 影响 |
|---|---|
| SMPL / SMPL-X 模型权重**需注册授权**，不能随工程分发 | 上面那条链**在这台机器上跑不起来**（除非走 ComfyUI + 手工下权重） |
| 社区有人把这条链跑到底，公开的最好成绩 MP≈39/100，且明确写"天花板由提取器决定，不是 retarget" | 不要指望"接个模型就完美" |
| 舞蹈/运动场景的失败点是**快速动作 + 遮挡 + 多人物** | 单人固定机位是最优起点 |
| retarget 侧业界共识：搬**关节相对 bind 的额外旋转**，不是搬关节位置 | 与本仓 `skeleton_retarget.h` 的 ⟨★⟩ 一致 |

**结论**：走 **MediaPipe PoseLandmarker**（Apache-2.0，权重随包可下载，33 点 2D +
world-3D）。牺牲精度，换来**可复现、可分发、当天能跑通**。SMPL 路线留作后续可选项。

---

## 2. 数据契约

`extract/pose_extract.py` 输出 `track.json`：

```jsonc
{ "version":1, "fps":24, "width":720, "height":960,
  "frames":[ { "t":0.0417,
     "landmarks":[{"x":px,"y":px,"z":rel,"visibility":v} ×33],   // 像素空间，y 向下
     "world":[{"x":m,"y":m,"z":m} ×33],                          // 髋原点近似米制，y 向下
     "presence":0.98 } ] }
```

提取器**保持哑**：不翻 y、不归一化、不插值。所有解释都在 C++/Python 的 fit 侧，
这样换提取器（换 GVHMR 等）只换 `track.json` 的产出方式。

两种输入：`--video`（VIDEO running mode，有跨帧跟踪）与 `--images`（IMAGE mode，
给"AI 连环画"用，逐张独立检测）。

---

## 3. 算法

### 3.1 骨方向场（三种）

| algo | 定义 |
|---|---|
| `worldz` | `d_j = normalize(world[child] - world[parent])` |
| `lift2d` | `u = |p2d_child - p2d_parent| / s`（s = 像素/米，全局中位数）；<br>`if u < L_j: z = ±√(L_j²-u²)`（符号取 world-z 差）；`d_j = (dx, dy, z)/L_j` |
| `lift2d_t` | `lift2d` + **零相位**高斯平滑（正反各滤一次）+ 深度符号中值保持 + 深度分量再平滑 |

**为什么 `lift2d` 在原理上更好**：单目里"骨在画面里的方向"是**可直接观测**的（2D 关键点），
"骨朝镜头多少度"只能靠**骨长先验**推。MediaPipe 的 world-z 是网络回归出来的，
在快速动作上噪声极大；而 2D 关键点在同样的帧上是稳的。
`√(L²-u²)` 就是"骨头被投影缩短了多少 ⇒ 出平面了多少"，这是唯一有物理依据的换算。

**符号仍然只能借 world-z**：单目深度歧义（+z 与 −z 投影相同）数学上无解，必须外部信号。
这也是 `lift2d` 唯一还依赖 MediaPipe 3D 的地方。

### 3.2 retarget（与 C++ 同一套）

```
W(j) = Swing(Rb(j)·ŷ → d_j) · Rb(j)        # 相对自己 rest 的最小摆动，roll 继承 bind
P(j) = B(j)⁻¹ · W(parent)⁻¹ · W(j)          # local 旋转（相对 bind）
```

不变量：`d_j = Rb(j)·ŷ ⇒ P = identity`（T-pose 不动）。与 `skeleton_retarget.h`
的"偏差式"同源，只是源从"另一套骨架"换成"关键点方向场"。

### 3.3 为什么是 swing-only（不做 twist 优化）

关节 twist（骨绕自身轴转了多少）**单目不可观测**，只能靠先验猜。swing-only =
"相对 T-pose 零 twist"，可预测、无抖动。代价：手掌朝向、前臂旋前/旋后不对。
实测拳击/射箭这类动作里，**方向对就够辨认**，twist 错只影响手的姿态细节。

---

## 4. 实验（拉弓射箭，121 帧 @24fps，单人正面全身）

| algo | dir_err（画面内） | reproj RMS | jitter |
|---|---|---|---|
| worldz | 5.4° | 58.4 px | 29.9 mm |
| lift2d | **0.0°** | 58.9 px | 41.8 mm |
| lift2d_t | 0.6° | 58.9 px | **3.2 mm** |

- `dir_err` = FK 骨方向 vs 画面里同骨 2D 方向的夹角。**这才是算法该看的指标**。
- `reproj RMS` **不能**当判据：它被"骨架比例 vs 被测者比例"支配（Mixamo23 是 1.60m
  官方比例），三种算法都 58~59px，看不出差别。踩过这个坑。
- `jitter` = 关节位置二阶差分均方。`lift2d` 比 `worldz` 差，因为把 2D 抖动原样带进 3D；
  `lift2d_t` 用**零相位**平滑后降到 3.2mm，且**没有滞后**（普通滑动平均会把动作推后
  (win−1)/2 帧，24fps 下 7 帧窗 = 0.13s，足够让"拉满"和"起手"错位）。

**AI 看图裁判**（同一帧并排渲染 + `image` 工具打分）结论一致：`lift2d*` 明显优于
`worldz`，`worldz` 会把两只手臂缩成一团（深度噪声把骨方向拽向镜头轴）。

---

## 5. 产物链（FBX）

`poses.json` →（Blender headless）→ FBX →（`jpov_fbx_viewer --shot` 逐帧）→ mp4。

### 两个必须记住的坑（都实测踩过）

1. **轴向**：Blender 把「Z-up→Y-up」的 −90°X 转换挂在**骨架对象节点**上，
   而 ufbx/JPOV 读的是**骨节点的 local translation** ⇒ 只有根骨被转、子骨没转，
   整个人躺在地上。
   **解法**：在 Blender 里先把骨架对象 `rotation_euler=(+90°X)` 并 **apply**，
   再用 `axis_up="Z"` 导出 —— 此时文件里的数字本身就是 Y-up。
2. **单位**：Blender 默认写出"数值是米、却声明 cm"，ufbx 报 `unit_meters=0.01`
   ⇒ JPOV 再 ×0.01，角色缩成 1cm。**解法**：几何按 **cm** 写进 Blender
   （`--unit-scale 100`），与 Mixamo 官方 FBX 同惯例。

---

## 6. 现在的质量与天花板

**能看的**：躯干朝向、双臂的大方向（伸/屈/抬）、腿部大体站姿、动作节律。

**已知缺陷（按影响排序）**：

1. **骨架比例固定**：Mixamo23 是 1.60m 官方比例，被测者高/矮会让肘膝落点偏
   （这是 reproj 58px 的全部来源）。业界做法是按被测者身高/肢长**逐骨**适配。
2. **脊柱只有 3 段共线**：33 点里没有脊椎中段，`Spine/Spine1/Spine2` 只能用同一个
   方向，弯腰/侧倾表达不出来（射箭的"挺胸"就丢了）。
3. **手部无朝向**：无 palm normal，`LeftHand/RightHand` 只跟前臂方向。
4. **深度符号在自交/遮挡时会翻**：中值滤波只能压制，不能解。
5. **多人物会串**：两人贴身时 MediaPipe 把两个人的点混成一个人（实测拳击比赛片段
   完全不可用）。这不是本工具的 bug，但**是使用边界**。
6. **无脚部 IK**：站立动作够用，走跑会滑。

---

## 7. 后续可做（按性价比）

1. **逐骨比例适配**（把 `√(L²-u²)` 里的 L 换成被测者的实测骨长 + 输出侧按比例缩放）——
   直接消掉最大的一项误差。
2. **脚部锁地 IK** + 根位移滤波。
3. **SMPL 路线对照**（GVHMR）：若拿到授权权重，可作为第四条算法进同一套对比框架
   （`track.json` 契约不变，只需换产出器）。
4. **C++ 端口**：`lift2d` + `pose_from_dirs` 搬进 `interface/` 并加 gtest，
   与 `skeleton_retarget.h` 并列；Python 侧只留提取与实验。

---

## 8. C++ 落地（2026-11-04，Danis 定）

目录 `tools/jpov/mocap/algo/`（**动捕算法独立落点**，GL-free）：

| 文件 | 职责 |
|---|---|
| `mocap_types.h` | track / 方向场 / fit 结果的数据契约 |
| `mocap_landmarks.h` | 33 点 → 22 段骨的**唯一**骨段表（算法与评价共用） |
| `mocap_math.h` | 零相位平滑 / 中位数 / 归一（算法与 fit 共用） |
| `mocap_algorithms.*` | 方向场算法注册表：`worldz` / `worldz_t` / `lift2d` / `lift2d_t` |
| `mocap_fit.*` | 方向场 → Mixamo23 pose（整体朝向 + root + 脚钉地） |
| `mocap_io.*` | track / poses / 批量报告 JSON / CSV |
| `mocap_fit_main.cc` | `jpov_mocap_fit` CLI |

**加新算法的方式**：写一个 `ComputeXxx(track, skeleton) → DirectionSequence`，注册进
`ListDirectionAlgorithms()` 即可；fit、评价、CLI、批量报告全部自动覆盖，不需要改链路。

### 8.1 两个此前缺失的自由度（已修）

1. **整体朝向（yaw）**：从髋线/肩线在水平面的方向估（圆周平均，肩线权重 0.6 / 髋线 0.4），
   鼻尖-肩中心只用于消除 ±180° 二义性。yaw 落在 **Hips 的 local 旋转**上（不是包装 Root）：
   Hips 在 FBX / glTF / BodyRetarget 里都有同名骨，写在那里才会被下游真正搬走。
   施加式：`P_hips ← (B_hips⁻¹ · R_yaw · B_hips) · P_hips`。
   ⚠️ **MediaPipe world 的 z 越小越近相机**，本工程观察世界是 +Z 朝相机 ⇒ 加载时
   `(x, -y, -z)` 一起翻。只翻 y 会让角色面朝反方向（实测 yaw 变 170°，已修）。
2. **根位移 / 悬空**：水平 `(x,z)` = 髋中心 world 相对首帧的位移；竖直 `y` = 先按
   `root_offset=0` 做 FK，取最低脚/趾关节，令 `root_offset.y = −min_foot_y` ⇒ 最低点落
   `y=0`。根位移再做零相位平滑（无滞后）。
   已知边界：第一期假设**至少一只脚着地**；跳跃/腾空没有接触检测，会被压回地面。
   平滑后仍可能有几毫米穿地，报告里 `foot_penetration_mm` 如实给出。

### 8.2 蓝人模特（`interface/skeleton_mannequin.h`）

现成 `mixamo_male.glb` 是 Tripo 资产：rest 网格本身蹲姿、骨长约 1.0m，拿它验收会把
「重定向错」和「资产 rest 错」混在一起。改为**程序化生成**与 Mixamo23 同源的蓝人模特：

- 每个关节一个球 + 每个 `parent→child` 段一个胶囊；段权重给**父关节**（LBS 里绕父关节转），
  球体权重给自己的关节，弯折时靠重叠盖住接缝。
- 顶点在骨架空间（Y-up、模型正面 +Z），`flags = Position|Normal|Joints`，刚性权重。
- viewer 新增 `--view 8`（红骨人 + 蓝模特）/ `--view 9`（只看蓝模特）。

### 8.3 FBX → 视频（viewer 自己覆盖）

`jpov_fbx_viewer` 新增 headless 序列输出，不再需要外部逐帧脚本：

```bash
# 单角度 mp4
output/jpov_fbx_viewer/jpov_fbx_viewer anim.fbx --view 9 \
    --video out.mp4 --frames 121 --fps 24 --azimuth-deg 0 --elev-deg 5

# 两个角度并排（正面 + 侧面）→ 一段 2560×720 视频
output/jpov_fbx_viewer/jpov_fbx_viewer anim.fbx --view 9 \
    --video out_2angles.mp4 --frames 121 --fps 24 \
    --azimuth-deg 0 --azimuth2-deg 90 --elev-deg 5

# 只出帧序列（自己后处理）
... --shot-dir output/frames --frames 121
```

实现：逐帧 `RunOnce` → PNG 序列 → `RunFfmpeg`；两角度各渲一遍、合成两段 mp4 后
`ffmpeg -filter_complex hstack`。临时帧目录用完即删。

### 8.4 实测（拉弓射箭 121 帧 @24fps，C++ 结果）

| algo | dir_err（画面内） | jitter | 说明 |
|---|---|---|---|
| worldz | 5.43° | 30.67 mm | 与 Python 版一致 |
| worldz_t | 5.44° | 4.65 mm | 时序基线 |
| lift2d | 0.003° | 42.06 mm | 画面内方向最准 |
| lift2d_t | 0.65° | **4.67 mm** | 推荐默认 |

`px_per_meter` 463.8；`evaluated_bones` 2662 个样本。
