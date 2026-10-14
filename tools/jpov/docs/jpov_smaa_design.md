# JPOV SMAA 后处理抗锯齿设计

> 状态：MVP 已实现（`tools/jpov/src/smaa/`）。作为 3D 后处理链的**最后一站**，
> 在 tone map 之后、2D/UI 之前，对最终 LDR 图像做 **SMAA 1x** 空间抗锯齿。
> 查看器开关：skylight viewer 面板「SMAA 抗锯齿」勾选框 + 阈值滑条。

## 1. 为什么是这里（分辨率是关键）

后处理链（HDR 路径）：

```
阴影 pass → 3D 不透明 pass（MRT：颜色 RGBA16F + 场景深度 R32F）
        → 雾 → highlight → bloom → tone map(ACES + sRGB)  ← 这里发生「3D FBO → 屏幕」的重采样
        → 【SMAA 1x】→ 2D/UI
```

- **tone map 这一步会改变分辨率**：它读 `fbo_3d_w×fbo_3d_h` 的 HDR 纹理，
  按 `camera.viewport_*`（屏幕/viewport 尺寸）写进主 FBO。两者尺寸**可以不同**
  （`Camera::fbo_3d_width_/height_` 与 `viewport_*` 是两组独立自由度）。
- SMAA 是**逐像素、边缘导向**的 AA：边缘检测在最终灰度（gamma 空间）上做才有意义。
  若在 3D FBO 分辨率下做 AA、再经上面这步重采样到屏幕，AA 结果会被二次过滤、
  且与最终像素栅格**错位**，收益打折甚至变糊。
- 因此 **SMAA 一律在「输出（viewport）分辨率」下工作**：tone map 先把场景写进主 FBO
  的 viewport 矩形，再拷进 SMAA 的 LDR 输入缓冲（同分辨率），三趟都在该分辨率下跑，
  最后一趟直接把结果写回主 FBO 的同一矩形。这样无论 3D FBO 与屏幕是否等尺寸，
  AA 都作用在最终像素栅格上。

## 2. 算法（SMAA 1x，参考 Jimenez 2011）

三趟，与官方参考实现一致：

1. **Luma 边检测**（`smaa_edge`）：在 gamma 空间颜色上算亮度，按阈值找出水平/垂直边，
   输出 `edgesTex.rg`（r=西侧边，g=北侧边）。带「局部对比度自适应」
   （邻边对比度过大时丢弃本边，抑制伪交叉边）。
2. **混合权重计算**（`smaa_weight`）：读 `edgesTex`，按正交 / 对角模式**搜索线端**，
   用两张预计算查找表算「每侧应混合多少」：
   - `AreaTex`（160×560 RG8）：给定距离 + 两侧交叉边 → 覆盖面积；
   - `SearchTex`（64×16 R8）：最后一步长度的 0/1/2 修正。
   还含角点修正（`CORNER_ROUNDING`）。输出 `blendTex`（r/g 水平两侧，b/a 垂直两侧）。
3. **邻域混合**（`smaa_blend`）：按 `blendTex` 权重在两侧邻居间（借双线性）加权混合。

预置参数 = 参考 **HIGH** 预设：`MAX_SEARCH_STEPS=16`、`MAX_SEARCH_STEPS_DIAG=8`、
`CORNER_ROUNDING=25`（编译期常量，写在 shader 里）。只有**边阈值**（`SmaaConfig::threshold`，
默认 0.1）走 uniform，方便查看器实时对比。

未做（后续可选）：depth 边检测 / predication、temporal（T2x）、spatial MS（S2x）、
stencil 优化、运动矢量 reprojection。

## 3. 文件与接口

```
tools/jpov/src/smaa/
  smaa_shader.h       三趟 GLSL 330（参考 SMAA.hlsl 的 GLSL 移植）
  smaa_renderer.{h,cc} SmaaRenderer：建 FBO/纹理、上传 LUT、跑三趟 Apply()
  smaa_area_tex.h     参考实现 MIT 预计算 AreaTex（160x560 RG8 数据）
  smaa_search_tex.h   参考实现 MIT 预计算 SearchTex（64x16 R8 数据）
```

- 数据来源：https://github.com/iryoku/smaa （MIT）。**shader 与 LUT 必须同版本**，
  改其一须同步另一（在文件头有相互引用注释）。Windows 端 `GL_RG8` 已补进
  `third_party/gl_loader-mingw/gl_loader.h` 的「缺失 GL 常量」段。
- 用户接口：`RenderCommandList::smaa`（`std::optional<SmaaConfig>`，见
  `interface/render_command.h`）。`enabled=true` 才生效；**需要 `tone_mapping=true`**，
  否则 Renderer `CHECK` 报错（不静默失效）。
- `Renderer`：`Init` 里 `smaa_renderer_.Init(&shader_mgr_)`；`Render` 的 HDR 路径
  tone map 之后调用 `Apply`。不开 SMAA 时该渲染器完全不被触碰，**零开销零回归**
  （tone map 输出路径字节不变）。

## 4. 边界与已知限制

- **不做时序**（无 TAA）：单帧空间 AA，静止画面有效；运动画面按 SMAA 1x 表现。
- **不认深度**：只在颜色亮度上找边。极低对比度的边（如相近色）检测不到（可调小阈值）。
- 仅 HDR/tone-mapping 路径；旧 LDR 直通路径不支持（会 CHECK）。
- 只看颜色，不看 alpha；对 alpha-test 植物/火粒子的硬边不特殊处理（与主 pass 一致）。
- 只作用于 **viewport 矩形**内的 3D 图像；UI（2D）在 SMAA 之后绘制，不经 AA。

## 5. 验证

- `tools/jpov/test/postprocess/jpov_smaa_test.cc`：纯色三角斜边造锯齿，断言
  ①SMAA 生效（有像素变化）②变化局部（占比 <15%）③远离边缘的平坦像素**逐字节不变**
  ④两次渲染确定。
- skylight viewer：面板开关 + 阈值滑条；`--capture-smaa <dir>` 出 `smaa_off.png` /
  `smaa_on.png` 同机位 A/B。
- 全量：`bazel build //...` + `bazel test //tools/jpov/...`（106 tests / 441 cases）全绿。
