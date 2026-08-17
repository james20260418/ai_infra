# NEXT_PR: 天光（梯度半球 SkyLight）+ 太阳平行光（带 Shadow）

> 目标分支：`scene-assets-ibl`
> 生产方式：gold test 开道，分步走，每步跑通再进下一步。
> 背景：PR #50（点光源场景基线）遗留——纯点光源无环境反射，金属哑光；本系列接上"天光/IBL"。
> Danis 工作流：**每完成一步，Danis 要看代码理解，再进下一步。别一口气做满。**

## 已完成（工作区未提交）

### SkyLight 改为 std::optional
- `render_command.h`：`std::vector<SkyLight> sky_lights` → `std::optional<SkyLight> sky_light`（配合 `#include <optional>`）。
- 理由（Danis 拍板/探究后）："季节/早晚切换"是时域单天光插值，不需要 vector 多探针；`optional` 更贴"全局单光可开关"的现状（类型即文档）。

### 太阳平行光 + Shadow Map（第 2 步，2026-08-17 完成）
- `DirectionalLight`（direction/color/intensity）+ `std::optional<DirectionalLight> sun`。
- 正交 shadow pass：`Renderer::DrawShadowPass`（光空间 lookAt + ortho，包住场景质心，near=dist-half/far=dist+half），渲到 `shadow_fbo_` + `shadow_depth_tex_`（1024²）。
- 深度专用 shader `kShadowVs`/`kShadowFs`；PBR frag 加 `shadowFactor`（3×3 PCF + bias0.003）+ 太阳直射 GGX（diffuse+specular）× shadow。
- `Object3DRenderer::DrawObject3DShadow` + `UploadSunData`（sun + shadow 纹理 uniform 上传两 program）。
- 场景：`LightMode::kPointLights`（旧 baseline，3 点光源，默认）/ `kSun`（太阳+天光 ambient+影子）。
- 新 gold：`sun_scene_1280x720.png`（`jpov_sun_scene_gold_*`），与旧 `scene_1280x720.png`（3 点光源）并排对比。
- **sun intensity 调参**：初始 2.4 太暗（max≈97）；相机 near/far 初始 (-20,20) 把场景（距光 30）压到 far 平面，导致亮度/阴影被压制——改为 dist=40, near=20, far=60 后正常；intensity 提到 **10.0** 得到清晰影子 + 良好曝光（无 acne/无 peter-panning）。
- **shadow acne 修复**：地面 25 个共面 quad 接缝处深度突变 → 常量 bias 导致"中间自阴影发黑 + 边缘漏光亮边"。改为 **slope-scaled bias**（`bias*(1-dot(N,L))`，掠射角越大偏置越大）。
- **z-up 坐标统一（Danis 拍板，2026-08-17）**：scene 历史遗留 y-up → 全场景+相机绕 x+90° 旋转成 **z 正方向朝上**（y→z, z→-y）。点光源 baseline 刚性旋转后画面不变（mean 62.1 vs 原 62.2）。
- **🔥 影子方向根因（2026-08-17 定位）**：`DrawShadowPass` 里阴影相机 `world_up` 硬编码 **(0,1,0)（y-up）**，与 z-up 世界不符 → 阴影相机横滚错位，导致水平(x/y)方向影子落向**翻转**而 z 方向近似正常（即 Danis 观察的"x,y 反号, z 正确"）。修复为 `world_up=(0,0,1)` + 防退化（太阳近正上方时回退 y 做参考 up）。太阳 direction 语义=光传播方向，shader 用 `L=normalize(-uSunDir)`，非纹理/UV 问题。
- 全部 8 个 object3d 测试通过（含新 sun 测试），点光源场景无回归。

### 遗留（未做）
- 第 3 步 specular IBL（预滤波 env + BRDF LUT，让金属侧脸也有环境反射）。
- 连续时间轴季节/早晚插值模型。
- 阴影软度：当前 PCF 3×3 + LINEAR，边缘偏硬，可后续调大 PCF 或加较深半影。

## 分步计划（原始）

### 第 1 步：点光源 → 梯度半球天光（SkyLight）
- 把 `jpov_scene_common.h` 里 scene 的 **3 个点光源去掉**，换成 **梯度半球天光**。
- `SkyLight` config **独立成 RenderCommand 里的一个单独结构体**，不塞进现有 PointLight。
- 更新 scene gold test：gold 开道，验证半球天光插值正确。

### 第 2 步（已完成）：太阳影子
- 梯度半球基础上，加太阳平行光（DirectionalLight）+ 阴影贴图（Shadow Map）。
- 关键架构注意：需要第 2 个 FBO + 正交 shadow pass，牵动现有单-FBO tile culling，要小心别破坏现有 point light 路径与既有 gold tests。

### 第 3 步（很可能后续单独 PR）：specular IBL
- 接回 PR #50 遗留：预滤波环境贴图 + BRDF LUT，让金属恢复高光（这才是"天光让金属亮"的正主）。


---

## 第 1 步详细设计（本次实现）

### 1. `interface/render_command.h` 新增 `SkyLight` 结构体

独立结构体，**不复用 PointLight**（语义不同，避免歧义）：

```cpp
// 梯度半球天光（Hemispheric Sky Light）。
// 一个极廉价的"天光"近似：把天空切成上/下两个半球，
// 各给一个颜色，按片元法线朝上程度线性插值。
//   factor = clamp(N.y * 0.5 + 0.5, 0.0, 1.0)   // 朝上→1，朝下→0
//   ambient = mix(ground_color, sky_color, factor) * intensity
// 成本：一次 mix，0 额外纹理采样 / 0 额外 draw call。
struct SkyLight {
    Color sky_color;     // 天顶方向颜色（随季节/早晚变，如昼蓝/夕橙）
    Color ground_color;  // 地面反射颜色（随季节变，如春绿/冬雪白）
    float intensity = 0.4f;  // 整体亮度标量（黑白天：夜 0.05 / 昼 1.0）
};
```

- 季节/黑白天：由调用方换 `sky_color / ground_color / intensity` 三参数实现，shader 只做 mix，**无额外插值模型**（连续时间轴插值留给后续，Danis 本步只要"4 组预设切换"验证插值）。

### 2. `RenderCommandList` 加字段

在 point_lights 附近加（不删除现有 point_lights，向后兼容既有 point light gold tests）：

```cpp
// 全局天光。当前实现为梯度半球近似；为空/未设置时退回旧常量 ambient
// （AMBIENT_COLOR=白, AMBIENT_STRENGTH=0.4）以保持既有 gold test 不回归。
std::vector<SkyLight> sky_lights;
```

用 vector 是为了将来能扩展多个天光光探针；本步场景只放 1 个，shader 只取首个或全部累加。

### 3. PBR shader（`object3d_renderer.h` kMeshFs3dPBR）

- 新增 `uniform uSkyColors[2]`（pack 2 个 vec3）`+ uSkyIntensity`（或用数组 `uSkyLights[]` 简化为 1 个）。为最小改动，用 2 个 uniform 即可。
- ambient 计算从：
  ```glsl
  const vec3 AMBIENT_COLOR = vec3(1.0); const float AMBIENT_STRENGTH = 0.4;
  vec3 ambient = AMBIENT_COLOR * AMBIENT_STRENGTH;
  ```
  改为：
  ```glsl
  // 梯度半球：法线朝上取 sky，朝下取 ground
  float up = clamp(N.y * 0.5 + 0.5, 0.0, 1.0);
  vec3 ambient = mix(uGroundColor, uSkyColor, up) * uSkyIntensity;
  ```
- `ambient * base_color * ao` 不变。

### 4. 场景（`jpov_scene_common.h`）与 gold test

- scene 构建里：**去掉 `point_lights`（3 个点光源）**，改为 push 1 个 `SkyLight`。
- 为了让第 1 步可观测"梯度生效"，场景材质偏中性（家具/地面 base color 已提亮 3-4x），天光用**有对比的上下半球**（如 sky=蓝 `(0.5,0.75,1.0)`、ground=暗绿 `(0.15,0.3,0.2)`），让"顶面亮、底面/侧面暗"肉眼可见。
- `jpov_scene_gold_generator.cc`：重写 gold image `scene_1280x720.png`（半球天光版）。
- `jpov_scene_gold_test.cc`：gold 存在 + smoke check（沿用 PR #50 决策：llvmpipe PBR 三稳态非确定，跳过像素级颜色比对）。

### 5. 构建验证目标（本步结束的标准）

- `jpov_scene_gold_test` PASS（新 gold）。
- 既有 point light gold tests 不回归（`jpov_pbr_cube_normal_gold_test` 等仍走旧常量 ambient → 用 `sky_lights` 为空回退逻辑保证）。
- 构建全绿。

### 遗留（本步明确不做）
- 太阳平行光 + 阴影（第 2 步）。
- specular IBL / BRDF LUT / 预滤波 env（第 3 步或后续）。
- 连续时间轴季节/早晚插值模型（Danis 后续再定）。
- sky_lights 多探针累加（向量已预留字段，shader 暂只处理首个/单值）。

### 风险与注意
- **向后兼容**：sky_lights 为空时 PBR 必须退回旧常量 ambient，否则 3 个既有 point light gold test 全崩。这是本步最重要的回归红线。
- **uniform 上传**：Object3DRenderer 的光源上传逻辑（`UploadLightData` 那套 + tile culling）暂时不动；SkyLight 是全局的，不进 tile，走独立 uniform 上传路径，避免和现有 255-light tile 机制纠缠。
