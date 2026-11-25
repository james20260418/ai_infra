// JPOV 天光查看器（skylight viewer）— 渲染核心 App（场景渲染体 + 交互面板宿主）
//
// 与 model viewer 的 viewer_app.h 同构（同一套 JPOV::OneIteration 渲染体 +
// 即时模式 UI 面板约定），差别只在**场景与光照来源**：
//   - 场景 = 三方块（低反/高光/金属）+ 灰色地面（skylight_scene.h），不加载 glTF；
//   - 天光 = `CreateDefaultSkyCommand`（其余参数走默认构造；月亮恒取 moon_dir = −sun_dir）；
//   - 光照 = 由该 SkyCommand **推导**（主平行光 + ambient 色/强），不手配。
//
// 交互面板：五个天光自由度（六个滑条）+ 三色环境光开关 + SMAA 开关/阈值 +
// 环境镜面开关/天光消费量：
//   ① 浊度 turb [0,8]
//   ② 季节色温 日光 [−1,+1]（左=蓝偏 / 右=红偏；只染太阳能通道）
//   ③ 天体方向：仰角 [−90,+90] + 方位角 [0,360)（两个滑条，属同一个自由度）
//      仰角正 = 太阳在地平线上（白天，太阳平行光）；负 = 太阳沉下、月亮升到反向
//      等高（夜间，月亮平行光）。月亮方向恒取 moon_dir = −sun_dir，不单独给滑条。
//   ④ 月色变红 [0,1]（0=常月，1=血月；只染月盘 + 月光）
//   ⑤ 夜空偏蓝 [0,1]（0=出厂夜色，1=梦幻蓝且更亮；夜色两色整体乘子）
//   ⑥（开关）三色环境光：开 = ambient 按法线仰角在 [天, 天际线, 地] 间插值（AmbientTricolor），
//      关 = 原单色 ambient（AmbientColor）。供肉眼对比两种环境光。
//   ⑦（开关 + 滑条）环境镜面（IBL specular）：开 = 所有材质从天空 cubemap 取镜面反射，
//      滑条 = 天光消费量 [0,1]（见 interface/render_command.h 的 EnvSpecular）。
//      验收工件 = 第三方块（金属，metallic=1）—— 关时是发黑的块，开时能看见天光/地色被映上去。
//
// 视角变换沿用 model viewer 的 ViewConfig（y-up 球面角相机 + 右键拖拽/滚轮缩放），
// 保证两个查看器手感一致；默认相机放在三方块斜前方，一眼看全三块。

#ifndef JPOV_DEMO_SKYLIGHT_VIEWER_APP_H_
#define JPOV_DEMO_SKYLIGHT_VIEWER_APP_H_

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/demo/skylight_scene.h"
#include "tools/jpov/interface/water_config.h"
#include "tools/jpov/demo/view_config.h"
#include "tools/jpov/demo/viewer_app.h"   // 复用 kViewerWidth/Height/Fps/FontAlias
#include "tools/jpov/interface/ui.h"

namespace jpov_skylight {

// 分辨率/帧率/字体别名沿用 model viewer 的单点定义（viewer_app.h），
// 避免两个查看器各写一套硬编码而分叉。
using jpov_viewer::kViewerWidth;
using jpov_viewer::kViewerHeight;
using jpov_viewer::kViewerFps;
using jpov_viewer::kViewerFontAlias;

// 天光查看器渲染核心 App。
class SkylightApp : public JPOV {
public:
    using JPOV::JPOV;

    // ── 场景静态资源（Init() 后装配一次，不在 OneIteration 里重复构造/上传）──
    uint32_t box_mesh_ = 0;              // 1×1×1 方块（三块共用）
    uint32_t ground_mesh_ = 0;           // 40×40 地面 quad
    // 材质阶梯用的长条（20×200）：四段沿 X 并排，见 skylight_scene.h 的 kLadder*。
    uint32_t ground_strip_mesh_ = 0;
    jpov::PBRMaterial mat_low_;          // 低反（rough=1.0, metal=0）
    jpov::PBRMaterial mat_high_;         // 高光（rough=0.05, metal=0）
    jpov::PBRMaterial mat_metal_;        // 金属（metal=1, rough=0.15）
    jpov::PBRMaterial mat_ground_;       // 灰色地面
    jpov::PBRMaterial mat_clutter_;      // 镜子上方随机模型（粗糙灰）
    jpov::PBRMaterial mat_mirror_skin_;  // 镜面皮肤（曲面 SSR 反射体：人）

    // ── 蓝人（mixamo_male）：同一份蒙皮 mesh + 骨架，每个方块顶面各一个 T-pose 实例 ──
    // person_mesh_ / person_skeleton_id_ 为 0（未装配）时整段跳过 → 不带人场景零回归。
    // person_instances_ 只存 3 个**静态** SkinnedInstanceState（pose_a==pose_b=0 = 恒等 pose
    //   = 骨架 rest = T-pose），每帧拷一份交给 DrawMeshWithSkeleton（一次 instanced draw）。
    uint32_t person_mesh_ = 0;           // 蒙皮 mesh_id（glb 第一个 primitive）
    uint32_t person_skeleton_id_ = 0;    // RegisterSkeleton 返回的骨架 id
    jpov::PBRMaterial person_material_;  // 蒙皮网格的 PBR 材质
    jpov::GltfObject person_gltf_;       // 资源保活（同 models_ 的所有权约定）
    std::vector<jpov::SkinnedInstanceState> person_instances_;
    // 镜面皮肤的蓝人（2026-10-31）：4 个随机站在镜面地板上的实例，**ssr_reflector=true**
    // ⇒ 属性 pass 会给它们写视空间法线（曲面），用来测"曲面上的 SSR 贴不贴"。
    std::vector<jpov::SkinnedInstanceState> person_mirror_instances_;
    float person_scale_ = 1.0f;     // InstallPerson 算出的归一缩放（镜面人复用）
    float person_min_y_ = 0.0f;     // 资产包围盒 min.y（脚点对齐用）

    // 4 个随机摆放：极坐标 r∈[4,14]、随机朝向；固定种子 ⇒ 每次拍摄同一个场景。
    void BuildMirrorPeople(int count = 4, uint32_t seed = 20261031u) {
        person_mirror_instances_.clear();
        uint32_t st = seed;
        auto rnd = [&st]() {
            st ^= st << 13;
            st ^= st >> 17;
            st ^= st << 5;
            return static_cast<float>(st & 0xFFFFFFu) / static_cast<float>(0x1000000);
        };
        for (int i = 0; i < count; ++i) {
            const float ang = rnd() * 6.2831853f;
            const float r = 4.0f + rnd() * 10.0f;
            const float yaw = rnd() * 6.2831853f;
            jpov::SkinnedInstanceState inst;
            inst.transform.center = {r * std::cos(ang),
                                     -person_scale_ * person_min_y_,
                                     r * std::sin(ang)};
            inst.transform.up = {0.0f, 1.0f, 0.0f};
            inst.transform.front = {std::sin(yaw), 0.0f, std::cos(yaw)};
            inst.transform.scale = person_scale_;
            inst.pose_a = 0;
            inst.pose_b = 0;
            inst.ratio = 0.0f;
            person_mirror_instances_.push_back(inst);
        }
    }

    // ── 额外模型（桌子 / 高模橡树）：用来看“物体受光”，供标定夜色 ambient ──
    // 每个 Slot = 一个 glTF + 世界摆放（center/up/front/scale），由主程序装载后 AddModel。
    struct ModelSlot {
        jpov::GltfObject obj;
        jpov::Vec3f center;
        jpov::Vec3f up;
        jpov::Vec3f front;
        float scale = 1.0f;   // 整体缩放（资产原始尺寸差异很大，需按目标高度归一）
    };
    std::vector<ModelSlot> models_;

    void AddModel(jpov::GltfObject obj, jpov::Vec3f center,
                  jpov::Vec3f up, jpov::Vec3f front, float scale = 1.0f) {
        models_.push_back({std::move(obj), center, up, front, scale});
    }

    // ── 视角：沿用 ViewConfig（y-up 球面角 + 右键拖拽/滚轮缩放）──
    jpov_viewer::ViewConfig view_;

    // ── 天光自由度（跨帧持有；天光与光照全由它们推导）──
    // 滑条值 → SkyCommand 字段的编码见 skylight_scene.h 的 SkyDegrees。
    SkyDegrees deg_;

    // 是否使用**三色环境光**（[天, 天际线, 地] 垂直梯度）替代单色 ambient。
    // 供肉眼对比：开=由 SkyCommand 推导的三色（AmbientTricolor），关=原来的单色（AmbientColor）。
    void SetTricolorAmbient(bool on) { tricolor_ambient_ = on; }

    // ── 调试用光照覆盖（仅拍摄验证，不提供 UI）──
    // 目的：固定 sky，只改 RenderCommandList 的光源，验证「水体/水皮消费的是光，不是 sky」。
    // 不设 = 用天际推导的默认值。
    void SetAmbientOverride(const jpov::AmbientLight& a) { ambient_override_ = a; }
    void ClearAmbientOverride() { ambient_override_.reset(); }
    void SetSunOverride(const jpov::DirectionalLight& d) { sun_override_ = d; }
    void ClearSunOverride() { sun_override_.reset(); }

    // 是否开启 SMAA 后处理抗锯齿（供肉眼 A/B 对比）。headless 拍摄可显式设。
    void SetSmaa(bool on, float threshold = 0.1f) {
        smaa_on_ = on;
        smaa_threshold_ = threshold;
    }

    // 环境镜面（IBL specular）开关 + 天光消费量。headless A/B 拍摄显式设，
    // 交互时走面板（⑦）。默认开、消费量 1.0（即“所有镜面都消费天光”）。
    void SetEnvSpecular(bool on, float sky_scale = 1.0f) {
        env_specular_on_ = on;
        env_specular_scale_ = sky_scale;
    }

    void InstallTextMeasure() {
        ui_.SetTextMeasure(&SkylightApp::AppTextWidth, this);
    }

    // 交互窗口是否绘制光照面板（headless 拍摄=false，截图即纯 3D 场景）。
    void SetShowPanel(bool show) { show_panel_ = show; }

    // 档位 → 地面材质（0..3；四段拼接由调用方按 kLadderMaterial 逐段取）。
    static jpov::PBRMaterial FloorMaterialOf(int preset) {
        switch (preset) {
            case 0:
                return jpov_skylight::FloorMirrorMaterial();
            case 1:
                return jpov_skylight::FloorMetalMaterial();
            case 2:
                return jpov_skylight::FloorStoneMaterial();
            default:
                return jpov_skylight::FloorRockMaterial();
        }
    }

    // 是否绘制场景几何（三方块 + 地面）。headless 拍“天空本身”时置 false，
    // 排除方块遮挡与受光干扰，只留天光背景。
    void SetShowScene(bool show) { show_scene_ = show; }

    // ── 场景下拉（见面板“场景”）────────────────────────────────────────────
    //   0 = 标定场：灰地面 + 三方块 + 桌子/橡树 + 蓝人（原有）
    //   1 = **镜面地板 + 随机模型**：SSR 的验收场景（地表大镜子，上方随机摆方块/模型）
    //   2 = 只天空：不画任何几何（headless 拍天光用）
    int scene_index_ = 0;

    // ── 水凝胶场景（M1 验收）：水体介质 + 水下岩板/三件物体 ──
    //
    // σ 默认值 = 纯水 20°C 的吸收谱（650nm 0.349 / 550nm 0.054 / 450nm 0.014 1/m，
    // Pope & Fry 1997 量级）—— 所以“10 m 后只剩蓝”不是调出来的，是参数自然结果。
    // 散射取 0.05（清澈偏浊）：既能看出介质注入，又不把水下东西糊掉。
    static constexpr int kSceneWater = 3;
    bool IsWaterScene() const { return scene_index_ == kSceneWater; }

    // ── 场景 4：海面选型场（Ocean Lab，2026-11-20）──
    // **面板只碰 WaterConfig**：风（速度/方向/风区）、涌（开关/波高/周期/方向）、
    // 介质（水色/浑浊/水深/曝光）。谱怎么离散、相位怎么随机、64 条细节怎么铺，
    // 全部由 BuildWater() 自动完成（interface/water_config.h）。
    static constexpr int kSceneOceanLab = 4;
    bool IsOceanLab() const { return scene_index_ == kSceneOceanLab; }

    jpov::water::WaterConfig water_cfg_;
    jpov::water::WaterQuality water_quality_;
    // 派生缓存：只有 WaterConfig 变化时才重算（每帧重算 80 条波是浪费，也会拖慢 UI）。
    jpov::WaterWaves lab_waves_;
    jpov::water::WaterReadout lab_readout_;
    bool lab_derived_dirty_ = true;
    void InvalidateLabWater() { lab_derived_dirty_ = true; }
    void EnsureLabWater() {
        if (!lab_derived_dirty_) {
            return;
        }
        jpov::water::BuildWater(water_cfg_, water_quality_, &lab_waves_, &lab_readout_);
        lab_derived_dirty_ = false;
    }

    // 选型场默认值 = 上轮实验的胜出者（B 谱结构：4~8 主波 + 64 条各向同性细节，无 warp）：
    // U=8 m/s、吹向 34°（= 方位 214° − 180°，即相机朝太阳那侧）、风区 250 km、
    // σa 0.50/0.054/0.014、σs 0.04、曝光 0.6、45° 俯视 5 m。
    void SetOceanLab(bool on) {
        if (!on) {
            scene_index_ = 0;
            return;
        }
        scene_index_ = kSceneOceanLab;
        water_waves_on_ = true;
        water_skin_on_ = true;
        water_medium_on_ = true;
        deg_.sun_elev_deg = 35.0f;
        deg_.sun_azim_deg = 214.0f;
        deg_.turbidity = 2.0f;
        deg_.daylight_season = 0.0f;
        water_cfg_.SetWind(8.0f, 34.0f * 3.14159265358979323846f / 180.0f);
        water_cfg_.fetch_km = 250.0f;
        water_cfg_.duration_h = std::numeric_limits<float>::infinity();
        water_cfg_.swell.height_m = 0.0f;   // 默认无涌：与胜出者 B 对齐
        water_cfg_.swell.period_s = 9.5f;
        ocean_swell_on_ = false;            // 面板开关必须与 height_m=0 一致（否则勾着却没涌）
        water_cfg_.depth_m = 3.5f;
        water_cfg_.level_y = 0.0f;
        exposure_ = 0.6f;
        InvalidateLabWater();
        SetOceanLabView();
    }

    // 选型场默认机位：45° 俯视、相机高 5 m、朝太阳（= 上轮实验的 ① 机位）。
    void SetOceanLabView() {
        constexpr double kDegToRad = 3.14159265358979323846 / 180.0;
        view_ = jpov_viewer::DefaultView();
        view_.phi = 45.0 * kDegToRad;
        view_.theta = (static_cast<double>(deg_.sun_azim_deg) - 180.0) * kDegToRad;
        view_.R = 5.0 / std::sin(view_.phi);
    }

    // 选型场的动画时钟：真实时间（frame_count/60 掉帧会变慢）。
    float OceanLabTime() {
        if (!ocean_clock_started_) {
            ocean_clock_start_ = std::chrono::steady_clock::now();
            ocean_clock_started_ = true;
        }
        const auto dt = std::chrono::steady_clock::now() - ocean_clock_start_;
        return std::chrono::duration<float>(dt).count();
    }
    // 方向键的按住计数（纯状态，语义见 view_config.h::ApplyArrowKeys）。
    jpov_viewer::ArrowKeyState key_turn_;

    std::chrono::steady_clock::time_point ocean_clock_start_{};
    bool ocean_clock_started_ = false;
    float ocean_time_speed_ = 1.0f;   // 时间倍率（0 = 冻结）
    bool  ocean_swell_on_ = false;    // 涌开关（面板）
    // 两个正交开关（2026-11-19 Danis 要的「拆开看对齐」）：
    //   · water_skin_on_   = 界面层（水皮 WBO）：反射 / 透射 / 水面高光 / Snell 窗；
    //   · water_medium_on_ = 水体介质（体积）：水下吸收 + 单次散射注入。
    // 默认都开（旧行为逐字节不变）。
    bool water_skin_on_ = true;
    bool water_medium_on_ = true;
    // 短波（毛细波/短重力波）：只进 MSS（不改 displacement / 法线 / 求交）。
    // 方向自动取 4 波谱的主方向；量由 Cox–Munk 风速给出（面板滑条）。
    float water_wind_ms_ = 8.0f;

    // ── 水面波浪（法线扰动 / 高度置换）──
    // 默认关（= 纯平面，零回归）。capture/UI 打开后按下面这份谱叠加。
    static jpov::WaterWaves MakeDefaultWaves() {
        jpov::WaterWaves w;
        w.num_waves = 4;
        w.waves[0] = jpov::WaterWave{1.0f, 0.0f, 24.0f, 0.28f, 0.0f};
        w.waves[1] = jpov::WaterWave{0.6f, 0.8f, 13.0f, 0.14f, 1.7f};
        w.waves[2] = jpov::WaterWave{-0.8f, 0.6f, 7.0f, 0.07f, 0.4f};
        w.waves[3] = jpov::WaterWave{0.3f, -0.95f, 3.5f, 0.03f, 2.6f};
        w.flow_align = 0.6f;
        w.flow_wind_gain = 0.6f;
        w.flow_uv_scale = 1.0f / 220.0f;
        return w;
    }
    jpov::WaterWaves water_waves_ = MakeDefaultWaves();
    bool water_waves_on_ = false;
    // 远场统计着色（2026-11-25）：只作用于**水上**——水面一个像素装不下最长波时，反射走
    // 椭圆核采样、反射率/透射走斜率分布的积分。默认关（旧行为逐字节不变）。
    bool water_farfield_on_ = false;
    bool water_farfield_yellow_ = false;   // 调试：远场像素涂黄
    bool water_farfield_reflect_ = false;  // 远场：平面镜反射
    bool water_farfield_specular_ = false; // 远场：平面镜太阳高光
    bool water_farfield_alpha_ = false;    // 远场：alpha 用方向反照率
    void SetWaterFarFieldAlpha(bool on) { water_farfield_alpha_ = on; }
    void SetWaterFarFieldReflect(bool on) { water_farfield_reflect_ = on; }
    void SetWaterFarFieldSpecular(bool on) { water_farfield_specular_ = on; }
    void SetWaterFarField(bool on) { water_farfield_on_ = on; }
    void SetWaterFarFieldYellow(bool on) { water_farfield_yellow_ = on; }
    float water_wave_amp_scale_ = 1.0f;
    // true ⇒ 帧内直接用 water_waves_.num_waves（capture 用整份配置覆盖时打开）。
    bool water_use_config_count_ = false;
    void SetWaterWaves(bool on) { water_waves_on_ = on; }
    // 用一整份配置替换默认谱（海面报告 / 参数扫描用）。
    // 位置参数不可读时无法用指定初始化，直接整体赋值最清楚。
    void SetWaterWavesConfig(const jpov::WaterWaves& waves) {
        water_waves_ = waves;
        water_use_config_count_ = true;
    }
    void SetWaterWaveAmplitudeScale(float scale) { water_wave_amp_scale_ = scale; }
    void SetWaterFlowTexture(uint32_t id) { water_waves_.flow_texture_id = id; }
    void SetWaterWaveDisplacement(bool on) { water_waves_.displacement = on; }
    // 时间控制：开启冻结后 time_s 用它（否则按 frame_count/60 前进）。
    // 报障复现必须有这个 —— 波浪是时变的，光靠机位对不上。
    float water_wave_time_s_ = 0.0f;
    bool water_wave_time_freeze_ = false;
    void SetWaterWaveTime(float sec) { water_wave_time_s_ = sec; water_wave_time_freeze_ = true; }
    void SetWaterWaveTimeFrozen(bool on) { water_wave_time_freeze_ = on; }
    // 时间倍率（0 = 冻结）：调用方对 time 的缩放，不再是波谱里的字段。
    void SetWaterTimeSpeed(float s) { ocean_time_speed_ = s; }
    int water_wave_debug_ = 0;   // 0 正常 / 1 法线颜色（capture 用）
    // 曝光（tone map 之前的线性增益）。1.0 = 物理锚点（默认）。
    float exposure_ = 1.0f;
    void SetWaterWaveDebug(int mode) { water_wave_debug_ = mode; }
    // 面板：法线可视化开关 + 波数（SliderFloat 只能用 float）。
    bool water_wave_normal_view_ = false;
    float water_wave_count_f_ = 4.0f;
    float water_sigma_a_r_ = 0.35f;
    float water_sigma_a_g_ = 0.054f;
    float water_sigma_a_b_ = 0.014f;
    float water_sigma_s_ = 0.05f;
    float water_phase_g_ = 0.35f;
    float slab_top_depth_m_ = jpov_skylight::kSlabTopDepthDefM;
    // 水配置：水位 + 折射率。**天空折射也消费这一份**（没有独立的天球折射开关）。
    float water_level_y_ = jpov_skylight::kWaterLevelY;
    float water_ior_ = jpov_skylight::kWaterIor;
    void SetWaterLevel(float y) { water_level_y_ = y; }
    void SetWaterIor(float ior) { water_ior_ = ior; }

    // ── 水下验收物体：蓝人（skinned）──
    // 用 InstancedInstanceState 的静态语义（pose_a = pose_b = 0 = 恒等 pose）：一次 instanced
    // draw 画出多个人；脚点对齐岩板顶面（-person_scale_*person_min_y_ 是 InstallPerson 的脚点偏移）。
    // Pre-condition: InstallPerson() 已调用（需要 person_scale_ / person_min_y_）。
    std::vector<jpov::SkinnedInstanceState> water_people_;
    // center.y 按“岩板顶面 = 0”算；真正出图时整组平移到岩板顶面（滑条可动）。
    void BuildWaterPeople(int count = 3) {
        CHECK_GE(count, 1);
        water_people_.clear();
        const float xs[3] = {-4.0f, 0.0f, 4.5f};
        const float zs[3] = {-2.0f, -4.0f, -1.0f};
        for (int i = 0; i < count; ++i) {
            jpov::SkinnedInstanceState inst;
            inst.transform.center = {xs[i % 3], -person_scale_ * person_min_y_,
                                     zs[i % 3]};
            inst.transform.up = {0.0f, 1.0f, 0.0f};
            inst.transform.front = {0.0f, 0.0f, 1.0f};
            inst.transform.scale = person_scale_;
            inst.pose_a = 0;
            inst.pose_b = 0;
            inst.ratio = 0.0f;
            water_people_.push_back(inst);
        }
    }
    void SetWaterMedium(bool on) { water_medium_on_ = on; }
    void SetWaterSkin(bool on) { water_skin_on_ = on; }
    void SetSlabDepth(float m) { slab_top_depth_m_ = m; }
    void SetWaterScene(bool on) { scene_index_ = on ? kSceneWater : 0; }

    // ── 镜面高光带宽（光源角半径 θ）验收：场景与机位 ─────────────────────
    //
    // 地面 = 200×200 m 的大镜面（材质走 SetFloorMaterialOverride(MirrorFloorMaterial(r))），
    // 太阳是画面里唯一的强光。机位**算死**：原点处的反射方向 = 仰角 φ、方位 θ+180，
    // 故取 φ = 太阳仰角、θ = 太阳方位 − 180° ⇒ 太阳的像精确落在画面正中（原点）。
    // 为什么要它：完美镜面（roughness=0）+ 平行光 = 两个 δ 相乘，没有光源角半径（带宽）
    // 时逐像素采样只能拿到 ∞ 或 0 ⇒ 地面上**一个高光像素都没有**（docs/jpov_specular_bandwidth.md）。
    static constexpr float kMirrorSunElevDeg = 12.0f;   // 低斜阳：日盘与它的像同框
    static constexpr float kMirrorSunAzimDeg = 180.0f;
    static constexpr double kMirrorCameraRadiusM = 20.0;

    // 装配「镜面地板」场景所需的最小资源（地面 + 散布方块）。不装模型/蓝人：
    // 本验收不需要外部资产（拍摄与单测都能直接跑）。
    void InstallMirrorFloorScene() {
        if (box_mesh_ == 0) {
            box_mesh_ = RegisterMesh(jpov::MeshData::MakeBox(
                jpov_skylight::kBoxHalf, jpov_skylight::kBoxHalf,
                jpov_skylight::kBoxHalf));
        }
        if (ground_mesh_ == 0) {
            ground_mesh_ = RegisterMesh(jpov_skylight::MakeGroundQuad());
        }
        mat_clutter_ = jpov_skylight::MirrorClutterMaterial();
        BuildClutter(/*count=*/24);   // 固定 24 个散布方块：给镜子一点"有内容"的倒影
        scene_index_ = 1;   // 镜面地板场景（地板 = ssr_reflector）
    }

    // 把相机与太阳摆到「太阳的像落在画面正中」的那一套几何上。
    void SetMirrorSpecularView() {
        constexpr double kDegToRad = 3.14159265358979323846 / 180.0;
        deg_.turbidity       = jpov_skylight::kTurbidityDef;
        deg_.daylight_season = 0.0f;
        deg_.sun_elev_deg    = kMirrorSunElevDeg;
        deg_.sun_azim_deg    = kMirrorSunAzimDeg;
        view_ = jpov_viewer::DefaultView();
        view_.phi   = static_cast<double>(kMirrorSunElevDeg) * kDegToRad;
        view_.theta = static_cast<double>(kMirrorSunAzimDeg - 180.0f) * kDegToRad;
        view_.R     = kMirrorCameraRadiusM;
    }

    // ── 点光源镜面验收：灯的像落在画面正中 ──────────────────────────────
    //
    // 与太阳那套同一个思路，但点光源没有「方向」可拿来对齐，所以反过来算：
    //   相机 C 看向原点 P ⇒ 原点处的视线 v = −C，关于地面 y=0 镜像得 r；
    //   把灯放在 P + d·r ⇒ 原点处 H = normalize(r − v) = +Y = 地面法线 ⇒ 高光正中。
    // 灯的**角半径**逐像素为 asin(R/dist)（R = physical_radius），所以 R 越大，
    // 地面上那块「灯的像」越大越糊 —— 这就是点光源角半径验收要看的量。
    static constexpr float kPointLampDistanceM = 3.5f;  // 灯到原点（=反射点）的距离
    static constexpr float kPointLampIntensity = 1.0f;
    static constexpr float kPointLampRangeM = 40.0f;    // linear_radius（衰减范围）

    // 相机：俯视 30°（不像 12° 那样把像拉成细条，灯的圆盘读得清）。
    void SetPointSpecularView() {
        constexpr double kDegToRad = 3.14159265358979323846 / 180.0;
        deg_.turbidity       = jpov_skylight::kTurbidityDef;
        deg_.daylight_season = 0.0f;
        deg_.sun_elev_deg    = 30.0f;
        deg_.sun_azim_deg    = 180.0f;
        view_ = jpov_viewer::DefaultView();
        view_.phi   = 30.0 * kDegToRad;
        view_.theta = 0.0;
        view_.R     = 12.0;
    }

    // 把一盏球灯（半径 physical_radius）放到「它的像正好落在画面正中」的位置。
    // Pre-condition: 已调用过 SetPointSpecularView()（灯的位置依赖当前相机）。
    void PlacePointLampAtScreenCenter(float physical_radius) {
        const jpov::Vec3f c = view_.Position();
        const float len = std::sqrt(c.x() * c.x() + c.y() * c.y() + c.z() * c.z());
        CHECK_GT(len, 1.0e-6f);
        // v = P − C = −C（P = 原点）⇒ r = 关于 y=0 的镜像 = (−Cx, +Cy, −Cz)/|C|
        jpov::PointLight lamp;
        lamp.position = {-c.x() / len * kPointLampDistanceM,
                         c.y() / len * kPointLampDistanceM,
                         -c.z() / len * kPointLampDistanceM};
        lamp.color = {1.0f, 0.96f, 0.90f, 1.0f};
        lamp.linear_radius = kPointLampRangeM;
        lamp.physical_radius = physical_radius;
        lamp.intensity = kPointLampIntensity;
        point_lamp_ = lamp;
    }

    // 点光源覆盖（headless 拍摄 / 单测用）。不设 = 场景里没有点光源。
    std::optional<jpov::PointLight> point_lamp_;
    void ClearPointLamp() { point_lamp_.reset(); }

    // 镜面地板这一场景里随机摆放的方块（固定种子，装配一次；不逐帧随机）。
    struct Clutter {
        jpov::Vec3f center;
        jpov::Vec3f front;
        float scale = 1.0f;
    };
    std::vector<Clutter> clutter_;

    // 固定种子生成 N 个"散布在镜子上方"的方块：极坐标 r∈[5,30]、随机朝向/大小。
    // 用固定种子是刻意的：SSR 的验收图要能重拍对比，场景不能每帧变。
    void BuildClutter(int count, uint32_t seed = 20261030u) {
        clutter_.clear();
        uint32_t s = seed;
        auto rnd = [&s]() {   // xorshift32：不引 <random>，且跨平台逐位一致
            s ^= s << 13;
            s ^= s >> 17;
            s ^= s << 5;
            return static_cast<float>(s & 0xFFFFFFu) / static_cast<float>(0x1000000);
        };
        for (int i = 0; i < count; ++i) {
            const float ang = rnd() * 6.2831853f;
            const float r = 5.0f + rnd() * 25.0f;
            const float sc = 0.5f + rnd() * 3.0f;
            const float yaw = rnd() * 6.2831853f;
            Clutter c;
            c.center = {r * std::cos(ang), 0.5f * sc, r * std::sin(ang)};
            c.front = {std::sin(yaw), 0.0f, std::cos(yaw)};
            c.scale = sc;
            clutter_.push_back(c);
        }
    }

    // ── SSR（屏幕空间反射）──────────────────────────────────────────────
    // 只对 ssr_reflector 的物体生效（镜面地板）；**必须同时开环境镜面**（合成要重算并替换
    // 那一项，没有 env 就没有可替换的对象）。关掉时整条链路零开销。
    bool  ssr_on_ = false;
    // 地面材质档位（镜面 / 金属 / 石砖 / 岩石 / 四段拼接）。见 skylight_scene.h 的预设。
    int   floor_preset_ = 0;
    // 地面材质覆盖（headless 拍摄专用；不设 = 用 floor_preset_ 的预设）。
    // 为什么要这个口子：镜面高光的带宽要拿**roughness=0 的大镜面地面**验收，而面板的
    // 四个预设都是有实际用途的档位（r=0 的纯镜面没有 SSR/IBL 之外的内容，不适合当预设）。
    // 与 sun_override_ 同一套用法：只在拍摄路径里设，交互与其它拍摄不受影响。
    std::optional<jpov::PBRMaterial> floor_material_override_;
    void SetFloorMaterialOverride(const jpov::PBRMaterial& material) {
        floor_material_override_ = material;
    }
    void ClearFloorMaterialOverride() { floor_material_override_.reset(); }
    float ssr_scale_ = 0.5f;        // = 1/N，N = 1..4（面板滑条给的是 N）
    int   ssr_downscale_n_ = 2;     // 面板滑条：降采样倍率 N（1 = 不降采样）
    float ssr_thickness_px_ = 6.0f; // N_px（SSR 像素）
    int   ssr_max_ray_px_ = 220;    // 屏幕空间射线预算（SSR 像素）
    float ssr_rough_to_lod_ = 8.0f;   // roughness → mip LOD 的斜率（"磨砂"强度）
    float ssr_rough_cutoff_ = 0.60f;  // 超过它就不发射线（直接走 env）
    int   ssr_debug_ = 0;
    // 掠射淡出（2026-11-13）：kappa = cos(θ镜像)/cos(θ主视)，越大越该淡出。
    // 只有两个阈值；把 end 调到 <= start 即等于关闭。
    float ssr_grazing_start_ = 2.0f;
    float ssr_grazing_end_ = 10.0f;
    // A 的三个淡出（2026-11-13，遮丑用）。单位 = **全分辨率屏幕像素**；0 = 关闭。
    // 默认全 0 ⇒ 与"没有这三条"逐字节相同（gold 不动）。
    float ssr_fade_budget_px_ = 0.0f;      // ① 命中点离射线预算还剩多少
    float ssr_fade_edge_px_ = 0.0f;        // ② 反射体像素 P 贴边
    float ssr_fade_hit_edge_px_ = 0.0f;    // ③ 命中点 Q 贴边
    // 遮丑滤波器（2026-11-13）：三个 toggle 各自独立，默认全关 = 旧行为。
    bool  ssr_filter_hole_ = false;        // ① 形态学补洞（1px 梳齿缝）
    bool  ssr_filter_clamp_ = false;       // ② 方差引导离群抑制（毛刺）
    bool  ssr_filter_atrous_ = false;      // ③ 边缘保持 à-trous（多尺度）
    float ssr_filter_atrous_iters_ = 1.0f; // ③ 趟数 1..3（步长 1/2/4）
    float ssr_filter_clamp_k_ = 2.0f;      // ② 允许偏离几倍标准差（面板暂未暴露，拍摄用 env）
    // 背面命中的两个正交 flag（2026-11-13）：模块照常给颜色，处置在这里。
    // 默认 (false, 1.0) = 交回 env = 与旧行为逐字节相同（gold 不动）；交互时再打开。
    bool  ssr_backface_use_ssr_ = false;   // false = 用 env；true = 用 SSR 颜色
    float ssr_backface_dim_ = 1.0f;        // 该像素最终采用那份颜色的亮度因子
    void SetSsr(bool on) { ssr_on_ = on; }

    // ⭐ 唯一渲染体：交互 Run 循环 与 headless 拍摄共用（zero 分叉）。
    void OneIteration(int64_t frame_count, const jpov::InputSnapshot& input,
                      const jpov::WindowInfo& winfo,
                      jpov::RenderCommandList* cmds) override {
        (void)frame_count;

        cmds->camera.fbo_3d_width_  = kViewerWidth;
        cmds->camera.fbo_3d_height_ = kViewerHeight;

        // 交互输入 → 改视角（同 model viewer 的 ApplyInput）；headless 不消费输入。
        if (show_panel_) {
            float dx = 0.0f, dy = 0.0f, scroll = 0.0f;
            if (input.right.IsDrag()) {
                dx = input.mouse_dx;
                dy = input.mouse_dy;
            }
            if (input.scroll_delta != 0.0f) scroll = input.scroll_delta;
            jpov_viewer::ApplyInput(&view_, dx, dy, scroll,
                                    static_cast<int>(winfo.width),
                                    static_cast<int>(winfo.height));

            // ── 方向键调视角（2026-11-20）：与右键拖拽同一套符号，每次 5° ──
            // 纯函数在 view_config.h::ApplyArrowKeys（可单测），这里只提供步长。
            constexpr double kKeyStep = 5.0 * 3.14159265358979323846 / 180.0;
            jpov_viewer::ApplyArrowKeys(input, kKeyStep, &key_turn_, &view_);
        }

        // 相机由 view_ 推导（同 model viewer；target=原点）。
        cmds->camera.position = view_.Position();
        cmds->camera.target   = jpov_viewer::ViewConfig::Target();
        cmds->camera.up       = {0.0f, 1.0f, 0.0f};
        cmds->camera.fov      = 60.0f;
        cmds->camera.near     = 0.05f;
        cmds->camera.far      = 1000.0f;

        // 天光 + 光照：全由五个自由度推导——sky 走 CreateDefaultSkyCommand，
        // 主平行光（白天太阳/夜间月亮）与 ambient 全由该 sky 推导（含夜色项）；
        // moon_season 与夜空蓝覆盖默认值。
        const SkyLighting nl = MakeSkyLighting(deg_, tricolor_ambient_);
        const bool water_scene = IsWaterScene();
        const bool ocean_lab = IsOceanLab();
        jpov::SkyCommand sky = nl.sky;
        // 地平线以下的填充色与探针下半球**不在这里配**：Renderer 在有水面时统一按
        // 「深海渐近色 + 水面 Fresnel」自动填（单一出处，见 renderer.cc 的天空段）。
        cmds->sky     = sky;
        // 天球折射不再由场景配置：Renderer 在「有水面 + 相机在水下」时自动填
        // （折射率取 WaterCommand::ior，与水体/水皮同一份配置）。
        cmds->sun     = nl.dir_light;
        cmds->ambient = nl.ambient;
        // 调试用覆盖（仅 headless 拍摄「水到底在消费谁」的 A/B）：放在推导之后，
        // 保证「sky 不变、只换光」——旧实现里水的照明只认 sky，这种 A/B 根本做不出来。
        if (ambient_override_.has_value()) {
            cmds->ambient = *ambient_override_;
        }
        if (sun_override_.has_value()) {
            cmds->sun = *sun_override_;
        }
        // 点光源镜面验收（交互/其它拍摄不用）：灯的位置由 PlacePointLampAtScreenCenter()
        // 算好，保证它的像落在画面正中（推导见那个函数的注释）。
        if (point_lamp_.has_value()) {
            cmds->point_lights.push_back(*point_lamp_);
        }
        cmds->tone_mapping = true;
        // 曝光（EV=0 基准）。海面报告用它把「物理锚点」下的亮度对齐到照片观感；
        // 默认 1.0 ⇒ 旧行为逐字节不变。
        cmds->exposure = exposure_;

        // 环境镜面（IBL specular）：让 metallic 材质从天空 cubemap 取反射。
        // 没这一项时，金属块只有太阳/点光源的直射高光，其余全黑——
        // 三块方块的 index 2（metallic=1, “无漫反射，纯环境反射”）是验收工件。
        // ⚠️ 水场景**必须**有环境镜面：水面唯一的反射源是天光探针，Renderer 对
        //    「有水面却没 env_specular / sky_scale=0 / 探针没建」直接 CHECK（宁可崩也不静默
        //    退化成黑水面）。所以这里在水场景强制打开并把消费量抬到 (0,1]，避免用户在面板上
        //    一勾就把进程打挂。
        if (env_specular_on_ || water_scene || ocean_lab) {
            jpov::EnvSpecular es;
            es.sky_scale = (water_scene || ocean_lab) ? std::max(env_specular_scale_, 0.05f)
                                                      : env_specular_scale_;
            cmds->env_specular = es;
        }

        // SMAA 后处理抗锯齿（可选）：tone map 之后、2D 之前的最后一站。
        if (smaa_on_) {
            jpov::SmaaConfig sc;
            sc.enabled = true;
            sc.threshold = smaa_threshold_;
            cmds->smaa = sc;
        }

        // 场景（下拉）：
        //   0 标定场：灰地面 + 三方块（低反/高光/金属）+ 桌子/橡树 + 蓝人
        //   1 镜面地板 + 随机模型：SSR 的验收场景（地板是 ssr_reflector）
        //   2 只天空：不画几何（headless 拍天光）
        const bool draw_scene = show_scene_ && scene_index_ != 2;
        // ── 场景 4：海面选型场 —— 只画**一整块**海底底板（120×120 m）。──
        // 不叠任何礁斑/棋盘格：共面薄垫在 70 m 外必 z-fight，硬边深色块在水下又被读成"黑洞"
        // （2026-11-20 Danis 实拍确认，见 known_issue.md §10.18）。
        if (draw_scene && ocean_lab) {
            const float bed_y = water_cfg_.level_y - water_cfg_.depth_m;
            // 海底只画这一块。浅海靠"水深滑条 + 沙底本身的明暗"表达，不再加装饰几何。
            cmds->DrawObject3D(ground_mesh_, jpov_skylight::SeabedMaterial(1.0f),
                               {0.0f, bed_y, 0.0f},
                               /*up*/ {0.0f, 1.0f, 0.0f},
                               /*front*/ {0.0f, 0.0f, 1.0f}, /*scale*/ 5.0f);
        } else if (draw_scene && water_scene) {
            // 水下岩板（30×30 m）：顶面在「水位 − 深度」，随两条滑条升降。
            // 材质中性（无彩灰）⇒ 颜色的任何变化都只能来自水体。
            const float top = water_level_y_ - slab_top_depth_m_;
            cmds->DrawObject3D(ground_mesh_, jpov_skylight::SlabMaterial(),
                               /*center*/ {0.0f, top, 0.0f},
                               /*up*/     {0.0f, 1.0f, 0.0f},
                               /*front*/  {0.0f, 0.0f, 1.0f},
                               /*scale*/  jpov_skylight::kSlabSizeM /
                                          jpov_skylight::kSlabGroundMeshSizeM);
            // 三件物体坐在岩板上：低反 / 高光 / 金属。
            // 验收“凝胶对物体的遮挡”：随滑条下沉，三件的颜色应整组变暗、逐渐只剩蓝。
            cmds->DrawObject3D(box_mesh_, mat_low_,   {6.0f, top + 1.0f, 5.0f},
                               /*up*/ {0.0f, 1.0f, 0.0f}, /*front*/ {0.0f, 0.0f, 1.0f},
                               /*scale*/ 2.4f);
            cmds->DrawObject3D(box_mesh_, mat_high_,  {-5.5f, top + 1.0f, 6.0f},
                               /*up*/ {0.0f, 1.0f, 0.0f}, /*front*/ {0.0f, 0.0f, 1.0f},
                               /*scale*/ 2.4f);
            cmds->DrawObject3D(box_mesh_, mat_metal_, {1.0f, top + 1.4f, -6.0f},
                               /*up*/ {0.0f, 1.0f, 0.0f}, /*front*/ {0.0f, 0.0f, 1.0f},
                               /*scale*/ 2.2f);
            // 蓝人（skinned pass）：三个人站在岩板上 —— 水下看人的轮廓/颜色最能暴露
            // 「水的消光是否作用在正确的地方」。
            if (person_mesh_ != 0 && !water_people_.empty()) {
                std::vector<jpov::SkinnedInstanceState> people = water_people_;
                for (jpov::SkinnedInstanceState& inst : people) {
                    inst.transform.center = {inst.transform.center.x(),
                                              inst.transform.center.y() + top,
                                              inst.transform.center.z()};
                }
                cmds->DrawMeshWithSkeleton(person_mesh_, person_skeleton_id_,
                                           person_material_, people);
            }
            // 树：复用标定场已装载的 glTF 模型，整体平移到岩板顶上（不额外加资源）。
            for (const ModelSlot& m : models_) {
                cmds->DrawGltfObject(m.obj, {m.center.x(), top + m.center.y(), m.center.z()},
                                     m.up, m.front, m.scale);
            }
        } else if (draw_scene && scene_index_ == 1) {
            // 地面：按档位选材质。四段拼接时每段各画一遍（材质不同 ⇒ 必须是不同命令；
            // 属性 pass 会逐物体上传该段的 F0/roughness，合成层逐像素取 —— 这正是
            // 2026-11-02 那两条修正要验证的东西）。
            if (floor_preset_ == jpov_skylight::kFloorLadder) {
                for (int i = 0; i < jpov_skylight::kLadderSegments; ++i) {
                    const jpov::PBRMaterial mat = FloorMaterialOf(
                        jpov_skylight::kLadderMaterial[i]);
                    const float x = jpov_skylight::kLadderCenterX(i);
                    cmds->DrawObject3D(ground_strip_mesh_, mat,
                                       /*center*/ {x, 0.0f, 0.0f},
                                       /*up*/     {0.0f, 1.0f, 0.0f},
                                       /*front*/  {0.0f, 0.0f, 1.0f},
                                       /*scale*/  1.0f, /*highlight*/ false, /*picking_id*/ 0,
                                       /*wind*/ std::nullopt, /*ssr_reflector*/ true);
                }
            } else {
                // 整块地面：ground_mesh_ 是 40×40 的 quad（顶点 y=0），scale=5 ⇒ 200×200 m。
                const jpov::PBRMaterial ground_mat = floor_material_override_.has_value()
                    ? *floor_material_override_
                    : FloorMaterialOf(floor_preset_);
                cmds->DrawObject3D(ground_mesh_, ground_mat,
                                   /*center*/ {0.0f, 0.0f, 0.0f},
                                   /*up*/     {0.0f, 1.0f, 0.0f},
                                   /*front*/  {0.0f, 0.0f, 1.0f},
                                   /*scale*/ 5.0f, /*highlight*/ false, /*picking_id*/ 0,
                                   /*wind*/ std::nullopt, /*ssr_reflector*/ true);
            }
            for (const Clutter& c : clutter_) {
                cmds->DrawObject3D(box_mesh_, mat_clutter_, c.center,
                                   {0.0f, 1.0f, 0.0f}, c.front, c.scale);
            }
            // 点光源参照物：自发光小球（直径 = 2×physical_radius）。
            // 灯本身不是几何 —— 没有它，这张图就只剩「地上一块亮斑」，看不出亮斑是灯、
            // 更看不出「灯越大，像越大」。R=0（等效 δ）时不画（本来就没有尺寸）。
            if (point_lamp_.has_value() && point_lamp_->physical_radius > 0.0f) {
                jpov::PBRMaterial lamp_mat;
                lamp_mat.base_color = {0.0f, 0.0f, 0.0f, 1.0f};
                lamp_mat.emissive = point_lamp_->color;
                // box_mesh_ 是 1×1×1（半宽 0.5）⇒ scale = 直径 = 2R
                cmds->DrawObject3D(box_mesh_, lamp_mat, point_lamp_->position,
                                   /*up*/ {0.0f, 1.0f, 0.0f},
                                   /*front*/ {0.0f, 0.0f, 1.0f},
                                   /*scale*/ 2.0f * point_lamp_->physical_radius);
            }
            // 镜面皮肤的蓝人（**曲面**反射体）：单独一条命令（材质不同），带 ssr_reflector。
            // 4 个随机站位，固定种子；属性 pass 会为它们写逐顶点（曲面）视空间法线。
            if (person_mesh_ != 0 && !person_mirror_instances_.empty()) {
                cmds->DrawMeshWithSkeleton(person_mesh_, person_skeleton_id_, mat_mirror_skin_,
                                           person_mirror_instances_, /*highlight=*/false,
                                           /*ssr_reflector=*/true);
            }
            // 桌子/橡树也放上来：它们在镜面里的倒影比方块更"有内容"。
            for (const ModelSlot& m : models_) {
                cmds->DrawGltfObject(m.obj, m.center, m.up, m.front, m.scale);
            }
        } else if (draw_scene) {
            cmds->DrawObject3D(ground_mesh_, mat_ground_,
                               /*center*/ {0.0f, 0.0f, 0.0f},
                               /*up*/     {0.0f, 1.0f, 0.0f},
                               /*front*/  {0.0f, 0.0f, 1.0f});
            cmds->DrawObject3D(box_mesh_, mat_low_,   BoxCenter(0),
                               {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f});
            cmds->DrawObject3D(box_mesh_, mat_high_,  BoxCenter(1),
                               {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f});
            cmds->DrawObject3D(box_mesh_, mat_metal_, BoxCenter(2),
                               {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f});
            // 额外模型（桌子 / 橡树）：用来看“物体受光”，供夜色标定。
            for (const ModelSlot& m : models_) {
                cmds->DrawGltfObject(m.obj, m.center, m.up, m.front, m.scale);
            }
            // 蓝人：三份 T-pose 实例**一次** instanced draw（person_instances_ 拷贝传入，
            //   DrawMeshWithSkeleton 按值收 vector）。未装配时 person_mesh_==0 → 跳过。
            if (person_mesh_ != 0 && !person_instances_.empty()) {
                cmds->DrawMeshWithSkeleton(person_mesh_, person_skeleton_id_,
                                           person_material_, person_instances_);
            }
        }

        // 水体：世界属性，与“画不画场景”无关。
        //   · **界面层**（水皮）只要在水场景就有（它是世界的一部分）；
        //   · **介质**才受 `水体介质` 开关控制。
        // 两者分开 ⇒ A/B 的语义是「界面不变、只切介质」，不会把界面一起关掉。
        if (water_scene || ocean_lab) {
            jpov::WaterCommand w;
            w.level_y = ocean_lab ? water_cfg_.level_y : water_level_y_;
            // 水面时钟：**唯一出处**（WaterWaves 里已经没有 time_s）。
            // 选型场走真实时钟（"动态的海"）；冻结时用滑条秒数（可复现对帧）。
            {
                // 时间倍率对「冻结 / 实时」**都生效**：冻结时它只是把同一个时刻按倍率缩放
                //（= 慢动作/快进到同一相位），否则 --capture-ocean-report 的 speed 配置
                // 会变成空操作（曾经就是这样：冻结时直接走 water_wave_time_s_）。
                const float base_t = ocean_lab
                                         ? (water_wave_time_freeze_ ? water_wave_time_s_
                                                                    : OceanLabTime())
                                         : (water_wave_time_freeze_
                                                ? water_wave_time_s_
                                                : static_cast<float>(frame_count) / 60.0f);
                w.time_s = static_cast<double>(base_t * ocean_time_speed_);
            }
            w.depth_m = ocean_lab ? water_cfg_.depth_m : jpov::water::kOceanDeepDepthM;
            w.ior = ocean_lab ? water_cfg_.ior : water_ior_;
            w.skin = water_skin_on_;
            // 面板允许把 σ 全拖到 0；那等价于「没有介质」，不能让它撞 renderer 的 CHECK
            //（CHECK 是给 API 误用准备的，UI 不该能触发）。全 0 ⇒ 按关闭处理。
            const jpov::Vec3f sa = ocean_lab ? water_cfg_.sigma_a
                                             : jpov::Vec3f{water_sigma_a_r_, water_sigma_a_g_,
                                                           water_sigma_a_b_};
            const float ss = ocean_lab ? water_cfg_.sigma_s : water_sigma_s_;
            const bool sigma_nonzero =
                sa.x() > 0.0f || sa.y() > 0.0f || sa.z() > 0.0f || ss > 0.0f;
            w.medium.enabled = water_medium_on_ && sigma_nonzero;
            w.medium.sigma_a = sa;
            w.medium.sigma_s = {ss, ss, ss};
            w.medium.phase_g = ocean_lab ? water_cfg_.phase_g : water_phase_g_;
            // 波浪：只改界面几何（求交/法线）；介质仍按 level_y 半空间。
            if (ocean_lab) {
                // 选型场：谱整份由 WaterConfig 推出来（缓存，不每帧重算）。
                EnsureLabWater();
                jpov::WaterWaves wv = lab_waves_;
                for (int i = 0; i < wv.num_waves; ++i) {
                    wv.waves[i].amplitude_m *= water_wave_amp_scale_;
                }
                for (int i = 0; i < wv.num_detail_waves; ++i) {
                    wv.detail_waves[i].amplitude_m *= water_wave_amp_scale_;
                }
                wv.debug = (water_wave_debug_ != 0)
                               ? water_wave_debug_
                               : (water_wave_normal_view_ ? 1 : 0);
                wv.far_field = water_farfield_on_;
                wv.far_debug_yellow = water_farfield_yellow_;
                wv.far_reflect = water_farfield_reflect_;
                wv.far_specular = water_farfield_specular_;
                wv.far_alpha_albedo = water_farfield_alpha_;
                w.waves = wv;
            } else if (water_waves_on_) {
                jpov::WaterWaves wv = water_waves_;
                wv.enabled = true;
                int n = water_use_config_count_
                            ? water_waves_.num_waves
                            : static_cast<int>(water_wave_count_f_ + 0.5f);
                // 只有滑条路径才抬到 1（滑条最小 1 条）。配置给 0 条 = 只要细节波/平面：
                // 强行抬成 1 会凭空多一条 waves[0] 的默认波（10 m / 0.1 m），静默改画面。
                if (!water_use_config_count_ && n < 1) {
                    n = 1;
                }
                n = std::clamp(n, 0, jpov::WaterWaves::kMaxWaves);
                wv.num_waves = n;
                for (int i = 0; i < n; ++i) {
                    wv.waves[i].amplitude_m *= water_wave_amp_scale_;
                }
                for (int i = 0; i < wv.num_detail_waves; ++i) {
                    wv.detail_waves[i].amplitude_m *= water_wave_amp_scale_;
                }
                wv.debug = (water_wave_debug_ != 0)
                               ? water_wave_debug_
                               : (water_wave_normal_view_ ? 1 : 0);
                // 短波：方向 = 4 波方向的矢量和（波谱主方向），斜率 RMS 由滑条给。
                float sx = 0.0f;
                float sz = 0.0f;
                for (int i = 0; i < n; ++i) {
                    sx += wv.waves[i].dir_x;
                    sz += wv.waves[i].dir_z;
                }
                const float sl = std::sqrt(sx * sx + sz * sz);
                wv.short_wave.dir_x = (sl > 1.0e-6f) ? sx / sl : 1.0f;
                wv.short_wave.dir_z = (sl > 1.0e-6f) ? sz / sl : 0.0f;
                wv.short_wave.wind_speed_ms = water_wind_ms_;
                w.waves = wv;
            }
            cmds->water = w;
        }

        // SSR：把配置交给 Renderer（属性 pass / 降采样 / 模块 / 合成都在那边）。
        // 必须同时开环境镜面 —— 没有 env 就没有"可替换的那一项"（Renderer 会 FATAL）。
        if (ssr_on_ && env_specular_on_) {
            jpov::SsrConfig sc;
            sc.enabled = true;
            sc.resolution_scale = ssr_scale_;
            sc.thickness_pixels = ssr_thickness_px_;
            sc.max_ray_pixels = ssr_max_ray_px_;
            sc.roughness_to_lod = ssr_rough_to_lod_;
            sc.roughness_cutoff = ssr_rough_cutoff_;
            sc.debug_coverage = ssr_debug_;
            sc.grazing_fade_start = ssr_grazing_start_;
            sc.grazing_fade_end = ssr_grazing_end_;
            sc.fade_budget_pixels = ssr_fade_budget_px_;
            sc.fade_edge_pixels = ssr_fade_edge_px_;
            sc.fade_hit_edge_pixels = ssr_fade_hit_edge_px_;
            sc.filter_hole_fill = ssr_filter_hole_;
            sc.filter_outlier_clamp = ssr_filter_clamp_;
            sc.filter_atrous = ssr_filter_atrous_;
            sc.filter_atrous_iterations =
                static_cast<int>(std::lround(ssr_filter_atrous_iters_));
            sc.filter_clamp_sigma = ssr_filter_clamp_k_;
            sc.backface_use_ssr = ssr_backface_use_ssr_;
            sc.backface_dim = ssr_backface_dim_;
            cmds->ssr = sc;
        }

        // 光照面板（仅交互窗口；headless 拍摄是纯 3D 截图）。
        if (show_panel_) {
            DrawLightPanel(input);
            ui_.End();
            ui_.Emit(cmds);
        }
    }

private:
    static float AppTextWidth(const char* text, float font_size,
                              const char* /*font_alias*/, void* userdata) {
        SkylightApp* app = static_cast<SkylightApp*>(userdata);
        return app->MeasureTextWidth(/*alias=*/std::string(),
                                     /*text=*/text ? text : "", font_size);
    }

    // 光照面板（两列）：左列 = 五个天光自由度 + 三色 ambient 开关；右列 = 远景仰角雾
    // （开关 + 几何滑条 + 雾色色块）。雾色由天光推导，色块可目视核对“天光 → 雾色”的推理链。
    void DrawLightPanel(const jpov::InputSnapshot& input) {
        const float w = static_cast<float>(kViewerWidth);
        const float h = static_cast<float>(kViewerHeight);
        jpov::UiTheme theme = jpov::UiTheme::Default(kSliderFontSize);
        theme.font_alias = kViewerFontAlias;
        const float frame_dt_ms = 1000.0f / kViewerFps;
        ui_.Begin(input, theme, w, h, frame_dt_ms);

        const float kColW    = 0.45f * w;
        const float kColX[2] = {0.025f * w, 0.515f * w};
        const float kRowH    = 24.0f;
        const float kSpacing = 5.0f;
        const float kBottom  = 16.0f;
        // 行数**由面板自己声明**，不再手维护（2026-11-24 第二次踩坑：加控件忘了加行数，
        // 水面/水位那两格直接画到屏幕外）。面板底对齐 ⇒ 行数必须 =「该面板最大行号 + 1」，
        // 多给的行变成顶部一片空白。
        // ⚠️ 改某个面板的 row 索引时，只需改对应的那个常量。
        constexpr int kOceanLabPanelRows = 16;   // 场景4：row(0,0..13) + row(1,0..15)
        constexpr int kWaterPanelRows    = 16;   // 场景3：row(0,0..13) + row(1,0..15)
        const int kRows = IsOceanLab()   ? kOceanLabPanelRows
                        : IsWaterScene() ? kWaterPanelRows
                                         : 17;
        const float top      = h - kBottom
                             - (static_cast<float>(kRows) * kRowH
                                + static_cast<float>(kRows - 1) * kSpacing);

        auto row = [&](int col, int i) {
            return jpov::UiRect{{kColX[col], top + static_cast<float>(i) * (kRowH + kSpacing)},
                                {kColW, kRowH}};
        };

        // ── 左列 0..2：场景下拉 + SSR 总闸 / 调试视图 ──
        // 放在列首（2026-10-30 Danis：「下拉菜单不要放下面，一旦下拉会出屏幕」）：
        // Ui::Combo 的下拉列表恒朝下展开（框底 = 列表顶），贴底的行一展开就出屏；
        // 列首下方空间充足，两个下拉都能完整展开。
        {
            const std::vector<const char*> scenes = {"场景0 标定场（方块+人）",
                                                     "场景1 镜面地板+随机模型",
                                                     "场景2 只天空",
                                                     "场景3 水凝胶（水体介质）",
                                                     "场景4 海面选型（WaterConfig）"};
            int sel = scene_index_;
            if (ui_.Combo("场景", &sel, scenes, row(0, 0))) {
                scene_index_ = sel;
                if (sel == 1) {
                    // 切到镜面场景给一台低角度相机：掠射最能暴露自交/厚度问题。
                    view_.phi = 0.21;   // ≈12° 仰角
                    view_.theta = 0.0;
                    view_.R = 45.0;
                } else if (sel == kSceneWater) {
                    // 水场景：俯视看岩板（深度 × 颜色的梯度最好看），太阳偏侧。
                    view_.phi = 0.62;   // ≈35° 仰角
                    view_.theta = 0.6;
                    view_.R = 26.0;
                }
            }

            // 水场景：面板只留「水体介质 + 天体光照」（验收物理口径，不是调参手感）。
            // ── 场景 4：海面选型场（只碰 WaterConfig）──
            // 想告诉美术的一句话：「风多大、吹哪边、风区多大、有没有涌」。
            // 谱怎么离散、相位怎么随机、细节多少条，全自动。
            if (IsOceanLab()) {
                // 风速 / 风向（面板用度数，写回 WaterConfig 的向量；方向 = 吹向）
                constexpr float kDeg = 3.14159265358979323846f / 180.0f;
                float wind_speed = water_cfg_.WindSpeedMs();
                float wind_dir_deg = water_cfg_.WindDirRad() / kDeg;
                if (ui_.SliderFloat("风 风速 m/s", &wind_speed, row(0, 1), 0.0f, 25.0f, 1)) {
                    water_cfg_.SetWind(wind_speed, wind_dir_deg * kDeg);
                    InvalidateLabWater();
                }
                if (ui_.SliderFloat("风 吹向 °", &wind_dir_deg, row(0, 2), 0.0f, 360.0f, 0)) {
                    water_cfg_.SetWind(wind_speed, wind_dir_deg * kDeg);
                    InvalidateLabWater();
                }
                // 风区：0 = 关闭限制（充分成长）。面板上用 0 表示 ∞，写回时转成 infinity。
                float fetch_km = std::isfinite(water_cfg_.fetch_km) ? water_cfg_.fetch_km : 0.0f;
                if (ui_.SliderFloat("风 风区 km (0=充分成长)", &fetch_km, row(0, 3),
                                    0.0f, 1000.0f, 0)) {
                    water_cfg_.fetch_km = (fetch_km < 0.5f)
                                              ? std::numeric_limits<float>::infinity()
                                              : fetch_km;
                    InvalidateLabWater();
                }
                // 涌
                if (ui_.Checkbox("涌 开启", &ocean_swell_on_, row(0, 4)) && ocean_swell_on_ &&
                    !(water_cfg_.swell.height_m > 0.0f)) {
                    // 打开涌时给一组能肉眼看出"交叉"的默认值：1.5 m / 11 s，与风偏 70°。
                    constexpr float kDeg = 3.14159265358979323846f / 180.0f;
                    water_cfg_.SetSwell(1.5f, 11.0f,
                                        water_cfg_.WindDirRad() * kDeg + 70.0f * kDeg);
                    InvalidateLabWater();
                }
                float swell_hs = water_cfg_.swell.height_m;
                if (ui_.SliderFloat("涌 波高 m", &swell_hs, row(0, 5), 0.0f, 5.0f, 2)) {
                    water_cfg_.swell.height_m = ocean_swell_on_ ? swell_hs : 0.0f;
                    InvalidateLabWater();
                }
                float swell_tp = water_cfg_.swell.period_s;
                if (ui_.SliderFloat("涌 周期 s", &swell_tp, row(0, 6), 4.0f, 20.0f, 1)) {
                    water_cfg_.swell.period_s = swell_tp;
                    InvalidateLabWater();
                }
                float swell_dir = water_cfg_.SwellDirRad() / kDeg;
                if (ui_.SliderFloat("涌 传向 °", &swell_dir, row(0, 7), 0.0f, 360.0f, 0)) {
                    water_cfg_.SetSwell(water_cfg_.swell.height_m, water_cfg_.swell.period_s,
                                        swell_dir * kDeg);
                    InvalidateLabWater();
                }
                // 介质（水色 = 逐通道吸收；浑浊 = 散射）
                if (ui_.SliderFloat("水 吸收 R 1/m (水色 蓝↔绿)", &water_cfg_.sigma_a.x(),
                                    row(0, 8), 0.0f, 1.5f, 3)) {
                    InvalidateLabWater();
                }
                ui_.SliderFloat("水 吸收 G 1/m", &water_cfg_.sigma_a.y(), row(0, 9),
                                0.0f, 0.3f, 3);
                ui_.SliderFloat("水 吸收 B 1/m", &water_cfg_.sigma_a.z(), row(0, 10),
                                0.0f, 0.1f, 3);
                ui_.SliderFloat("水 散射 1/m (浑浊)", &water_cfg_.sigma_s, row(0, 11),
                                0.0f, 0.5f, 3);
                ui_.SliderFloat("水 水深 m (浅海)", &water_cfg_.depth_m, row(0, 12),
                                0.3f, 40.0f, 1);
                ui_.SliderFloat("曝光", &exposure_, row(0, 13), 0.2f, 2.0f, 2);

                // 右列：环境/时间/开关 + 只读派生量
                ui_.SliderFloat("天体仰角 ° (负=月光/正=日光)", &deg_.sun_elev_deg, row(1, 0),
                                -90.0f, 90.0f, 0);
                ui_.SliderFloat("天体方位角 °", &deg_.sun_azim_deg, row(1, 1), 0.0f, 360.0f, 0);
                ui_.SliderFloat("浊度 turb", &deg_.turbidity, row(1, 2), kTurbidityMin,
                                kTurbidityMax, 1);
                ui_.Checkbox("水皮界面层 (WBO)", &water_skin_on_, row(1, 3));
                ui_.Checkbox("水体介质 (吸收+注入)", &water_medium_on_, row(1, 4));
                ui_.SliderFloat("主波波幅倍率（调试）", &water_wave_amp_scale_, row(1, 5),
                                0.0f, 3.0f, 2);
                ui_.SliderFloat("时间速度 (0=冻结)", &ocean_time_speed_, row(1, 6), 0.0f, 3.0f, 2);
                ui_.SliderFloat("时间 s（冻结时）", &water_wave_time_s_, row(1, 7), 0.0f, 120.0f, 2);
                if (ui_.Checkbox("冻结时间", &water_wave_time_freeze_, row(1, 8))) {
                    // 不额外做事：time_s 的取值在渲染体里按开关分叉
                }
                // 远海（只作用于水上）：二分流，远场走独立着色路径。
                ui_.Checkbox("远海 开启 (水上)", &water_farfield_on_, row(1, 11));
                ui_.Checkbox("远海 涂黄(看边界)", &water_farfield_yellow_, row(1, 12));
                ui_.Checkbox("远海 +反射(平面镜 env)", &water_farfield_reflect_, row(1, 13));
                ui_.Checkbox("远海 +高光(平面镜 太阳)", &water_farfield_specular_, row(1, 14));
                ui_.Checkbox("远海 alpha=方向反照率", &water_farfield_alpha_, row(1, 15));
                // 只读派生量（把"自动推出来的东西"亮出来，否则用户不知道自己调出了什么海）
                EnsureLabWater();
                {
                    char buf[160];
                    std::snprintf(buf, sizeof(buf), "Hs %.2f + %.2f = %.2f m",
                                  static_cast<double>(lab_readout_.hs_wind_m),
                                  static_cast<double>(lab_readout_.hs_swell_m),
                                  static_cast<double>(lab_readout_.hs_total_m));
                    ui_.Text(buf, row(1, 9));
                    std::snprintf(buf, sizeof(buf), "Tp %.1f s   λp %.0f m  坡度 %.0f°",
                                  static_cast<double>(lab_readout_.period_s),
                                  static_cast<double>(lab_readout_.lambda_peak_m),
                                  static_cast<double>(
                                      std::atan(lab_readout_.rms_slope) * 57.29577951308232));
                    ui_.Text(buf, row(1, 10));
                }
                // 场景 4 的面板到这里就结束：**必须早退**，否则会继续画下面那套
                // 「SSR / 天光自由度」通用面板，它们用的也是 row(0,1..15) / row(1,0..)，
                // 两套控件叠在同一批行上 ⇒ 文字互相重叠、完全不可读
                //（2026-11-24 Danis 报障：「场景4 UI 重叠，字都混在一起」）。
                return;
            }

            if (IsWaterScene()) {
                ui_.Checkbox("水体介质 (吸收+注入)", &water_medium_on_, row(0, 1));
                ui_.SliderFloat("吸收 R 1/m", &water_sigma_a_r_, row(0, 2), 0.0f, 1.0f, 3);
                ui_.SliderFloat("吸收 G 1/m", &water_sigma_a_g_, row(0, 3), 0.0f, 1.0f, 3);
                ui_.SliderFloat("吸收 B 1/m", &water_sigma_a_b_, row(0, 4), 0.0f, 1.0f, 3);
                ui_.SliderFloat("散射 1/m (灰)", &water_sigma_s_, row(0, 5), 0.0f, 1.0f, 3);
                ui_.SliderFloat("相位 g (前向散射)", &water_phase_g_, row(0, 6), 0.0f, 0.9f, 2);
                ui_.SliderFloat("岩板顶面深度 m", &slab_top_depth_m_, row(0, 7), 0.5f, 20.0f, 2);
                ui_.SliderFloat("天体仰角 ° (负=月光/正=日光)", &deg_.sun_elev_deg,
                                row(1, 0), -90.0f, 90.0f, 0);
                ui_.SliderFloat("天体方位角 °", &deg_.sun_azim_deg, row(1, 1), 0.0f, 360.0f, 0);
                ui_.SliderFloat("季节色温 日光", &deg_.daylight_season, row(1, 2),
                                -1.0f, 1.0f, 2);
                ui_.SliderFloat("浊度 turb", &deg_.turbidity, row(1, 3),
                                kTurbidityMin, kTurbidityMax, 1);
                ui_.SliderFloat("水折射率 ior", &water_ior_, row(1, 4), 1.0f, 2.0f, 3);
                ui_.SliderFloat("水位 y (m)", &water_level_y_, row(1, 5), -6.0f, 6.0f, 2);

                // ── 水面波浪（part 1：解析谱 + 水皮真位移；本版不含 flow map）──
                ui_.Checkbox("水面波浪", &water_waves_on_, row(0, 8));
                ui_.SliderFloat("波浪 波幅倍率", &water_wave_amp_scale_, row(0, 9),
                                0.0f, 6.0f, 2);
                // 时间倍率改成"调用方对 time 的缩放"，不再是波谱里的字段。
                ui_.SliderFloat("波浪 时间速度 (0=冻结)", &ocean_time_speed_,
                                row(0, 10), 0.0f, 3.0f, 2);
                ui_.Checkbox("波浪 真位移 (关=只改法线)", &water_waves_.displacement,
                             row(0, 13));
                // 左列最后一格：水皮开关（与 row(0,1) 的「水体介质」正交）。
                //   水皮 = 界面层 WBO（反射 / 透射 / 水面高光 / Snell 窗）；
                //   介质 = 水下体积（吸收 + 单次散射注入）。
                ui_.Checkbox("水皮界面层 (WBO)", &water_skin_on_, row(0, 15));
                // 短波：只进 MSS（反射率/透射率重分配）。F_eff 本身没有开关 —— 永远开。
                ui_.SliderFloat("风速 m/s (短波方差)", &water_wind_ms_, row(0, 16), 0.0f, 20.0f, 1);


                // 时间：冻结 + 精确秒数（报障复现用）
                ui_.Checkbox("波浪 冻结时间", &water_wave_time_freeze_, row(1, 14));
                ui_.SliderFloat("波浪 时间 s", &water_wave_time_s_, row(1, 15), 0.0f, 120.0f, 2);
                return;
            }
            ui_.Checkbox("SSR 屏幕空间反射", &ssr_on_, row(0, 1));
            // 5 档与 SsrConfig::debug_coverage 一一对应（3/4 以前只能在拍摄脚本里用）。
            const std::vector<const char*> dbg = {"SSR 调试：关",
                                                  "SSR 调试：A 覆盖率",
                                                  "SSR 调试：直通（无 SSR）",
                                                  "SSR 调试：SSR 辐亮度",
                                                  "SSR 调试：A 置信度（含掠射淡出）",
                                                  "SSR 调试：背面命中掩码"};
            int dsel = ssr_debug_;
            if (ui_.Combo("SSR 调试", &dsel, dbg, row(0, 2))) {
                ssr_debug_ = dsel;
            }

        }

        // ── 左列 3..13：天光自由度 ──
        ui_.SliderFloat("浊度 turb", &deg_.turbidity, row(0, 3),
                        kTurbidityMin, kTurbidityMax, 1);
        ui_.SliderFloat("季节色温 日光 (左蓝偏..右红偏)", &deg_.daylight_season,
                        row(0, 4), -1.0f, 1.0f, 2);
        ui_.SliderFloat("天体仰角 ° (负=月光/正=日光)", &deg_.sun_elev_deg,
                        row(0, 5), -90.0f, 90.0f, 0);
        ui_.SliderFloat("天体方位角 °", &deg_.sun_azim_deg, row(0, 6), 0.0f, 360.0f, 0);
        ui_.SliderFloat("月色变红 (血月)", &deg_.moon_red, row(0, 7), 0.0f, 1.0f, 2);
        ui_.SliderFloat("夜空偏蓝 (梦幻夜)", &deg_.night_blue, row(0, 8), 0.0f, 1.0f, 2);
        ui_.Checkbox("三色环境光 (天/天际线/地)", &tricolor_ambient_, row(0, 9));
        // SMAA 后处理抗锯齿：与右侧“远景仰角雾”并列，单独对比开关 + 阈值（敏感度）。
        ui_.Checkbox("SMAA 抗锯齿", &smaa_on_, row(0, 10));
        ui_.SliderFloat("SMAA 阈值 (小=更敏感)", &smaa_threshold_, row(0, 11),
                        0.02f, 0.5f, 2);
        // 环境镜面（IBL specular）：验收工件是金属块（index 2）。
        // 关 = 只有直射高光（发黑）；开 = 能看见天光渐变与地色被映上去。
        ui_.Checkbox("环境镜面 (天光反射)", &env_specular_on_, row(0, 12));
        ui_.SliderFloat("天光消费量 [0,1]", &env_specular_scale_, row(0, 13),
                        0.0f, 1.0f, 2);

        // ── 左列 14..15：遮丑滤波器 ①②（2026-11-13）──
        // 三个 toggle 完全独立，可以逐条对比；③ 的趟数放在右列 8。
        ui_.Checkbox("遮丑① 补洞 (1px 缝)", &ssr_filter_hole_, row(0, 14));
        ui_.Checkbox("遮丑② 离群抑制 (方差)", &ssr_filter_clamp_, row(0, 15));

        // 远景雾：整条功能已删除（2026-11-23 Danis：不值得为这点收益多一层心智负担，
        // 尤其是还要牵涉透明物体的各类合成）。需要地平线发白时，摆一个大薄板 PointFog；
        // 见 render_command.h 的 PointFog 头注释（注意它吃光照、占每 tile 预算）。

        // ── 右列 0..2：掠射淡出（2026-11-13）──
        // 放在列首：不受 SSR 总闸影响也能看见（关掉 SSR 时它不生效，但不会"找不到"）。
        // 掠射淡出：只有两个阈值（end <= start 即关闭）。
        ui_.SliderFloat("掠射淡出起点 kappa", &ssr_grazing_start_, row(1, 0), 1.0f, 60.0f, 1);
        ui_.SliderFloat("掠射淡出终点 kappa", &ssr_grazing_end_, row(1, 1), 1.0f, 60.0f, 1);

        // ── 右列 4..6：A 的三个淡出（2026-11-13，遮丑）──
        // 单位 = 全分辨率屏幕像素；0 = 关闭该条。三条相乘（与上面两个 kappa 也是相乘）。
        // ① 预算：光靠"快走到预算上限"就压 A —— 擦边命中不再是一根刺。
        // ② / ③ 贴边：P（倒影显示位置）和 Q（反射源）各自一条缓冲带。
        ui_.SliderFloat("A 淡出·预算带 px", &ssr_fade_budget_px_, row(1, 4), 0.0f, 120.0f, 0);
        ui_.SliderFloat("A 淡出·反射体贴边 px", &ssr_fade_edge_px_, row(1, 5), 0.0f, 240.0f, 0);
        ui_.SliderFloat("A 淡出·命中点贴边 px", &ssr_fade_hit_edge_px_, row(1, 6), 0.0f, 240.0f, 0);

        // ── 右列 7..8：遮丑滤波器 ③（2026-11-13）──
        ui_.Checkbox("遮丑③ 边缘保持小波", &ssr_filter_atrous_, row(1, 7));
        ui_.SliderFloat("小波趟数 (1..3)", &ssr_filter_atrous_iters_, row(1, 8), 1.0f, 3.0f, 0);

        // ── 右列 2..3：背面命中的两个正交 flag（2026-11-13）──
        // ① 来源：关 = 交回 env（旧行为）；开 = 用 SSR 给出的那份颜色。
        // ② 亮度因子：只看"最终采用的那份颜色"，1 = 不压暗。
        //    与①正交：(关,0.2) = 环境项只保留 20%；(开,0.25) = 背面颜色整体压暗。
        ui_.Checkbox("背面命中用 SSR 颜色（关=用 env）", &ssr_backface_use_ssr_, row(1, 2));
        ui_.SliderFloat("背面命中压暗比 (1=不压暗)", &ssr_backface_dim_, row(1, 3),
                        0.0f, 1.0f, 2);

        // ── 右列 9..15：地面材质（下拉）+ SSR 的分辨率 / 厚度 / 预算 / 磨砂 ──
        {
            // 地面材质下拉（2026-11-02）：镜面 / 金属 / 光滑石砖 / 粗糙岩石 / 四段拼接。
            // 材质**逐物体**进属性纹理 ⇒ 四段拼接里每一段各用各的 F0/roughness，
            // 这是"同屏多反射材质"这条通路的唯一验收手段。
            std::vector<const char*> floors(jpov_skylight::kFloorPresetNames,
                                            jpov_skylight::kFloorPresetNames
                                                + jpov_skylight::kFloorPresetCount);
            int fp = floor_preset_;
            if (ui_.Combo("地面材质", &fp, floors, row(1, 9))) {
                floor_preset_ = fp;
            }
            // 降采样倍率滑条 N（1..4）：分辨率 = 1/N。N=3 也能跑（整数格 + 加权，不要求 2 的幂）。
            float n_f = static_cast<float>(ssr_downscale_n_);
            if (ui_.SliderFloat("SSR 降采样倍率 1:N", &n_f, row(1, 10), 1.0f, 4.0f, 0)) {
                ssr_downscale_n_ = static_cast<int>(std::lround(n_f));
                ssr_downscale_n_ = std::max(1, std::min(4, ssr_downscale_n_));
                ssr_scale_ = 1.0f / static_cast<float>(ssr_downscale_n_);
            }
            ui_.SliderFloat("SSR 厚度 N_px", &ssr_thickness_px_, row(1, 12), 1.0f, 24.0f, 1);
            float mrp = static_cast<float>(ssr_max_ray_px_);
            if (ui_.SliderFloat("SSR 射线预算 px", &mrp, row(1, 13), 40.0f, 800.0f, 0)) {
                ssr_max_ray_px_ = static_cast<int>(mrp);
            }
            // 磨砂强度：roughness² × 该值 = 采样的 mip 级。0 = 永远锐利（旧行为）。
            ui_.SliderFloat("SSR 磨砂强度 (模糊斜率)", &ssr_rough_to_lod_, row(1, 14),
                            0.0f, 8.0f, 2);
            // 发射线门槛：超过它的粗糙度直接用 env（省射线，且本来也糊得看不出形状）。
            ui_.SliderFloat("SSR 发射线门槛 roughness", &ssr_rough_cutoff_, row(1, 15),
                            0.2f, 1.0f, 2);
        }
    }

    bool show_panel_ = true;
    bool show_scene_ = true;
    bool tricolor_ambient_ = false;   // 三色环境光开关（默认关=原单色，零回归）
    // 调试用光照覆盖（仅拍摄；见 SetAmbientOverride 的注释）。未设 = 用天际推导的默认值。
    std::optional<jpov::AmbientLight> ambient_override_;
    std::optional<jpov::DirectionalLight> sun_override_;
    bool env_specular_on_ = true;     // 环境镜面开关（默认开：本 PR 的主交付）
    float env_specular_scale_ = 1.0f; // 天光消费量 [0,1]（1 = 全部消费天光）
    bool smaa_on_ = false;            // SMAA 抗锯齿开关（默认关=零回归；交互面板可开）
    float smaa_threshold_ = 0.1f;     // SMAA luma 边缘阈值（越小越敏感）
    jpov::Ui ui_;

    static constexpr float kSliderFontSize = 15.0f;
};

}  // namespace jpov_skylight

#endif  // JPOV_DEMO_SKYLIGHT_VIEWER_APP_H_
