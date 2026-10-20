# 粒子火 · 管线 buff（朝向 / 真加性累积 / 火光照明 / 辉光）

> 一轮把「贴纸火」推向「发光火盆」的开关。除「朝向」外全部默认关闭，
> 默认关闭时与旧行为逐字节一致（gold 已锁）。

## 1. 朝向自适应（`AlignMode::kViewVerticalBlend`）

**治什么**：`kVertical`（roll 锁定）侧视最像火，但正俯视时竖直卡被看成**一条扁线**；
`kView` 俯视正常，但侧视时卡片随相机倾角翻转、火像"趴着"。

做法：按 `t = |dot(normalize(toCam), world_up)|` 在两组基向量间插值。

| t | 含义 | 结果 |
|---|---|---|
| 0 | 平视 | ≈ `kVertical` |
| 1 | 正俯视 | ≈ `kView` |

```glsl
float w = smoothstep(0.55, 0.95, t);              // 俯视 25° 内才开始切换
right = normalize(mix(r_vertical, uCamRight, w));
up    = normalize(mix(u_vertical,  uCamUp,   w));
up    = normalize(up - dot(up, right) * right);   // 正交化，防剪切
```

- 切换区间收在俯视 25° 内：过渡区基向量非刚性，范围大会在中间角度看到卡片"晃"。
- 插值后必须**重新正交化** `up`（减掉 `right` 分量），否则近过渡区卡片会被剪切拉斜。
- viewer 面板：`朝向` 下拉（view / vertical / facing / 自适应 blend），火/烟两层共用。

## 2. 发光体求和 / 吸收体求覆盖（`ParticleRenderCommand::emissive`，默认 true）

### 2.1 两种累积的数学

设某像素被 N 张粒子片覆盖，第 k 张贡献预乘色 `c_k = rgb_k·α_k` 与覆盖率 `α_k`。

**A. 覆盖集（旧路径，WBO）** —— 加权平均 + 覆盖率：

```
A   = Σ w_k·α_k                 （权重和）
C   = Σ w_k·c_k / A             （颜色的加权平均）
cov = 1 − Π(1 − α_k)            （「至少被一层覆盖」的概率，与顺序无关）
out = C·cov + bg·(1 − cov)      （预乘 over）
```

关键：**颜色是平均，不是求和。** 再叠 10 层同色片，`C` 不变，只是 `cov→1`。
所以火怎么叠都不会更亮，只会更"实"。

**B. 加法集（新路径）**：

```
out = bg + Σ c_k                （直接求和，不碰覆盖率）
```

同样是 10 层，亮度线性涨 10 倍（HDR 里真的 ×10，tone map 前）。

### 2.2 为什么观感差别是「核心更亮 + 暗部透背景」

| 现象 | 覆盖集 | 加法集 |
|---|---|---|
| 单层、α=0.5、背景亮度 `bg=0.05` | `0.5·c + 0.05·0.5` | `0.05 + 0.5·c` |
| 差 | — | **多出 `0.025`（= bg·α）**，即背景不再被压暗 |
| 多层重叠 | `C` 不变（平均） | `Σ c_k` 线性累加 → 爆亮 |

所以两件事同时发生：**背景不被遮挡（暗部透出来）** + **重叠区亮度线性叠加（核心过曝）**。
"核心过曝"不是 bug，是求和的必然结果；要压就压每层的 `c_k`（发光增益），不是改累积方式。

### 2.3 实现要点

| 附件 | 覆盖集（kAlpha） | 真加法 |
|---|---|---|
| att0 accum | `vec4(c·w, α·w)` | `vec4(0)`（恒等） |
| att1 reveal | `α` | `0`（`ZERO,ONE_MINUS_SRC_COLOR` ⇒ dst×1，恒等） |
| att2 add | `vec4(0)` | `vec4(c)`（`ONE,ONE` 求和） |

- 合成跑两趟：覆盖集 over → 加法集 `ONE,ONE`。
- **与 blend 模式无关**：遮罩火（kAlpha，RGB=白/A=形状）的贡献同样是 `rgb·α`，
  `emissive=true` 就变成"发光求和"。第一版只在 `blend==kAdditive` 时生效，
  导致默认的 5 套遮罩火拨开关无反应（验收踩到）。
- **没有开关了**（2026-10-20 定）：`emissive` 默认 **true**——发光体求和才物理；
  "不叠加"反而是错的。画**吸收介质（烟/尘/雾）必须显式置 false**，否则烟会变成
  "不遮挡背景的发光霾"。viewer 里火/火星用默认，烟显式 false。
- 已知近似：加法光**不被覆盖集衰减**（烟雾罩在火前面的情况）——单层 OIT 的固有局限。

## 3. 火光照明（viewer 侧，跟随火焰的点光源）

粒子本身不参与光照，但**场景要被火照亮**才有火盆感：

```cpp
light.position = {燃料块 x, 燃料块顶部 + 0.12, 燃料块 z};
light.color = {1.0, 0.52, 0.18};   // 暖橙
light.linear_radius = 2.5f;
light.physical_radius = 0.06f;     // 代表点半径 → 高光柔化
light.intensity = 强度 × 闪烁;
```

闪烁 = 两个正弦叠加（`sin(6.1t)` + `sin(17.3t)`）给出 ±8%，位置再微抖 ±1cm ——
周期正弦肉眼看不出"机械"，但足以避免"死灯"。

## 4. 辉光（`RenderCommandList::bloom`）—— 以及修掉的两个 bug

引擎已有完整链（prefilter → 逐级降采样 → 上采样累加 → tone map 前叠回）。

### 4.1 bug 1：上采样「视口 vs 纹理」错配（整屏拉丝）

- 乒乓 acc 缓冲按**半分辨率**分配，但循环里对 1/4、1/8 级用
  `glViewport(0,0,level_w,level_h)` 往**同一张半分辨率纹理**里画 → 只有左下角一块有内容，
  其余是旧数据/未初始化。
- 上采样又按**整张 UV [0,1]** 读 ⇒ 把"缩在角落的小图 + 大片脏数据"当成整张辉光 ⇒ 整屏拉丝。

修法：每级只在自己的纹理、自己的视口整幅读写，累加交给 blend：

```
for (i = n-2; i >= 0; --i) {
    bind FBO(L_i), viewport = size(L_i);
    enable blend (ONE, ONE);
    draw fullscreen sampling L_{i+1}      // 双线性放大
}
bloom_final = L_0
```

乒乓 acc 缓冲整体删除。整屏 5.5 万像素污渍 → 光晕只落在火焰周围 3.2 千像素。

### 4.2 bug 2：pass 没还原 GL 状态 → UI 文字糊成白块

`DrawBloomPass` 开头 `glDisable(GL_BLEND)` 之后**从未恢复**。而它后面的 tone map、SMAA
和**所有 2D/UI 绘制**都依赖 `GL_BLEND` 已开：字形是"alpha 贴图 × 颜色"靠混合写进去的，
混合一关，alpha 被忽略 ⇒ **每个字形四边形整块写成不透明纯白** ⇒ 面板糊成一坨白块
（验收时被描述为"UI 也被辉光了"）。

修法：整段 `glPushAttrib(GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT)`
+ 结尾 `glPopAttrib()`，把 enable / blend / depth-mask 全部还原。

> 教训：后处理 pass 只要碰了全局 GL 状态，就必须成对保存/还原；"纯后处理所以很简单"
> 恰恰容易漏在这一步。

### 4.3 逻辑复检（2026-10-20 第二轮）

按「纯后处理能有什么 bug」的追问把整条链又读了一遍，除上面两个，还修了三处：

| 位置 | 问题 | 处理 |
|---|---|---|
| 合成 | 上采样是**纯相加**，`n` 级各贡献一次 ⇒ 总能量 ∝ 级数，改 `levels` 等于偷改亮度 | 合成时乘 `1/levels` 归一化，`intensity` 才是稳定旋钮 |
| 入口 | `intensity <= 0` 仍然跑完整条链（含各级 FBO 分配与 5+ 次全屏 pass） | 直接透传原 HDR，零开销 |
| 销毁 | `bloom_final_tex_` 指向 `bloom_texs_[0]`，重建链时没清零（悬空 GLuint） | `DestroyBloomChain` 里置 0 |

其余复检项（都没问题）：prefilter 阈值在 tone map 前对线性亮度做；降采样每级按**自己的**
尺寸取 `uTexelSize`；上采样只在「本级 vs 下一级」两纹理间读写，无 feedback；各级每帧
整幅重写，不需要额外 clear；视口尺寸与纹理尺寸在每一级都一致（这正是 bug 1 的根因）。

### 4.4 参数

粒子火的 HDR 亮度上限 ≈1.8，乘覆盖率后大多 <1.0，**门槛用默认 1.0 几乎不出光晕**，
所以默认 门槛 0.6 / 强度 0.8（面板可调）。

## 5. A/B 复现（headless）

```bash
./jpov_particle_viewer --capture /tmp/a --fire 0 --align 3 --cam-phi-deg 45
./jpov_particle_viewer --capture /tmp/b --fire 0   # 发光求和已是默认
./jpov_particle_viewer --capture /tmp/c --fire 0 --fire-light 1 --fire-light-intensity 8
./jpov_particle_viewer --capture /tmp/d --fire 0 --bloom 1 --bloom-threshold 0.6
```

`--cam-phi-deg` 用来换相机仰角 —— 「朝向自适应」只有换视角才看得出来。

## 6. 顺带删掉的

- `SortMode` / `ParticleRenderCommand::sort`：**惰性字段**，renderer 从来没读过（WBO 顺序无关）。
- UV 噪声扰动（曾做过一版）：验收判定「副作用大于收益」——我们现有图集 alpha 本来就软，
  它的价值只在交叉卡/重复卡去同步，等真做交叉卡时再说。代码已整块移除。
- 单片静态火调试视图：随 UV 一起移除。
- 「真加性累积」开关：改成默认行为（`emissive`），UI/CLI 开关都删了。
