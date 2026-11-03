# sp_ui 整键按钮图集 —— 金属扫光 flipbook

> 位置：`tools/jpov/assets/sp_ui/button/`
> 消费者：`sp_ui` 的 `SpButtonSkin::Style::kFlipbook`（整块按钮 = 这张图集）+ `jpov_sp_ui_demo`

---

## 产物

| 文件 | 内容 |
|------|------|
| `make_button_flipbook.py` | **生成脚本**（纯 numpy/PIL，无外部素材、无随机种子 ⇒ 可复现） |
| `plate_button_steel_4x4.png` | 最终图集：4×4、每格 380×96、共 16 帧、1520×384 |

帧序遵守 `interface/flipbook.h`：**行主序、帧 0 在图片左上角**，
且帧 N 与帧 0 首尾相接（相位走满整圈，`t = N/fps` 与 `t = 0` 逐像素相同）。

---

## 复现

```bash
cd tools/jpov/assets/sp_ui/button
python3 make_button_flipbook.py --out plate_button_steel_4x4.png
# 换尺寸（比如按钮是别的长宽比时，让格子比例与按钮一致，别让图被拉伸）：
python3 make_button_flipbook.py --out plate_button_steel_4x4.png \
  --tiles 4 --cell-w 380 --cell-h 96
```

---

## 为什么是这几个参数

- **格子比例 = 按钮比例**。demo 里整键按钮是 173×44（左列三等分），
  所以格子取 380×96（≈3.95:1）。比例不一致时图会被非等比拉伸，倒角会变形。
- **`material_px` 必须保持 0**：整键皮是“一整块背景”，不该被平铺；
  0 表示一格图正好铺满一颗按钮（`tiling = 1`）。
- **拉丝频率要粗**。图集格 380 宽、实际按钮 173 宽（缩 2.2 倍），
  20px 以下的丝纹缩小后会被双线性抹平成平滑渐变（试过，肉眼完全看不见）。
- **扫光带中心从 −0.20 走到 1.20**（两端都在画面外）⇒ 循环时“带子在画面外换位”，
  看不到跳变。用 `cos(phase)` 那类写法会在两端堆成角落光团，且首尾帧不接。

## 造型

- 冷灰拉丝金属 + 边缘倒角（外 3px 压暗 / 内 2px 提亮，纯几何、不随时间变）
- 一条斜向高光带 16 帧扫过一圈
- 边缘冷蓝呼吸光（同相位、无缝）
- filmic 软限幅，避免高光爆白

**没有用任何水波/涟漪/位移场** —— 这里是机械表面，不是流体。

## tint 是唯一的状态语言

图集本身**不含 hover/pressed 的版本**：三种状态全靠 `SpButtonSkin` 的
`normal_tint / hover_tint / pressed_tint` 乘在这张图上。所以换状态不会换图、
不会跳帧，只是同一块金属亮起来/沉下去。

demo 的取值（`demo/sp_ui_demo_panel.h`）：

| 状态 | tint | 说明 |
|---|---|---|
| normal | 0.62 | 压暗，但拉丝/倒角仍看得见 |
| hover | 1.06 / 1.00 / 0.90 | 回到本色并略偏暖，亮到 1.0 就打住（再高白字会糊） |
| pressed | 0.30 | 沉下去 |
