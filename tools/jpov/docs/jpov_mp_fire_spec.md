# Astralax《In Flames》粒子火 —— 发射逻辑提取 & 在 JPOV 的对应

素材来源：<https://astralax.com/articles/inflames>（配套 `article_fire.zip`：5 张黑底手绘火舌贴图 256²、
`Fire.avi` 参考动画、`Fire.ptc` 工程文件）。

> `Fire.ptc` 是 Magic Particles 的私有二进制（文件头 `BIU` + 版本 1.26），没有公开格式说明，
> 无法可靠解析。以下参数是从文章正文 + 参考动画实测得到的**等价重建**，也是我们离线模拟器
> `genVolume/fire_mp_sim.py` / `genVolume/fire_puff_bake.py` 实际使用的值。

---

## 1. 贴图（决定"好不好看"的第一因素）

- 256×256，**黑底**，不需要 alpha —— 用 **Intense（= Photoshop Screen / 加法）** 混合，黑=透明。
- 手绘方法：细软边笔刷 → 涂抹（Smudge）拉长火舌 → 需要时复制图层 → 高斯模糊；
  文章建议一次画 **3–6 张**，做完再删掉不合适的。
- 每张 = **一根/一簇火舌**；文章原话："最终效果主要由你画出的火舌形态决定"。

## 2. 粒子发射逻辑（文章原始设置）

| 参数 | 文章做法 | JPOV 对应 |
|---|---|---|
| 发射器 | **线发射**（角度 0°、长度 100） | `EmitterConfig::shape = kBox`（薄盒）或 line 发射 |
| 发射方向 | **36° ~ 144°**（向上扇形） | `cone_half_angle_deg ≈ 54`、`emit_dir = +Y` |
| 贴图 | 每个粒子**随机取一张**，寿命内**不换帧** | 每粒子一条命令 / 或把贴图烘进 sheet |
| 速度 | 初速度较小，方向扇形外扩 | `speed = {20, 55}` |
| 重量（重力） | **负值**：粒子先外扩后被"浮力"托上去 | `gravity = +Y`（JPOV 的 gravity 是加速度，向上即浮力） |
| 尺寸 | **随寿命由大变小**（并整体随时间变细） | `size_curve` 由 1.0 → 0.45 |
| 自旋 | 随机自旋，**自旋量随寿命衰减**（避免顶部乱转） | 逐粒子 `rotation` + 旋转速度衰减（目前 JPOV 无角速度字段） |
| 颜色 | 出生白黄 → 橙 → 暗红 → **末端黑**（黑在 Intense 下=消失） | `color_over_life` 梯度，末端 alpha/rgb → 0 |
| 可见度 | 整个生命周期 visibility 曲线 35%（压密度） | 直接调 `rate` / 每卡亮度 |

## 3. 导出成 flipbook（文章 Part III）

文章明确说：**把整段火渲染成序列帧，然后循环播放当 sprite**。环闭合的做法是
「把动画复制成两层、错位半段、上层不透明度 0→100% 交叉淡化」。

我们两种产物都做了：

- **整火 flipbook**（`mp_campfire / mp_torch / mp_hero`）：一张卡 = 一整堆火；
  适合远景/小占屏（相机绕行会露馅，因为形状烘死在卡里）。
- **单团 puff flipbook**（`mp_puff_s1/s3/s5` 单火舌、`mp_puff_c3` 三舌团）：
  一张卡 = 一根火舌 / 一个小团，靠几十张叠成火 —— 这才是粒子系统该用的粒度。

## 4. 在 JPOV 里的用法

```cpp
jpov::particles::EmitterConfig c;
c.shape = ShapeKind::kCone;           // 线/锥发射
c.cone_half_angle_deg = 54.0f;        // 36°~144°
c.rate = 120.0f;                      // 单团 puff 靠数量叠
c.lifetime = {0.70f, 1.10f};
c.gravity = {0.0f, 0.45f, 0.0f};      // 向上 = 浮力
c.use_size_over_life = true;          // 由大到小
c.size_curve.keys = {{0.0f, 1.0f}, {1.0f, 0.45f}};
c.flipbook = true; c.tiles_x = 8; c.tiles_y = 4;
c.sheet_time_mode = SheetTimeMode::kLifetime;  // 单团 sheet = 一个粒子的整个生命周期
c.sheet_cycles = 1.0f;
c.start_frame_random = false;         // kLifetime 必须从第 0 帧开始
c.align = AlignMode::kView;
c.blend = BlendMode::kAdditive;
```

## 5. 已知的引擎侧坑（重要）

加法模式下 `a = clamp(max(col.rgb),0,1) × color.a`，而亮度 = `col.rgb × a` ——
对**本身带颜色的贴图**等于多了一次**平方压暗**（`rgb²`）。所以这类贴图在引擎里会明显比单帧暗，
目前靠把 `color.rgb` 提到 **2.4** 补偿（`particle_viewer_app.h` 的 fire 梯度）。

正解仍需定夺：加法改用 `a = col.a`（教科书预乘，贴图契约 = RGB 颜色 + A 覆盖）。
