# assets/ — 入库的样例材质资产

「组合查看器」的用法：**给几样渲染几样**。

- `shape`（`.pbtxt`）：必需，但省略时用打包目录里的 `shapes/willow.pbtxt`
- `bark/`：树皮材质目录，省略 = 树干按 level 上棕色
- `leaf/`：叶卡贴图（黑底 = 透明），省略 = 只画 debug 绿卡（纯拓扑视图）

## bark/bark001

ambientCG 的素材是 **CC0**（可商用、无需署名）；叶卡图是自有素材。

| 文件 | 来源 | 说明 |
|------|------|------|
| `albedo.png` | ambientCG `Bark001_1K-JPG_Color.jpg` | 无 JPEG 色度抽样、无块状噪声 |
| `normal.png` | `Bark001_1K-JPG_NormalGL.jpg` | **OpenGL 约定**（绿 = +v），引擎按 GL 采 |
| `roughness.png` | `Bark001_1K-JPG_Roughness.jpg` | 灰度，引擎只采 `.r` |

PNG 是**转换后入库**的成品（`make_bark_material.sh` 的产物），这样 clone 下来不需要
ImageMagick 就能直接跑。原始 JPG 在 `tools/vegetation/textures/bark001/`，重做转换：

```bash
./tools/jpov/tools/vegetation_tool_v2/make_bark_material.sh \
    tools/jpov/tools/vegetation/textures/bark001 \
    tools/jpov/tools/vegetation_tool_v2/assets/bark/bark001
```

## leaf/

叶卡图是自有素材（非第三方）。

| 文件 | 说明 |
|------|------|
| `willow_leaf.png` | 柳树叶簇，**黑底**（`max(r,g,b)` 低于阈值当透明），根部在**左下角** ⇒ 配 `anchor_mode: "bottom_left"` |
| `willow_leaf_flip.png` | 同上**左右翻转**，根部在**右下角** ⇒ 配 `anchor_mode: "bottom_right"`（镜像手性的 leaf type 用这张，引擎不做运行时镜像） |

- 尺寸 1151×1601，根在左下 `(0.07, 0.95)`。
- 用的是**黑底素材**，不是带 alpha 的 PNG：viewer 的 alpha 由 `max(r,g,b)` 现算
  （`--leaf-black` 控制阈值），自己带 alpha 的图会被忽略掉 alpha 通道。
- `--leaf-black`：软过档是 `[0.5t, 1.5t]`。这些图背景是 `≈10~13` 的灰噪、内容 `≥44`，
  所以给 **30**（= `[15, 45]`）；默认 12 会把灰噪留成半透明（远景一圈灰雾）。

## 用法

```bash
cd tools/jpov && ./build_jpov_vegetation_v2_viewer.sh      # 打包：会把 assets/ 拷成 leaf/ + bark/
cd output/jpov_vegetation_v2_viewer

./jpov_vegetation_v2_viewer                                                  # 只渲染 shape
./jpov_vegetation_v2_viewer --leaf leaf/willow_leaf.png --leaf-black 30 \
    --bark bark/bark001                                                       # 完整组合
xvfb-run -a ./jpov_vegetation_v2_viewer --capture shots/full \
    --leaf leaf/willow_leaf.png --leaf-black 30 --bark bark/bark001           # 无窗口出图
```
