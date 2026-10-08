# lace_skirt — 薄纱蕾丝裙（透明 / cutout 样例资产）

> 来源：Tripo 单图生成（`/james_pm/red skirt/red_skirt_v4.png`，图里把"蕾丝盖住的红底"
> 刷成**蓝色标记**）→ 生成 GLB → 用「蓝色→alpha」抠出薄纱孔 → 打回带 alpha 的 baseColor。
>
> 用途：JPOV **object3d alpha test（cutout）+ 双面渲染 + 外置 glTF 加载**的渲染样例。

## 文件

| 文件 | 说明 |
|---|---|
| `lace_skirt.glb` | 原始 Tripo PBR 模型（**不透明**；`alphaMode` 缺省 OPAQUE、baseColor=JPEG、`doubleSided=true`）。 |
| `lace_skirt_cutout.gltf` (+ `.bin` + 贴图) | **外置纹理**版：`alphaMode=MASK`、`alphaCutoff=0.5`、`doubleSided=true`；baseColor 换成 RGBA PNG（蓝色区域=薄纱网 alpha）。 |
| `lace_skirt_basecolor_alpha.png` | RGBA baseColor：蕾丝不透明、原蓝色底区→细网纱（alpha-test 保留网线）。 |
| `lace_skirt_orm.jpg` / `lace_skirt_normal.png` | 对应的 ORM / 法线贴图。 |

## 关键事实（踩坑记录）

- **Tripo 资产本身没有 alpha**：`alphaMode` 缺省 OPAQUE、baseColor 是 **JPEG**（存不了 alpha）。
  所以"透明蕾丝裙"不能指望 Tripo 直出，必须**自己在纹理上合成 alpha**。
- **glTF 规范**：cutout/blend 的 alpha 一律取自 **`baseColorTexture` 的 alpha 通道**（配 `alphaMode`/`alphaCutoff`）。
- **标记法**：把"要镂空的地方"在输入图里**换一个颜色**（本例 R/B 互换 → 蓝），让它被烘进
  生成的 baseColor；再按颜色检出 → 置 alpha=0。颜色要挑与素材区分度大的。
- 纯"挖空"会显得**悬空**；本例在原蓝色区烤进一张**菱形细网**（薄纱），观感更像 tulle。

## 复现渲染

```bash
# 需要 DISPLAY（如 Xvfb :99）；从仓库根运行
cd /tmp/workdir   # 任意含 fonts/ 的目录（model viewer 需字体）
DISPLAY=:99 <repo>/bazel-bin/tools/jpov/jpov_model_viewer --four_views \
    --output_dir /tmp/out <models_dir>/samples/lace_skirt/lace_skirt_cutout.gltf
# → /tmp/out/lace_skirt_cutout_{front,left,up,perspective}.png（裙摆透空、上身实心）
```

> ⚠️ 本样例**不承载 gold test**（外部生成资产不可控）。留作 cutout/双面/外置 glTF 的**人工验收入口**。
