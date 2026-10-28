# jpov/legacy —— 已下线代码的**纯备份**（不编译、不测试、不参与 CI）

这里放的是被新方案取代、但仍想留档的代码。规则只有一条：

- **不参与构建**：`legacy/` 下没有 `BUILD` 文件，任何目标都不依赖这里；
- **不修 bug**：需要改行为请改现役实现（v2 在 `tools/jpov/tools/vegetation_tool_v2/`）；
- 唯一允许的改动是「文件头补一句归档说明」。

## 内容

### `vegetation/` —— vege v1 生成链（Blender / Sapling + Python）

Sapling 建树（`build_tree.py`）→ 烘风（`wind_bake.py`：Voronoi 推断关节归属）→ 导出 glb。
作者当时留下的一套文件格式约定（glTF 两个扩展位：`TEXCOORD_1 = (joint_id, phase)`、
node extras `jpov_wind_joints` 每关节 7 float）**仍然有效**，只是生产者换成了 v2 烘焙器
（`vegetation_tool_v2/bake/`）。要看格式定义请看
`tools/jpov/docs/jpov_vegetation_wind_design.md`，不要来这里找。

`samples/` 里的柳树 glb 是 v1 产物（关节归属靠空间 Voronoi 猜，分叉处要靠渲染侧去补），
`textures/` 是当时下载的 CC0 素材（被 `.gitignore` 忽略，只在本地）。

### `vegetation_viewer/` —— vege v1 查看器 + CPU 风参考实现

- `jpov_vegetation_viewer.cc` / `vegetation_viewer_app.h`：v1 植物查看器（天光 + 实例化森林 + 面板）。
- `vegetation_mesh_export.{h,cc}`：把「当前风形态」离线导成网格，用来对账 GPU 结果。
- `wind_deform.h`：**v1 的 CPU 版风变形**（父链长度 L + 按位置焊接的 Laplacian 平滑 +
  最近两关节按 1/d 混合）。现役渲染已改成单关节（joint_id + phase），这份实现刻意留在
  这里当历史快照，不做同步。
- `build_jpov_vegetation_viewer.sh`：当时的构建/分发脚本（`bazel` target 已删除，跑不通）。

## 为什么下线（2026-10-26）

v2 生成链自己扫管，每个顶点的关节归属是**已知**的（同一枝同一环共享一个关节），
所以渲染侧不需要再靠「空间最近的两个关节 + 距离权重 + 5mm 焊接 + L 平滑」去救场；
那套补丁把 instance pass 的顶点属性撑到 `(j0, phase, L, j1)` + `(w0, w1)`，收益已经归零。
现役实现：`loc6 = vec2(joint_id, phase)`，路径长 L 由 VS 沿关节链现算。

## 想跑起来？

别跑。要看树请用 v2：`tools/jpov/build_jpov_vegetation_v2_viewer.sh`。
