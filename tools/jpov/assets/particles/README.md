# particles/ —— 粒子 sprite 素材

## ember_soft_dot.png

火星/余烬用的软圆点 sprite（128×128，RGBA，白色 RGB + alpha 形状）。

**为什么不用硬边 quad**：`texture_id = 0` 时面片是一块纯色矩形，缩小到屏幕上几个像素后
边缘是硬切的，看起来像"纸片"；换成软边 sprite 才有「亮核 + 光晕」的火星感。

**形状**（`r` = 到中心的归一化距离，0 = 中心、1 = 贴图边缘）：

```
alpha = clamp(0.95·exp(-(r/0.16)²) + 0.35·exp(-(r/0.42)²), 0, 1)
alpha *= clamp((1 - r)/0.12, 0, 1)      # 外圈收到 0，避免出现方形边界
```

- 第一项 = 亮核（高斯，σ = 0.16），第二项 = 光晕（σ = 0.42）。
- 贴图只提供**形状**，颜色来自粒子自身的生命期渐变（温度调色），所以这里是白色 RGB。

**生成脚本**（一次性，未入库；要改形状就照这个公式重画）：

```python
from PIL import Image
import math

S = 128
c = (S - 1) / 2.0
img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
px = img.load()
for y in range(S):
    for x in range(S):
        r = math.hypot((x - c) / (S * 0.5), (y - c) / (S * 0.5))
        a = 0.95 * math.exp(-((r / 0.16) ** 2)) + 0.35 * math.exp(-((r / 0.42) ** 2))
        a = min(1.0, max(0.0, a))
        a *= max(0.0, min(1.0, (1.0 - r) / 0.12))
        px[x, y] = (255, 255, 255, int(round(a * 255)))
img.save("ember_soft_dot.png")
```

**使用**：粒子只有几像素，注册时必须开 mipmap（`TextureOptions::mipmap = true`），
否则会闪成噪点。见 `demo/particle_viewer_app.h`（`kEmberSpriteFile`）与
`test/particles/particle_gold.cc`。
