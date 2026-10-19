# 粒子火的旋转与火星层

## 1. 自旋（rotation）

`EmitterConfig` 新增两个字段（`src/particles/particle_system.h`）：

| 字段 | 含义 |
|---|---|
| `RangeF rotation_speed` | 出生自旋角速度（弧度/秒，可负） |
| `float rotation_speed_decay` | 自旋随寿命衰减：0=匀速，1=寿命末端降到 0 |

模拟内核在 `Update()` 里积分：

```cpp
const float t = age / lifetime;
const float spin = spin0 * (1 - rotation_speed_decay * t);
rotation += spin * dt;
```

`ParticleInstance.rotation` 输出的是**累积后的当前角**（不再是出生角），
渲染器在 VS 里用它旋转广告牌的局部基向量。

火焰配置（`particle_viewer_app.h::MakeFireConfig`）：

```cpp
c.rotation = {0.0f, 6.2831853f};   // 出生随机朝向
c.rotation_speed = {-1.4f, 1.4f};  // 随机自旋
c.rotation_speed_decay = 0.85f;    // 顶部不乱转
```

单测：`ParticleSystemTest.SpinAdvancesRotationAndDecays`（验证 0.5s/1.0s 两个时刻的
角度精确值，含衰减项）。

## 2. 火星 / 余烬（embers）

一层简单的发光小点，独立于火焰和烟：

- 发射：锥形，位于火焰上方（跟随燃料块位置），`rate = 数量 / 平均寿命`
- 寿命 `0.6~1.4 s`、初速 `0.25~0.70 m/s`、浮力 `+0.25`、阻尼 `0.7`
- 尺寸：`size_start`（由 UI 大小控制）× 曲线 `0.6 → 1.0 → 0.3`
- 颜色：白热 → 橙 → 暗红 → 透明（alpha 两端 0）
- 渲染：`texture_id=0`（纯色）、`BlendMode::kAdditive`、`AlignMode::kView`、无 flipbook
- 颜色 RGB 可以 >1（HDR），加法模式下只增亮、不增遮蔽

面板控制：

- `火星`：开关
- `火星数量`：0~80（目标存活数）
- `火星大小`：2~40 mm（世界尺寸）

实现位置：`MakeEmberConfig()` + `ParticleViewerApp::OneIteration` 的「火星 / 余烬」块。

## 3. 验证

- `bazel test //tools/jpov/test/particles/...`：13/13
- `bazel build //tools/jpov:jpov_particle_viewer`：通过
- headless 出图：火星可见、大小合理、无方块伪影
