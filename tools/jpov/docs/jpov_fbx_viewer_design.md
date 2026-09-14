# JPOV FBX 观察器（FBX Viewer）— 设计

> 目的：把一段 **FBX 动作素材**以**火柴人**形式按它自己的 fps 播出来，肉眼 + 数值
> 验证「**从 FBX 的 Lcl Rotation 构造出的 skeleton pose 合不合法**」——
> 动作看起来是不是一个能辨认的人形在动、每帧位姿是不是源文件那一帧的位姿。
>
> 本文记录**已落地**的实现与其中的判断（实现见 `tools/jpov/demo/fbx_viewer/`）。
> 相关：`jpov_retarget_design.md`（§4 火柴人可视化 / §6.4「先用骨人调通，再上真皮」）。

---

## 1. 为什么需要它

retarget（M4）之前有一步「**先别上真皮**」：把 FBX 动作套到**火柴人**上看对不对
（`jpov_retarget_design.md` §6.4，Danis 定）。火柴人排除了 mesh/蒙皮本身的干扰，
能把「动作数学对不对」与「mesh 蒙皮对不对」解耦验证。

本工具就是这条路的载体，同时把上一批 PR 的产物串起来验收：

| 已落地产物 | 在本工具里的角色 |
|---|---|
| `LoadFbxAnimation`（帧存**相对 bind 的增量旋转**，2026-09-14 修复） | 动作帧的来源 |
| `LoadFbxSkeleton`（rest_offset 归一为**米**） | 骨架的来源 |
| `BuildBoneMeshInBoneSpace`（骨人 mesh，PR #94） | 渲染载体 |
| `view_config.h`（sunny day 光照 + 右键环绕相机） | 场景与交互 |
| `interface/animation_sampler.h`（**本 PR 新增**） | 「时刻 → 位姿」的取帧原语 |

---

## 2. 行为规格

| 项 | 值 |
|---|---|
| 输入 | 一个**带动画**的 `.fbx` 路径（命令行参数） |
| 场景 | 标准晴天（`MakeLighting(90°, turb=2)` 天光 + 太阳 + 环境光）+ 浅灰地面板（顶面 y=0，脚踩地） |
| 相机 | 右键 drag 环绕、滚轮 zoom（与模型查看器同一套 `ViewConfig`/`ApplyInput`），初始距离按火柴人包围盒自适应 |
| 渲染内容 | **当前帧的骨人（火柴人）**：每根骨一根细红杆，根杆收窄 1/3（区分 root） |
| 播放 | 按**源 FBX 的 fps** 播放：60fps 渲染下每帧推进 1/60 秒，帧间逐骨 **Slerp** 插值；**循环**（末帧回绕首帧） |
| 暂停 | 底部**按钮**：按下后**主频的时间停止更新**（画面定格在当前帧），按钮变「继续播放」 |
| 模式 | 底部**复选框**「固定 rest 位姿(identity)」：勾上 = 不看动画，固定 `pose = identity` 渲骨架 rest（T-pose）形态；此时时间同样不推进，取消勾选后从停住的时刻继续 |
| 状态行 | 帧号 / 总帧数 / 源帧频 / 动画时刻 / 播放 or 已暂停 |
| headless 出图 | `--shot <png> [--time <秒>｜--frame <帧号>] [--rest]`：指定时刻的单帧截图（AI 自查/留证；与交互共用同一条 `OneIteration`，零分叉） |

---

## 3. 关键设计判断

### 3.1 骨架与动画**分两个 loader 入口**取，靠「旋转与单位无关」拼起来

- `skeleton_` ← `LoadFbxSkeleton`：`rest_offset` 已归一为**米**，`bind_rotation` 为 bind 朝向；
- `clip_` ← `LoadFbxAnimation`：`fps` + 全帧；帧里存的是**相对 bind 的增量旋转**。

帧里只有**旋转**（纯角度量，与单位无关）+ 根位移，而骨架给的是**米制骨长** —— 两者
叠起来天然自洽（米制骨架 + 增量旋转），无需任何缩放。两个入口的骨序同源（同一条
`CollectBoneNodes` DFS），`LoadFbx` 里 **CHECK 骨数 + 逐骨骨名一致**，防止将来某侧改了
收集顺序而静默错位（把 A 骨的旋转套到 B 骨上）。

> 为什么不直接用 `LoadFbxAnimation` 的 `clip.skeleton`（源单位 cm）？那样骨架是 cm 制，
> 要么整体缩放（还得知道 `unit_meters`），要么场景尺度不对。用 `LoadFbxSkeleton`
> 顺便复用「与 glb / Mixamo23 同尺度（米）」这条既有约定。

### 3.2 「位姿 → 几何」直通（CPU 重建 mesh），不走 GPU 蒙皮

每帧：`anim_time_seconds_ --(SampleClipPose)--> SkeletonPose --(BuildBoneMeshInBoneSpace)-->
MeshData --(UpdateMesh)--> DrawObject3D`。

- 本工具要验的是**位姿本身**，链路上每一步都可见（骨杆朝向 = 每骨位姿的直接反映），出问题好定位；
- 骨数少（Mixamo 全身 65 骨 × 每骨 24 顶点 = 1560 顶点），每帧重建开销可忽略；
- 且**只在位姿真的变了时重建**（键 = `(rest 模式, 动画时刻)`）：暂停 / rest 模式下零重建。

> 备选：走 `RegisterSkeleton`（pose atlas）+ `DrawMeshWithSkeleton` 的 GPU 逐骨插值。
> 不选它的理由：① 本工具验证的粒度是「位姿对不对」，不需要蒙皮中间层；
> ② GPU 侧是矩阵 lerp（会收缩），而"合理的 pose 差值"应当用四元数 Slerp（见 §3.3）。
> 将来做**人群/真皮**时那条路另走（`jpov_skeleton_manager_design.md`）。

### 3.3 时间采样：`interface/animation_sampler.h`（新）

`SampleClipPose(clip, t, out)` = 一段动画素材的**取帧原语**，规则写在文件头：

1. **帧网格**：`f = t·fps`，起始帧 `k = floor(f)`、权重 `ratio = f − k`（源 key 恰好落在帧网格上，
   故"落在哪两帧之间"是纯算术）；
2. **插值**：每骨四元数 **Slerp**（等角速度、结果仍是单位四元数；矩阵/四元数线性插值会引入
   非旋转的收缩分量）；根位移 `root_offset` 线性插值；
3. **循环**：时长 = 帧数 / fps，`t` 先按 `fmod` 归一（负数同理），到末尾**回绕首帧**
   （`k+1` 取模）—— 循环动作在接缝处照样插值；
4. **帧精确**：`ratio ≈ 0`（阈值 `kFrameEpsilon = 4e-6` 帧，吸收 `t·fps` 的浮点往返误差）
   时直接**逐值拷贝**该帧 —— 按 fbx 的 fps 播放时给出的就是源帧本身，无插值误差。

> 放在 `interface/` 而不是 viewer 目录：它是 `FBXClip` 容器的配套取帧原语（与容器同层），
> 后续 retarget 取源姿态同款（M4 会需要）。

### 3.4 「合法」怎么被验证（三层，逐层更强）

| 层 | 手段 | 结果 |
|---|---|---|
| L1 单测 | `interface/animation_sampler_test`：合成 clip 锁帧归属 / Slerp（非线性）/ 回绕 / 归一 / 单位性 | 9 例全绿 |
| L2 真实资产 | `test/jpov_animation_sampler_test`：真 fbx 上验两入口一致、帧精确、全时间轴位姿合法、动作在动、循环归一；**并与 ufbx 自己的求值 `ufbx_evaluate_scene` 逐骨交叉验证** | 帧网格上偏差 **~5e-7 m**（浮点噪声级）；帧间偏差 ≤ **2.7e-3 m**（指尖，两条插值路径的固有差异） |
| L3 视觉 | `test/fbx_viewer/jpov_fbx_pose_gold_test`：渲固定帧与仓库 gold 比对 + 可见性门禁 + 「换时刻/换 rest 必须换画面」动态门禁 | diff=0；变化像素 1551 / 1722 |

**L2 的交叉验证是本 PR 最硬的一条证据**：它证明"我们的骨架 + 采样位姿"复现出的每骨世界
位置与 ufbx 直接求出的**源文件权威结果**一致到浮点精度 —— 即渲染出来的动作**就是源文件的
动作**，不是"看起来像人"而已。

> 比较时用「**相对根骨**的位置」（两边都减去根骨位置）：loader 只收 bone 节点，根骨之上
> 的非 bone 祖先变换（Mixamo 的包装层）不入骨架空间 —— 那是**整体刚性**差异（骨架空间
> 原点 ≠ 场景原点），不影响动作本身；相对根骨比较正好把这一层剔除。

### 3.5 目录与分层（与 viewer/editor 同一套惯例）

```
tools/jpov/demo/fbx_viewer/
  fbx_viewer_app.h            # 渲染核心 App（场景 + 位姿采样 + 播放面板 + 时间推进）
  jpov_fbx_viewer.cc          # 主程序（CLI 解析 + 装配 + 交互/出图分发）
  fbx_viewer_playback_test.cc # 播放控制白盒回归（纯 CPU：暂停/rest 冻结时间、位姿来源）
  BUILD
tools/jpov/interface/animation_sampler.h(+_test.cc)   # 取帧原语（GL-free、可单测）
tools/jpov/test/jpov_animation_sampler_test.cc        # 真资产 + ufbx 交叉验证
tools/jpov/test/fbx_viewer/                           # 固定帧渲染 gold（generator/test/PNG）
tools/jpov/build_jpov_fbx_viewer.sh                   # 编译 + 拷字体 + 打印用法
```

为什么不复用 `ViewerApp`（模型查看器）：两个工具被观察的对象与面板语义不同（viewer 看
静态 glTF + 光照滑条；本工具看 FBX 动作 + 播放控制），塞进一个类会变成 `show_editor_`
式的分支丛林（同 `editor_app.h` 的判断）。真正共享的是**场景构造**（`view_config.h`）、
**火柴人几何**（`skeleton_mesh.h`）、**时间采样**（`animation_sampler.h`）与出图路径
（`JPOV::RunOnce`）—— 该共享的都共享了，外壳各自独立。

---

## 4. 已知取舍 / 边界

- **播放时钟按渲染帧步进**：每帧推进 `1 / kViewerFps`（= 1/60 秒），即“按 fbx 的 fps 播放”
  是在渲染循环保持 60fps 的前提下成立。软渲染（llvmpipe，无 GPU）下若实际帧率低于 60，
  动画会按比例变慢（例如 40fps → 0.67× 速）。选这个时钟而非墙上时钟的原因：① 与
  `OneIteration` 给出的时间基准一致（帧计数器）、确定性强；② “暂停 = 主频的时间停止更新”
  语义直接（数帧即时间）；③ 出图（`--shot`）可精确对齐到某一帧。若要“不论帧率都按真实
  时间走”，把 AdvancePlayback 的 dt 换成墙上时钟差即可（需额外处理出图模式）。
- **循环时长 = 帧数 / fps**（如 518/30 = 17.267s，略长于源时窗 17.233s）：末帧到首帧之间
  也会插入一个 1/30s 的过渡区间 —— 这正是"循环播放"要的接缝行为。
- **帧间插值与源不完全等价**：源是逐通道（Euler 曲线）插值，我们是对合成后的四元数 Slerp，
  差异在指尖量级 ≤ 3mm（1.7m 身高）。需要"逐通道等价"时应在 loader 层保留曲线（当前不做）。
- **`root_offset`（root motion）不参与渲染**：`BuildBoneMeshInBoneSpace` 只用 `rest_offset`
  与旋转（根杆 = 根位置矢量）。本 PR 的目标是**旋转**合法性；root motion 的接线
  （`skeleton_manager.cc` 烘焙）是 M3，与本工具无耦合。
- **不做**：重定向（M4）、真皮蒙皮（M6）、编辑/保存（M5）、多动作剪辑管理。
