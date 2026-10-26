# JPOV 环境镜面（IBL specular）· 设计

> 日期：2026-10-26 ｜ 状态：**已实现（S1）** ｜ 决策人：Danis
>
> 关联：`interface/render_command.h` 的 `EnvSpecular` / `AmbientLight`、
> `src/env_specular.{h,cc}`、`src/skydome/sky_renderer.h` 的 `SkyOutput`、
> `demo/skylight_scene.h`（验收工件）。

## 0. 一句话

PBR 的镜面项原本只有**直射高光**，缺**环境镜面** —— 于是 `metallic=1` 的物体没有漫反射、
也没有环境反射，只剩太阳那一小块高光，看起来是一坨黑。本 PR 补上这一半：**把所有材质的
镜面项接到天光上**，用一个 0~1 的全局乘子控制“消费多少天光”。

## 1. 问题：那个“黑盒子”是承诺没兑现

`demo/skylight_scene.h` 的三块方块是刻意隔离变量的对照组，其中 index 2 的注释写着：

```
index 2：金属（metallic=1.0, roughness=0.15）→ 无漫反射，只反射环境
```

而着色器的最终一行此前是：

```glsl
result = ambient * base_color * ao / PI + total_diffuse + total_specular + emissive;
```

`total_specular` 只包含点光源与太阳的 GGX 项。**没有任何环境镜面项。**
所以“只反射环境”的金属块没有环境可反射 ⇒ 暗得发黑。

顺带一个已知的 cheat：`ambient * base_color * ao / PI` 没有乘 `(1 - metallic)`，
金属也吃到了一份漫反射环境光（不然纯金属在无 IBL 时是纯黑）。**本 PR 不动它**
（改它会改变现有全部观感与 gold），留作单独决策。

## 2. 光源：为什么是 SkyCommand 的 cubemap

| 候选 | 结论 |
|---|---|
| **手填一个“镜面颜色常数”** | ❌ 镜面项**是方向函数**，常数给不了方向感；光滑球会是一条死板均匀色，比现在还假 |
| **屏幕空间反射（SSR）** | ❌ 需要每像素法线/粗糙度/掩码（G-buffer），且只能反射屏幕内已渲染的东西；本阶段不做 |
| **平面镜像（planar reflection）** | ❌ 每个反射面一趟场景重绘，且只对平面成立；这是海面该用的，不是“全模型”该用的 |
| **天空 cubemap（本 PR）** | ✅ 无 G-buffer、无额外几何 pass、方向正确、天然带粗糙度模糊（mip 链） |

天空是解析 Preetham，与背景**同源同参数**（浊度/昼夜/季节/强度），
所以“看到的天空”和“被反射的天空”不可能不一致 —— 不会出现两条色温曲线分叉。

### 2.1 为什么另起 `kEnvProbe` 输出模式

天空 shader 现在有三种输出（`SkyOutput`）：

| 模式 | 内容 | 用途 |
|---|---|---|
| `kFull` | 完整天空（含日/月盘 + 光晕） | 背景垫底 |
| `kAtmoOnly` | 只含大气色；下半球也用地平线色 | 远景仰角雾的收敛色 |
| `kEnvProbe` | 只含大气色；**下半球用地色** | 环境反射探针 |

- **不含日/月盘**：日月的直射已经由 `total_specular`（GGX）给出，探针里再带一个盘 = 两倍亮。
- **下半球用地色**（而不是地平线色）：金属球的下半部应该反射地面。
  用 `kAtmoOnly` 会让下半球变成地平线色（因为那个模式为了“雾收敛”把 `dir.y` 夹到 ≥0）。

## 3. 渲染链路

### 3.1 每帧生成（`Renderer::Render`，天空块内）

```
if (cmds.sky && cmds.env_specular) {
    EnsureSkyEnvCube(128);                       // 首次分配；不随窗口尺寸重建
    SkyRenderer::DrawSkyEnvCube(*cmds.sky, 128, sky_env_tex_, sky_env_fbo_, shader_mgr_);
}
```

`DrawSkyEnvCube` 逐面构造 90° 透视相机，复用同一份 sky shader 画进 cube face，最后
`glGenerateMipmap`。面朝向按 GL cubemap 的 s/t 约定定（±X/±Z 面的“上”是 **-Y**，
+Y 是 +Z、-Y 是 -Z）；弄错会得到旋转/镜像的天空，而“错在哪”很难用语言描述。

成本：6 × 128² ≈ 10 万片元，相对一个全屏天空 pass（~200 万）不到 5%。
**未设置 `env_specular` 时零成本**（不分配、不渲染、shader 里 `uEnvEnabled=0` 直接返回 0）。

### 3.2 片元侧：分裂求和（split-sum）

```glsl
vec3 R  = reflect(-V, N);
vec3 pre = textureLod(uEnvCube, R, roughness * uEnvMaxLod).rgb;   // 粗糙度 → mip 级
vec2 AB  = EnvBrdfApprox(roughness, NoV);                          // Karis 解析拟合，免 LUT
vec3 F0  = mix(vec3(0.04), base_color, metallic);
result  += pre * (F0 * AB.x + AB.y) * uEnvSkyScale * ao;
```

- **GLSL 只有一份**：`src/env_specular.h` 的 `kEnvSpecularGlsl`，三份 PBR 片元
  （object3d / instanced / skinned）用 `std::string` 拼接注入。
  这修掉了一个既有风险：这三份 FS 本来就是各自独立的拷贝，再各抄一遍环境镜面必然分叉。
- **uniform 上传只有一份**：`UploadEnvSpecular`。每帧都必须调用（uniform 是 program 状态，
  上一帧开着而本帧不传 → 继续用旧的、可能已销毁的 cubemap）。
- `ao` 乘到镜面上：一阶近似（严格做法是独立的镜面遮蔽项）。与 `ambient * ao` 同一口径。

## 4. 接口

```cpp
struct EnvSpecular {
    float sky_scale = 1.0f;   // 天光消费量 [0,1]；1 = 全部消费天光（物理值）
};
// RenderCommandList:
std::optional<EnvSpecular> env_specular;   // 未设置 = 关闭，零开销零回归
```

- **全局，不是 per-object**。per-obstacle 的“这个物体能看见多少天光”（墙角的金属该暗、
  开阔处的该亮）计划由更强的**体素光场**接管，本版刻意不做。
- **必须有 `cmds.sky`**：环境镜面的光源就是天光。缺失时 `LOG(FATAL)` 而不是静默退化 ——
  静默会让“为什么金属是黑的”重新变成一个要查代码才能回答的问题。
- `sky_scale` 出 [0,1] 直接 `CHECK` 失败（不做静默 clamp）。

## 5. 已知限制（写清楚，别当成 bug）

1. **没有遮挡**：这是全局乘子，不考虑“这个角落看不见天空”。渲染上是“所有镜面拿同一份天空”。
   ⇒ 体素光场 / per-object 可见性是后续工作。
2. **粗糙度→mip 是近似**：真实做法要按 GGX 做重要性采样的预滤波（PMREM）。
   天空是极低频渐变，两级 mip 的差别肉眼不可见，故不做。
3. **探针只含天空与地色**，不含任何几何（没有树/山的倒影）。那是平面反射/SSR 的事。
4. **金属仍吃到漫反射 ambient**（§1 的 cheat 未动）。
5. 128² 面边长的立方体在 `roughness→0` 时可能出现极轻微的立方体接缝
   （故 shader 里粗糙度下限取 0.02）。

## 6. 验收

交互（需 DISPLAY）：

```
./tools/jpov/build_jpov_skylight_viewer.sh
output/jpov_skylight_viewer/jpov_skylight_viewer
```
面板左下新增两项：**「环境镜面 (天光反射)」开关** + **「天光消费量 [0,1]」滑条**（默认开、1.0）。
验收工件 = 右侧第三方块（金属）：关 = 暗灰块；开 = 能看见天光渐变被映上去，上半部偏天蓝、
下半部偏地色。

headless A/B（交互/自动化同一套渲染体）：

```
output/jpov_skylight_viewer/jpov_skylight_viewer --capture-env <out_dir>
```
出 `env_off.png` / `env_half.png` / `env_on.png`，只切乘子（0 / 0.5 / 1）。
半量应落在两者之间 —— 这是“乘子真的连续”的证据。

## 7. 未做（按优先级）

1. **屏幕空间 AO（HBAO）** —— 这才是“整体压环境光”的正确载体（8~16 采样、可从深度求导法线、
   单帧确定性）。接口已经留好：`result` 里的 `ao` 只需改成 `ao_tex * ao_screen`。
2. **水面/地板的平面反射** —— 单平面精确，比 SSR 准，成本是一趟低分辨率场景重绘。
3. **通用 SSR** —— 要 G-buffer 法线，动 11 个 shader + 全套 gold，收益最小。
4. **per-object 天光可见性（体素光场）** —— Danis 已指定方向，替换掉本版的全局乘子。
