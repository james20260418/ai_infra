# morph samples — FLOW / PROFILE 形变样例

`morph_util.py` 的两个算法（FLOW / PROFILE）的**样例输入、输出**，用于肉眼验收与回归对照。

## 文件

| 文件 | 说明 |
|---|---|
| `in_f1_mask.png` | 输入 A：F1 火舌遮罩（256×256，alpha=形状） |
| `in_f5_mask.png` | 输入 B：F5 火舌遮罩（256×256） |
| `out_flow_32f_8x4.png` | FLOW 结果：32 帧，8×4 atlas，2048×1024 |
| `out_profile_32f_8x4.png` | PROFILE 结果：同上 |
| `preview_flow_5f.png` | FLOW 抽样帧（0 / 8 / 16 / 24 / 31） |
| `preview_profile_5f.png` | PROFILE 抽样帧 |

> 素材来源：Astralax《In Flames》文章附带的**手绘贴图**，仅用于技术复现/对标，不可直接商用。
> 要商用请自绘等价贴图（形状与配色都需替换）。

## 复现

```bash
cd tools/jpov/genVolume
python3 morph_util.py --method flow \
    --a samples/morph/in_f1_mask.png --b samples/morph/in_f5_mask.png \
    --frames 32 --out /tmp/flow_8x4.png
```

## 接口速查

```python
morph_frames(img_a, img_b, n_frames=32, method="flow")  # -> [H,W] float32 帧列表
make_flipbook_sheet(frames, tiles_x=8, flip_rows=True)  # -> uint8 RGBA atlas
```

- 输入：`(H,W)` 单通道 float32 `[0,1]`（uint8 会自动 /255），两张同尺寸。
- `t = i / (n_frames - 1)`：**首帧 = A，末帧 = B**（单向形变）。
- 端点逐字节一致（`t=0/1` 直接返回原图）。
- 帧数建议 `24~32`；30 fps 下 32 帧 ≈ 1.07 s，覆盖常见火苗寿命 `0.7~1.1 s`。
- 播放帧率 `fps = n_frames / lifetime_seconds`；慢放请降 fps，不要改 t 的分布。
- `make_flipbook_sheet(..., flip_rows=True)` 按 JPOV `CellUV` 的行序（`vrow=tilesY-1-row`）写：
  **frame 0 在 PNG 的最后一行**；导出普通序列帧用 `flip_rows=False`。
