# JPOV 纹理离线预处理工具

> 位置：`tools/jpov/tools/texture_tool/`
>
> 两个纯 CPU 命令行工具（无 GL 依赖，Linux）：
>   1. `jpov_texture_normal` — 法线贴图生成
>   2. `jpov_texture_seamless` — 周期性（seamless/tileable）化
>
> 适用场景：给「地面 / 砖 / 墙面」这类要平铺的纹理做离线预处理——
> 先去接缝（seamless），再生成配套法线贴图（normal），
> 然后配合渲染端的 GL_REPEAT + MipMap 选项做无缝周期平铺。

---

## 构建

```bash
bazel build //tools/jpov/tools/texture_tool:jpov_texture_normal
bazel build //tools/jpov/tools/texture_tool:jpov_texture_seamless
```

产物在 `bazel-bin/tools/jpov/tools/texture_tool/`。

---

## 1. 法线贴图生成 `jpov_texture_normal`

```bash
jpov_texture_normal <input.png> <output.png> [normal_scale]
```

- 把输入图按**灰度高度场**处理，用 **Sobel 算子**估计每个像素的梯度，
  再构造切线空间法线：
  ```
  gx, gy = Sobel 水平/垂直梯度
  normal = normalize(-gx*scale, -gy*scale, 1)
  RGBA 写入: (n+1)/2  →  把 [-1,1] 映射到 [0,1]
  ```
- 输出 8-bit RGB PNG，像素值 `(128,128,255)` = 无扰动（垂直朝上）。
- 采样用 **周期 wrap**，所以从 seamless 图生成的法线也能无缝平铺。

### 参数
| 参数 | 说明 | 默认 |
|------|------|------|
| `normal_scale` | 扰动强度（对应 PBR 的 normal_scale）。越大越陡、光影越强；越小越平。砖/地面类 1~4 表现自然 | `2.0` |

### 示例
```bash
jpov_texture_normal ground_tile.png ground_tile_normal.png 2.5
```

### 建议流程
从**已 seamless 化**的图生成法线，保证法线也周期无缝：
```bash
jpov_texture_seamless raw.png seamless.png
jpov_texture_normal   seamless.png seamless_normal.png
```

---

## 2. 周期性（seamless）化 `jpov_texture_seamless`

```bash
jpov_texture_seamless <input.png> <output.png> [blend_width] [mode]
```

- 对每个像素取关于图像中心 **180° 对称的对侧位置**，按其离最近边缘的
  距离加权混合，消除接缝，使 `GL_REPEAT` 平铺时看不出拼接线：
  ```
  t = clamp(min(左,右,上,下 距离) / blend_width, 0, 1)
  out = cur * t + opposite(180°对侧) * (1 - t)
  ```
  靠边处（t→0）取对侧、中间（t→1）保留原值。
- 输出与输入**同通道数**（灰度 / RGB / RGBA 都支持）。

### 参数
| 参数 | 说明 | 默认 |
|------|------|------|
| `blend_width` | 混合带宽度（像素）。越大接缝越柔，但边缘原始细节损失越多 | `min(w,h)/4` |
| `mode` | `blend`（加权混合，柔和）\| `mirror`（镜像采样，保真度更高） | `blend` |

### 示例
```bash
jpov_texture_seamless ground.png ground_seamless.png
jpov_texture_seamless ground.png ground_seamless_hard.png 32 mirror
```

---

## 与渲染端 GL_REPEAT / MipMap 配合

纹理处理好后，在 JPOV 里用 `TextureManager`/`RegisterTexture` 的采样选项加载：

```cpp
jpov::TextureOptions opts;
opts.mipmap = true;   // 生成 mipmap（三线性），大透视平面防摩尔纹/闪烁
opts.repeat = true;   // GL_REPEAT wrap，UV 超 [0,1] 周期性平铺
uint32_t base   = app.RegisterTexture("grass_seamless.png", opts);
uint32_t normal = app.RegisterTexture("grass_normal.png",   opts);
```

- **`repeat=false`（默认）**：`GL_CLAMP_TO_EDGE`，UV 超 [0,1] 糊边缘色。
- **`mipmap=false`（默认）**：单 mip `GL_LINEAR`，无 LOD 递减。
- 两者默认都关，保持既有行为；平铺地面/墙面时再开。

参考 gold test：`//tools/jpov/test/object3d:jpov_repeated_texture_gold_test`
（10×10 平板 + UV 0~10 重复 + grass 纹理 repeat+mipmap+normal）。
