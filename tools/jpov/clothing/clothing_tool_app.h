// JPOV 穿衣工具 — 渲染核心 App（可视化框架 + 软体仿真）
//
// 与 jpov_soft_mesh_viewer / jpov_model_editor 是**姊妹工具**（同款渲染核心骨架：
// 场景静态资源 + 交互面板宿主 + 唯一 OneIteration 渲染体），职责聚焦：把「一件人体
// reference」与「一件衣服模型」加载进同一场景显示，并让衣服**被软体仿真器驱动**
// （重力 + 顶点间弹簧力场 + 地面投影），为后续「衣服贴合人体」的穿衣管线提供底座。
//
// 能力演进：
//   - M0（2026-09-27）：加载 + 显示。
//   - 2026-09-29：后台建图（最近邻三角形）+ 左上角 x/y/z 粗调。
//   - 2026-09-30：完整变换调节（平移 / 旋转 / 缩放；一律直接改衣服 mesh 顶点）+ 保存 glb。
//   - 2026-10-01（本 PR，Step 1）：**纳入软体仿真器**——
//       ① 衣服几何被 soft_mesh_simulator::Simulator 驱动（每 primitive 一个仿真器）；
//       ② 右上角面板控制动力学系数（重力 g / 总质量 M / 力系数 F / 衰减 k）；
//       ③ 「暂停 / 继续」与「重置衣服」两个按钮（重置回到**启动时**的原始几何）；
//       ④ 删除「绝对平移」输入框——平移 / 旋转 / 缩放改为**即时作用于仿真器状态**
//          （见 Simulator::ApplyTranslation/ApplyRotation/ApplyScaling）：仿真进行中也能调，
//          不中断（位置与绑定姿态同步变换；速度只随旋转转动）。
//       ⑤ Step 2：**人体排斥**（glb_repulsion 设计）——右上角开关 + buffer(0~0.1m，默认 0.01)；
//          衣服被人体顶开、不穿模。另加全局「速度上限」兑底（低质量数值失稳）。
//   - 2026-10-02：人体排斥强化——① 逃逸改用「入射线段二分」（新穿入按 [x(t),x(t+dt)]
//       二分 10 轮取边界点；仿真前已在体内则用旧投影）；② buffer 重新启用（作为“带 buffer 的
//       体内判定”）；③ 新增切向速度保留系数（默认 1.0）；④ 关联图连通性修复；
//       ⑤ 修「重置未能真正重置」（Reset 改用独立启动快照 startup_positions_）。
//   仍不做：**穿衣对齐 / 自动贴合**（其余功能已接）。
//
// 与 soft_mesh_viewer 的关键差异：
//   - 人体 reference 与衣服都是**静态 glTF 资产**（LoadGltf → GltfObject 画）；
//     衣服侧额外保留 CPU 几何（clothing_init 后台加载时顺带保留）用于变换 / 保存 / 仿真。
//   - 衣服几何每帧可能被仿真改写 → 必须 UpdateMesh 把新顶点推上 GPU（GltfObject 进
//     GPU 后几何就固化了，塞不进动态形变）。
//
// 坐标空间（⚠️ Danis 特别提醒：别把「窗口尺寸」和「3D FBO」搞混）：
//   - 2D 面板/文字画在**主 FBO**上，其尺寸 = **本帧窗口尺寸** winfo.width/height
//     （JPOV 每帧 `BeginFrame(winfo.width, winfo.height)`）；随窗口 resize 而变。
//     ⇒ 面板布局**必须**从 winfo 推算（不能用常量），否则窗口一变面板就飘。
//   - 3D 场景渲染到 **3D FBO**（cmds->camera.fbo_3d_width_/height_）。本工具让它
//     **跟随窗口尺寸**（同 model_editor 的做法），避免 resize 时 3D 被拉伸。
//
// 命名空间 jpov::clothing、文件夹 tools/jpov/clothing/ 均为**独立**的一整套。

#ifndef JPOV_CLOTHING_CLOTHING_TOOL_APP_H_
#define JPOV_CLOTHING_CLOTHING_TOOL_APP_H_

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

#include <glog/logging.h>

#include "tools/jpov/clothing/clothing_axis_input.h"
#include "tools/jpov/clothing/clothing_init.h"
#include "tools/jpov/clothing/clothing_save.h"
#include "tools/jpov/clothing/clothing_transform.h"
#include "tools/jpov/clothing/weight_transfer.h"
#include "tools/jpov/demo/skylight_scene.h"
#include "tools/jpov/demo/view_config.h"
#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/interface/skeleton_types.h"
#include "tools/jpov/interface/ui.h"
#include "tools/jpov/soft_mesh_simulator/soft_mesh_simulator.h"
#include "tools/jpov/src/gltf_loader.h"

namespace jpov {
namespace clothing {

// 视角 / 光照 / 地面工具：复用姊妹查看器的纯函数（ViewConfig、ApplyInput、
// MakeGroundQuad、GroundMaterial 在 jpov_viewer 命名空间）。
using jpov_viewer::ApplyInput;
using jpov_viewer::DefaultView;
using jpov_viewer::GroundMaterial;
using jpov_viewer::MakeGroundQuad;
using jpov_viewer::ViewConfig;
// 软体仿真器（纯 CPU / GL-free，独立包）。
using soft_mesh_simulator::Axis;
using soft_mesh_simulator::Simulator;

// 默认窗口尺寸（= headless 出图尺寸）。⚠️ 这只是**初始**尺寸；运行时窗口可 resize，
// 实际每帧尺寸以 winfo.width/height 为准（见文件头坐标空间说明）。**不要**拿它当
// 面板布局的依据（那是"窗口尺寸/分辨率" = winfo 的职责）。
inline constexpr int kDefaultWindowWidth  = 1280;
inline constexpr int kDefaultWindowHeight = 720;

// 交互帧率（查看器刷新率，Hz）。仿真步长固定为 Simulator::kDefaultDt（1/60 s）。
inline constexpr float kViewerFps = 60.0f;

// UI 文本默认字体 = CJK（面板标签显中文）。
inline constexpr const char* kViewerFontAlias = jpov::kFontBuiltinCJK;

// 未显式指定 --body_reference_path 时的演示用人体 reference。
// 项目内自带的 Mixamo 男性人体资产（rest/T-pose），便于快速跑通。
inline constexpr const char* kDefaultBodyReferencePath =
    "tools/jpov/test/object3d/mixamo_male/mixamo_male.glb";

// 数值输入框文本容量（含终止符）。填值格式如 "-0.35" / "45"，64 字节足够。
inline constexpr size_t kAxisInputCapacity = 64;

// 步长 / 系数的初始默认值（Danis 2026-09-30 指定）：平移步长 0.1 米、旋转步长 45 度；
// 缩放系数区间是 [1.0, 2.0]，取 1.1 作为既能变大又能变小的中性默认。
inline constexpr float kDefaultTransStep = 0.1f;
inline constexpr float kDefaultRotStep = 45.0f;
inline constexpr float kDefaultScaleStep = 1.1f;

// 默认全局速度上限（m/s）。低质量数值失稳的兑底（Danis 2026-10-01：把 M 调小后布料被
// 甩飞、拉成“淌”状长条）。0 = 不限；实测 25 m/s 能在保留正常下落（~8 m/s）的同时挡住失稳。
inline constexpr float kDefaultMaxSpeed = 25.0f;

// ── 软布自动蒙皮（weight transfer）参数 ──
// gap 阈值：衣物顶点到身体最近距离 > 该值即视为“不贴合”（宽松/缝线悬挂），走兜底
// 权重并被计数。5mm（设计文档 §3.3 建议 3~5mm 取上界，包容贴身件的数值误差）。
inline constexpr float kSkinGapThresholdM = 0.005f;
// 每顶点最大影响骨数（与 MeshData 的 4 组 joint/weight 对齐）。
inline constexpr int kSkinMaxInfluences = 4;
// 打开“平滑权重”时的拉普拉斯迭代次数（消关节附近条带）。
inline constexpr int kSkinSmoothPasses = 2;

// 一个数值输入框的跨帧状态：文本缓冲 + 上一帧聚焦态。
// 聚焦态用于检测"回车 / 焦点丧失"这一提交边界（InputText 返回的是"帧末是否聚焦"）。
struct NumberField {
    char text[kAxisInputCapacity];
    bool focused_prev = false;

    NumberField() { text[0] = '\0'; }
    // 从初始数值构造：文本用 "%g" 规范化，保证与默认常量（如 kDefaultRotStep）**单一来源**。
    explicit NumberField(float init) {
        snprintf(text, kAxisInputCapacity, "%g", static_cast<double>(init));
    }
};

// 穿衣工具渲染核心 App。
//
// 场景 = 一件人体 reference + 一件衣服 + 灰地面；光照固定 45° 天光 + 三色 ambient。
// 视角靠鼠标操作（右键 drag 转、滚轮 zoom），与姊妹查看器完全同款。
class ClothingToolApp : public JPOV {
public:
    using JPOV::JPOV;

    // ── 场景状态（main 在 Init() 后装配）──
    jpov::GltfObject body_;        // 人体 reference（--body_reference_path）
    jpov::GltfObject cloth_;       // 衣服模型（--cloth_path）
    std::string body_path_;        // 人体 reference 来源路径（面板显示）
    std::string cloth_path_;       // 衣服模型来源路径（面板显示 + 保存定位）

    // ── 后台初始化（建最近邻三角形匹配器；顺带保留衣服 CPU 几何）──
    ClothingInitController init_;

    uint32_t ground_mesh_ = 0;           // 300×300 地面 quad 的 GPU handle
    jpov::PBRMaterial ground_mat_;       // 高粗糙灰色地面材质

    // ── 当前视角：交互与 headless 出图共用的单一事实源。──
    ViewConfig view_;

    // ── 显示开关（面板勾选）──
    bool show_body_  = true;   // 画人体 reference
    bool show_cloth_ = true;   // 画衣服模型

    // 地面高度（米）滑条值，[-3, +3]。同时写入仿真器的物理地面（同值）。
    float ground_y_ = -3.0f;

    // ══════════════ 衣服几何（当前态快照）══════════════
    //
    // **唯一事实源是仿真器**（sims_[i].mesh()）：平移 / 旋转 / 缩放、仿真推进都直接作用于
    // 仿真器的内部状态（见 Simulator::ApplyTranslation 等），随后把结果同步过来显示 / 保存。
    // cloth_current_ 只是「当前几何快照」，供保存 glb（main 线程按值取走）。
    std::vector<jpov::GltfSaveMesh> cloth_current_;     // 当前态（显示 / 保存用）

    // 各步长 / 系数（供步进按钮使用；用户可在面板里用输入框改，提交时 clamp）。
    float trans_step_[3] = {kDefaultTransStep, kDefaultTransStep, kDefaultTransStep};
    float rot_step_[3]   = {kDefaultRotStep, kDefaultRotStep, kDefaultRotStep};
    float scale_step_    = kDefaultScaleStep;
    // 累计缩放系数（相对启动几何），仅用于把整体缩放夹在 [kClothScaleMin, kClothScaleMax]。
    float cloth_scale_   = 1.0f;

    // 面板数值输入框的跨帧文本 + 聚焦态（初始值来自上面的默认常量，避免字面量分叉）。
    NumberField trans_step_field_[3] = {NumberField(kDefaultTransStep),
                                        NumberField(kDefaultTransStep),
                                        NumberField(kDefaultTransStep)};
    NumberField rot_step_field_[3] = {NumberField(kDefaultRotStep),
                                      NumberField(kDefaultRotStep),
                                      NumberField(kDefaultRotStep)};
    NumberField scale_step_field_ = NumberField(kDefaultScaleStep);

    // 衣服保存控制器（后台线程写 glb）。
    ClothingSaveController save_ctrl_;

    // ── 软布自动蒙皮（weight transfer）状态 ──
    // 人体 reference 的骨架：蒙皮产出的权重按该骨架的关节序索引，保存时写进 glb 的 skin。
    jpov::SkeletonType body_skeleton_;
    bool has_body_skeleton_ = false;   // body_skeleton_ 是否有效（人体 glb 带 skin）
    // 已自动蒙皮：此后**冻结几何**（禁变换/禁仿真/禁重置），否则权重↔顶点对应被破坏。
    bool skinned_ = false;
    std::string skin_msg_;             // 蒙皮结果 / 失败提示（面板显示）
    bool skin_smooth_ui_ = true;       // 「平滑权重」勾选（默认开）

    // ══════════════ 软体仿真（Step 1）══════════════
    //
    // 每 clothes primitive 一个仿真器（多为单 primitive）。生命周期：
    //   - 场景就绪（GPU 上传）后按启动几何 Init（绑定姿态）；
    //   - 平移 / 旋转 / 缩放**即时作用于仿真器状态**（不重建、不中断，见 ApplyXxx）；
    //   - 「重置」= sim.Reset()（回绑定姿态）；「推进仿真」时逐帧 Step。
    //   ⇒ 仿真中也能点击变换，衣服会带着速度继续演化。
    std::vector<Simulator> sims_;

    bool sim_running_ = false;       // 是否推进仿真（暂停按钮的反相）

    // 动力学滑条镜像值（UI 写、每帧同步到仿真器）。
    float gravity_ui_ = Simulator::kDefaultGravity;
    float total_mass_ui_ = Simulator::kDefaultTotalMass;
    // 力系数 F 用**指数坐标**滑条：存滑条位置 t∈[0,1]，F = min*(max/min)^t（对数均匀）。
    // 初值取自 kDefaultForceCoeff（默认 F=3 N ⇒ t≈0.75），保证 UI 与物理默认一致。
    float force_coeff_t_ui_ = ForceNewtonToT(Simulator::kDefaultForceCoeff);
    float damping_ui_ = Simulator::kVelocityDamping;
    // 全局速度上限（m/s；0 = 不限）。低质量数值失稳的兑底（danis 2026-10-01）。
    float max_speed_ui_ = kDefaultMaxSpeed;
    // 人体排斥（Step 2）：开关 + buffer（米，离开体表的最小距离）。
    bool body_repulsion_ui_ = true;
    float body_buffer_ui_ = Simulator::kDefaultBodyBuffer;
    // 人体排斥的切向速度保留系数（0~1；默认 1.0 = 全保留）。Danis 2026-10-02。
    float body_parallel_damping_ui_ = Simulator::kDefaultBodyParallelDamping;

    // F 的指数映射：t(0..1) ↔ F(N)。
    static float ForceTToNewton(float t) {
        const float lo = Simulator::kMinForceCoeff;
        const float hi = Simulator::kMaxForceCoeff;
        return lo * std::pow(hi / lo, t);
    }
    static float ForceNewtonToT(float f) {
        const float lo = Simulator::kMinForceCoeff;
        const float hi = Simulator::kMaxForceCoeff;
        return std::log(f / lo) / std::log(hi / lo);
    }

    // 装配真实字体文本测量回调（UI 内部用），Init() 后调用一次。
    void InstallTextMeasure() {
        ui_.SetTextMeasure(&ClothingToolApp::ViewerTextWidth, this);
    }

    // 渲染/出图时是否绘制面板（交互窗口 = true；headless 纯 3D 截图 = false）。
    void SetShowPanel(bool show) { show_panel_ = show; }
    bool show_panel() const { return show_panel_; }

    // ⭐ 主入口：推进 N 步仿真（headless 出图前推进用；交互窗口由 sim_running_ 驱动）。
    // 每步 = 1/60 s 动力学。返回是否真的推进了（无仿真器时返回 false）。
    bool AdvanceSimulationSteps(int steps) {
        if (sims_.empty() || steps <= 0) {
            return false;
        }
        for (int i = 0; i < steps; ++i) {
            StepSimulationOnce();
        }
        return true;
    }

    // 把衣服整体平移 delta（等价于面板平移一次）。供 headless 预摆位 / 脚本用。
    // 场景未就绪（sims_ 空）或已蒙皮（几何已冻结）时 no-op。
    void TranslateCloth(const jpov::Vec3f& delta) {
        if (skinned_ || sims_.empty()) {
            return;
        }
        for (Simulator& sim : sims_) {
            sim.ApplyTranslation(delta);
        }
        SyncSimsToCloth();
    }

    // 后台初始化泵（每帧，在主/GL 线程调用；供 OneIteration 与 headless main 循环使用）。
    //
    // 三阶段：
    //   1) kBuilding：后台线程建图，屏幕上显示进度文案（不阻帧）。
    //   2) kDone 第一帧：先把进度页切成"正在上传 GPU 资源..."（本帧只画不做事）。
    //   3) 下一帧：真正做 GPU 上传（LoadGltf 必须走 GL，仅主线程；会阻帧几秒）。
    //
    // 返回：本帧是否已就绪（可画 3D 场景，即 GPU 资产已上传）。
    bool TickInit() {
        init_.Tick();  // 推进后台建图状态机
        if (init_.state() != InitState::kDone) {
            return false;  // kBuilding（继续显进度）/ kFailed（由调用方显失败页）
        }
        if (gpu_uploaded_) {
            return true;
        }
        if (!upload_screen_drawn_) {
            upload_screen_drawn_ = true;
            return false;
        }
        UploadGpuAssets();
        return true;
    }

    // 上传两份 GPU 资产（仅主线程/GL 上下文）+ 相机自适应 + 取回衣服 CPU 几何 + 建仿真器。
    // Pre-condition: init_.state() == kDone。
    void UploadGpuAssets() {
        CHECK_EQ(static_cast<int>(init_.state()), static_cast<int>(InitState::kDone));
        // GPU 上传（主线程）。失败会 LOG(FATAL)（与旧版一致，资产缺失是硬错误）。
        body_ = LoadGltf(body_path_);
        CHECK(!body_.empty()) << "人体 reference 加载失败或为空: " << body_path_;
        cloth_ = LoadGltf(cloth_path_);
        CHECK(!cloth_.empty()) << "衣服模型加载失败或为空: " << cloth_path_;

        // 取回后台顺带保留的衣服 CPU 几何（base 几何 + 材质），供仿真 / 保存用。
        // 顺序与 LoadGltf 内部的 LoadGltfScene 一致（同一次遍历的同一顺序），故可下标对齐。
        std::vector<jpov::GltfMeshEntry> startup_geometry = init_.TakeClothGeometry();
        CHECK_EQ(startup_geometry.size(), cloth_.size())
            << "衣服 CPU 几何与 GPU primitive 数量不一致："
            << startup_geometry.size() << " vs " << cloth_.size();

        // 当前几何快照：初值 = 启动几何（未做任何变换）。
        cloth_current_.resize(startup_geometry.size());
        for (size_t i = 0; i < startup_geometry.size(); ++i) {
            cloth_current_[i].mesh = startup_geometry[i].mesh;
            cloth_current_[i].material = startup_geometry[i].material;
        }

        // 建仿真器（绑定姿态 = 启动几何）。
        InitSimulators();

        // 人体排斥（Step 2）：把后台建好的「人体最近三角形」匹配器借给各仿真器。
        // 匹配器在 init_ 里（比 sims_ 活得久：init_ 声明在 sims_ 之前 ⇒ 后析构）。
        if (init_.body_matcher().valid()) {
            for (Simulator& sim : sims_) {
                sim.SetBodyMatcher(&init_.body_matcher().matcher.value());
            }
        } else {
            LOG(WARNING) << "人体匹配器不可用，人体排斥将无效果";
        }

        gpu_uploaded_ = true;
        // 人体骨架（蒙皮产出的权重按它索引；保存 glb 时作为 skin）。无 skin 时自动蒙皮不可用。
        std::vector<jpov::SkeletonType> body_skins;
        if (jpov::LoadGltfSkeleton(body_path_, &body_skins) && !body_skins.empty()) {
            body_skeleton_ = body_skins[0];
            body_skeleton_.Validate();
            has_body_skeleton_ = true;
        } else {
            LOG(WARNING) << "人体 reference 无骨架（skin），软布自动蒙皮不可用: "
                         << body_path_;
        }
        LOG(INFO) << "人体 reference: " << body_path_ << "（" << body_.size()
                  << " primitives）";
        LOG(INFO) << "衣服模型: " << cloth_path_ << "（" << cloth_.size()
                  << " primitives）";
        FitViewToScene();
    }

    // ⭐ 唯一的渲染体：交互循环与 headless 出图共用（zero 分叉）。
    void OneIteration(int64_t frame_count, const jpov::InputSnapshot& input,
                      const jpov::WindowInfo& winfo,
                      jpov::RenderCommandList* cmds) override {
        (void)frame_count;
        // 3D FBO 跟随窗口尺寸（见文件头坐标空间说明）：scene 按窗口分辨率渲染，
        // resize 时不拉伸。注意这与「2D 面板用的 winfo」是**同一个本帧窗口尺寸**，
        // 而不是某个固定常量。
        CHECK_GT(winfo.width, 0.0f);
        CHECK_GT(winfo.height, 0.0f);
        cmds->camera.fbo_3d_width_  = static_cast<int>(winfo.width);
        cmds->camera.fbo_3d_height_ = static_cast<int>(winfo.height);

        // 后台初始化泵：完成分两帧（先显"上传中"再真传，见 TickInit）。
        const bool scene_ready = TickInit();

        if (init_.state() == InitState::kFailed) {
            DrawInitFailedScreen(winfo, cmds);
            return;
        }
        if (!scene_ready) {
            DrawInitProgressScreen(winfo, cmds);
            return;
        }

        // 保存状态机泵（每帧）。
        save_ctrl_.Tick();

        // 交互输入 → 视角（仅可见窗口消费输入；headless 的相机由外部赋 view_）。
        if (show_panel_) {
            float dx = 0.0f;
            float dy = 0.0f;
            float scroll = 0.0f;
            if (input.right.IsDrag()) {
                dx = input.mouse_dx;
                dy = input.mouse_dy;
            }
            if (input.scroll_delta != 0.0f) {
                scroll = input.scroll_delta;
            }
            ApplyInput(&view_, dx, dy, scroll,
                       static_cast<int>(winfo.width),
                       static_cast<int>(winfo.height));
        }

        // ── 滑块 / 参数同步到仿真器（每个 primitive 一份）。──
        SyncSimParams();

        // ── 仿真推进：勾选运行时逐帧 Step（headless 由 AdvanceSimulationSteps 驱动）。──
        if (sim_running_) {
            StepSimulationOnce();
        }

        // ── 相机：由 view_ 推导 ──
        cmds->camera.position = view_.Position();
        cmds->camera.target   = ViewConfig::Target();  // (0,0,0)
        cmds->camera.up       = {0.0f, 1.0f, 0.0f};
        cmds->camera.fov      = 60.0f;
        cmds->camera.near     = 0.05f;
        cmds->camera.far      = 1000.0f;

        // ── 光照：skylight viewer 同款天光（太阳仰角 45° + 三色环境光）──
        const jpov_skylight::SkyDegrees sky_deg;  // 默认：仰角 45°、方位 45°
        const jpov_skylight::SkyLighting light =
            jpov_skylight::MakeSkyLighting(sky_deg, /*tricolor_ambient*/ true);
        cmds->sky     = light.sky;
        cmds->sun     = light.dir_light;
        cmds->ambient = light.ambient;
        cmds->tone_mapping = true;

        // ── 场景：地面 + 人体 reference + 衣服。──
        if (ground_y_ != ground_y_last_built_) {
            UpdateMesh(ground_mesh_, MakeGroundQuad(ground_y_));
            ground_y_last_built_ = ground_y_;
        }
        cmds->DrawObject3D(ground_mesh_, ground_mat_,
                           /*center*/ {0.0f, 0.0f, 0.0f},
                           /*up*/     {0.0f, 1.0f, 0.0f},
                           /*front*/  {0.0f, 0.0f, 1.0f});

        // 人体 reference 固定画在原点（不参与衣服调节）。
        if (show_body_) {
            cmds->DrawGltfObject(body_,
                                 /*center*/ {0.0f, 0.0f, 0.0f},
                                 /*up*/     {0.0f, 1.0f, 0.0f},
                                 /*front*/  {0.0f, 0.0f, 1.0f});
        }
        // 衣服：几何是唯一的（顶点已含全部变换 / 仿真形变），故用**恒等放置**。
        if (show_cloth_) {
            cmds->DrawGltfObject(cloth_,
                                 /*center*/ {0.0f, 0.0f, 0.0f},
                                 /*up*/     {0.0f, 1.0f, 0.0f},
                                 /*front*/  {0.0f, 0.0f, 1.0f});
        }

        // ── 面板（仅交互窗口；headless 是纯 3D 截图）──
        if (show_panel_) {
            DrawPanels(input, winfo, cmds);
            ui_.End();
            ui_.Emit(cmds);
        }
    }

private:
    // ---- 文本测量回调：转发到 JPOV::MeasureTextWidth（真实字体进宽）。----
    static float ViewerTextWidth(const char* text, float font_size,
                                 const char* /*font_alias*/, void* userdata) {
        ClothingToolApp* app = static_cast<ClothingToolApp*>(userdata);
        return app->MeasureTextWidth(/*alias=*/std::string(),
                                     /*text=*/text ? text : "", font_size);
    }

    // ==================== 仿真器接线 ====================

    // 按当前几何（cloth_current_）建每个 primitive 的仿真器（绑定姿态）。
    // Pre-condition: cloth_current_ 非空且与 cloth_ 的 primitive 数一致。
    void InitSimulators() {
        CHECK_EQ(cloth_current_.size(), cloth_.primitives.size());
        sims_.clear();
        sims_.resize(cloth_current_.size());
        for (size_t i = 0; i < sims_.size(); ++i) {
            sims_[i].Init(cloth_current_[i].mesh);  // 默认关联距离 d = kDefaultBindDistance
            PushSimParams(&sims_[i]);
        }
        LOG(INFO) << "已建软体仿真器 × " << sims_.size() << "（绑定姿态 = 启动衣服几何）";
    }

    // 把本 primitive 的仿真器参数写成 UI 镜像值（建/重建后调用，保证一致）。
    void PushSimParams(Simulator* sim) {
        CHECK(sim != nullptr);
        sim->SetGravity(gravity_ui_);
        sim->SetTotalMass(total_mass_ui_);
        sim->SetForceCoeff(ForceTToNewton(force_coeff_t_ui_));
        sim->SetVelocityDamping(damping_ui_);
        sim->SetGroundY(ground_y_);
        sim->SetMaxSpeed(max_speed_ui_);
        sim->SetBodyRepulsionEnabled(body_repulsion_ui_);
        sim->SetBodyBuffer(body_buffer_ui_);
        sim->SetBodyParallelDamping(body_parallel_damping_ui_);
    }

    // 每帧把滑条镜像值同步到所有仿真器（值没变则跳过，避免无谓 setter）。
    void SyncSimParams() {
        for (Simulator& sim : sims_) {
            if (sim.gravity() != gravity_ui_) {
                sim.SetGravity(gravity_ui_);
            }
            if (sim.total_mass() != total_mass_ui_) {
                sim.SetTotalMass(total_mass_ui_);
            }
            const float f = ForceTToNewton(force_coeff_t_ui_);
            if (sim.force_coeff() != f) {
                sim.SetForceCoeff(f);
            }
            if (sim.velocity_damping() != damping_ui_) {
                sim.SetVelocityDamping(damping_ui_);
            }
            if (sim.ground_y() != ground_y_) {
                sim.SetGroundY(ground_y_);
            }
            if (sim.max_speed() != max_speed_ui_) {
                sim.SetMaxSpeed(max_speed_ui_);
            }
            if (sim.body_repulsion_enabled() != body_repulsion_ui_) {
                sim.SetBodyRepulsionEnabled(body_repulsion_ui_);
            }
            if (sim.body_buffer() != body_buffer_ui_) {
                sim.SetBodyBuffer(body_buffer_ui_);
            }
            if (sim.body_parallel_damping() != body_parallel_damping_ui_) {
                sim.SetBodyParallelDamping(body_parallel_damping_ui_);
            }
        }
    }

    // 推进一个外部步（1/60 s）：每个 primitive 各自 Step，然后同步到显示 / 快照。
    // 已蒙皮后冻结（冻几何）：直接 no-op，避免破坏权重↔顶点对应。
    void StepSimulationOnce() {
        if (skinned_ || sims_.empty()) {
            return;
        }
        for (Simulator& sim : sims_) {
            sim.Step(Simulator::kDefaultDt);
        }
        SyncSimsToCloth();
    }

    // ==================== 衣服变换（即时作用于仿真状态） ====================

    // 同步仿真器状态 → 显示：把每个 primitive 的当前 mesh 取回、重算法线、推上 GPU、
    // 刷新保存用快照。
    void SyncSimsToCloth() {
        for (size_t i = 0; i < sims_.size(); ++i) {
            jpov::MeshData m = sims_[i].mesh();
            // 顶点被物理改过 / 被即时变换过 → 法线须重算，否则着色停留在旧姿态。
            RecomputeVertexNormals(&m);
            UpdateMesh(cloth_.primitives[i].mesh_id, m);
            cloth_current_[i].mesh = std::move(m);
        }
    }

    // 旋转 / 缩放共用的枢轴 = 所有仿真点（全部 primitive）的合并包围盒中心。
    // 空仿真器返回原点。
    jpov::Vec3f SimsBoundsCenter() const {
        jpov::Vec3f lo(0.0f, 0.0f, 0.0f);
        jpov::Vec3f hi(0.0f, 0.0f, 0.0f);
        bool any = false;
        for (const Simulator& sim : sims_) {
            for (const jpov::Vec3f& p : sim.sim_positions()) {
                if (!any) {
                    lo = p;
                    hi = p;
                    any = true;
                } else {
                    lo = jpov::Vec3f(std::min(lo.x(), p.x()), std::min(lo.y(), p.y()),
                                     std::min(lo.z(), p.z()));
                    hi = jpov::Vec3f(std::max(hi.x(), p.x()), std::max(hi.y(), p.y()),
                                     std::max(hi.z(), p.z()));
                }
            }
        }
        if (!any) {
            return jpov::Vec3f(0.0f, 0.0f, 0.0f);
        }
        return jpov::Vec3f((lo.x() + hi.x()) * 0.5f, (lo.y() + hi.y()) * 0.5f,
                           (lo.z() + hi.z()) * 0.5f);
    }

    // app 的轴号（0/1/2 = X/Y/Z）→ 仿真器 Axis。
    static Axis ToSimAxis(int axis) {
        switch (axis) {
            case 0:
                return Axis::kX;
            case 1:
                return Axis::kY;
            case 2:
                return Axis::kZ;
            default:
                LOG(FATAL) << "ToSimAxis: axis 必须 ∈ {0,1,2}，got " << axis;
        }
        return Axis::kX;  // 不可达（LOG(FATAL) 已终止）；为满足返回类型。
    }

    // 平移步进：所有仿真器状态沿 axis 轴平移 direction * trans_step_[axis]（速度不变）。
    // Pre-condition: 0 <= axis < 3。已蒙皮后冻结（no-op）。
    void StepTranslation(int axis, float direction) {
        CHECK_GE(axis, 0);
        CHECK_LT(axis, 3);
        if (skinned_) {
            return;
        }
        const float d = direction * trans_step_[axis];
        const jpov::Vec3f delta(axis == 0 ? d : 0.0f, axis == 1 ? d : 0.0f,
                                axis == 2 ? d : 0.0f);
        for (Simulator& sim : sims_) {
            sim.ApplyTranslation(delta);
        }
        SyncSimsToCloth();
    }

    // 旋转步进：绕合并中心、绕 axis 轴逆时针转 direction * rot_step_[axis] 度
    // （位置与**速度**一起转）。Pre-condition: 0 <= axis < 3。已蒙皮后冻结（no-op）。
    void StepRotation(int axis, float direction) {
        CHECK_GE(axis, 0);
        CHECK_LT(axis, 3);
        if (skinned_) {
            return;
        }
        const float deg = direction * rot_step_[axis];
        const jpov::Vec3f pivot = SimsBoundsCenter();
        for (Simulator& sim : sims_) {
            sim.ApplyRotation(ToSimAxis(axis), deg, pivot);
        }
        SyncSimsToCloth();
    }

    // 缩放步进：整体缩放乘 factor（> 1 放大、< 1 缩小），绕合并中心；累计系数夹到
    // [kClothScaleMin, kClothScaleMax]（超界则本次不生效）。**速度**不参与缩放。
    // Pre-condition: factor > 0。已蒙皮后冻结（no-op）。
    void StepScale(float factor) {
        CHECK_GT(factor, 0.0f);
        if (skinned_) {
            return;
        }
        const float target = ClampClothScale(cloth_scale_ * factor);
        const float applied = target / cloth_scale_;  // 实际生效的比例（可能被 clamp 到 1）
        if (applied == 1.0f) {
            return;  // 已到缩放上下界，本次不动
        }
        const jpov::Vec3f pivot = SimsBoundsCenter();
        for (Simulator& sim : sims_) {
            sim.ApplyScaling(applied, pivot);
        }
        cloth_scale_ = target;
        SyncSimsToCloth();
    }

    // 「重置衣服」：所有仿真器 Reset（回**启动时**的绑定姿态），停仿真回到可重调状态。
    // （Danis：重置按钮把 mesh 重置回 clothing tool 启动时的样子。）
    void ResetClothMesh() {
        if (skinned_) {
            LOG(WARNING) << "已蒙皮，重置被忽略（几何已冻结）";
            return;
        }
        for (Simulator& sim : sims_) {
            sim.Reset();
        }
        cloth_scale_ = 1.0f;
        sim_running_ = false;   // 停机，回到可重调状态
        SyncSimsToCloth();
        LOG(INFO) << "重置衣服：仿真器已回启动姿态（缩放归 1、仿真暂停）";
    }

    // ==================== 面板 ====================

    // 提交一个数值输入框：解析文本 →（可选）clamp → 写回目标；非法输入不改目标。
    // 无论成功与否，都把文本框规范化为**最终生效值**（用户可见 clamp 后的结果）。
    // 返回 true = 目标值发生了变化。
    // Pre-condition: field != nullptr；target != nullptr。
    template <typename ClampFn>
    static bool CommitNumberField(NumberField* field, ClampFn clamp_fn,
                                  float* target /*inout*/) {
        CHECK(field != nullptr);
        CHECK(target != nullptr);
        const float before = *target;
        float parsed = 0.0f;
        if (ParseAxisValue(field->text, &parsed)) {
            *target = clamp_fn(parsed);
        }
        snprintf(field->text, kAxisInputCapacity, "%g",
                 static_cast<double>(*target));
        return *target != before;
    }

    // 画两个面板（左上 = 变换 / 保存 / 地面 / 显示；右上 = 仿真动力学）。
    // 所有布局都从**本帧窗口尺寸** winfo 推算（见文件头坐标空间说明）。
    void DrawPanels(const jpov::InputSnapshot& input,
                    const jpov::WindowInfo& winfo,
                    jpov::RenderCommandList* cmds) {
        const float w = winfo.width;
        const float h = winfo.height;

        jpov::UiTheme theme = jpov::UiTheme::Default(kFontSize);
        theme.font_alias = kViewerFontAlias;
        ui_.Begin(input, theme, w, h, 1000.0f / kViewerFps);

        DrawLeftPanel(cmds, w);
        DrawRightPanel(cmds, w, h);
        DrawSkinPanel(cmds, w, h);
    }

    // ---- 左上角：变换 / 保存 / 地面 / 显示 ----
    //
    // 布局（自上而下，行游标）：
    //   平移表头(轴/步长/步进) + X/Y/Z 三行（步长框 + "<" ">"）
    //   旋转表头 + RX/RY/RZ 三行
    //   缩放行（系数框 + "-" "+" + 只读当前）
    //   保存按钮 + 保存状态
    //   地面高度滑条
    //   显示勾选（人体 / 衣服）
    //   两行只读（人体 / 衣服来源）
    void DrawLeftPanel(jpov::RenderCommandList* cmds, float win_w) {
        const float kMargin  = 12.0f;
        const float kPad     = 10.0f;
        const float kRowH    = kPanelRowH;
        const float kSpacing = 5.0f;
        const float panel_w  = 0.32f * win_w;
        const float panel_x  = kMargin;
        const float panel_y  = kMargin;
        // 行数：平移表头1 + 平移3 + 旋转表头1 + 旋转3 + 缩放1 + 保存1 + 保存状态1
        //        + 地面1 + 勾选1 + 只读2 = 15。
        constexpr int kRows = 15;
        const float panel_h = kPad * 2.0f + kRows * kRowH + (kRows - 1) * kSpacing;
        const jpov::Color kPanelBg{0.0f, 0.0f, 0.0f, 0.5f};
        cmds->DrawRect(/*pos*/ {panel_x, panel_y}, /*size*/ {panel_w, panel_h},
                       kPanelBg);

        const float left = panel_x + kPad;
        const float top  = panel_y + kPad;
        const float row_w = panel_w - kPad * 2.0f;
        const float step_y = kRowH + kSpacing;

        const float axis_w = 30.0f;     // "轴"列（X/Y/Z）
        const float stepbox_w = 72.0f;  // 步长输入框宽
        const float btn_w = 26.0f;      // "<" / ">" / "−" / "+" 按钮宽
        const float gap = kSpacing;
        const float col_axis   = left;
        const float col_stepbx = col_axis + axis_w + gap;
        const float col_btn_in = col_stepbx + stepbox_w + gap;   // "<"
        const float col_btn_in2 = col_btn_in + btn_w + gap;      // ">"

        float row_y = top;

        // ---- 平移表头 ----
        DrawLabel("轴", col_axis, axis_w, row_y);
        DrawLabel("步长(米)", col_stepbx, stepbox_w, row_y);
        DrawLabel("步进", col_btn_in, col_btn_in2 + btn_w - col_btn_in, row_y);
        row_y += step_y;

        // ---- 平移 X / Y / Z 行（只有步长框 + 步进按钮；无绝对位置）----
        static const char* const kAxisTags[3] = {"X", "Y", "Z"};
        for (int axis = 0; axis < 3; ++axis) {
            DrawLabel(kAxisTags[axis], col_axis, axis_w, row_y);
            const bool step_focus = ui_.InputText(
                "", trans_step_field_[axis].text, kAxisInputCapacity,
                jpov::UiRect{{col_stepbx, row_y}, {stepbox_w, kRowH}});
            if (trans_step_field_[axis].focused_prev && !step_focus) {
                CommitNumberField(&trans_step_field_[axis], ClampTransStep,
                                  &trans_step_[axis]);
            }
            trans_step_field_[axis].focused_prev = step_focus;

            if (ui_.Button("<", jpov::UiRect{{col_btn_in, row_y}, {btn_w, kRowH}})) {
                StepTranslation(axis, -1.0f);
            }
            if (ui_.Button(">", jpov::UiRect{{col_btn_in2, row_y}, {btn_w, kRowH}})) {
                StepTranslation(axis, +1.0f);
            }
            row_y += step_y;
        }

        // ---- 旋转表头 ----
        DrawLabel("轴", col_axis, axis_w, row_y);
        DrawLabel("步长(°)", col_stepbx, stepbox_w, row_y);
        DrawLabel("步进", col_btn_in, col_btn_in2 + btn_w - col_btn_in, row_y);
        row_y += step_y;

        // ---- 旋转 RX / RY / RZ 行（只有步长框 + 步进按钮）----
        static const char* const kRotTags[3] = {"RX", "RY", "RZ"};
        for (int axis = 0; axis < 3; ++axis) {
            DrawLabel(kRotTags[axis], col_axis, axis_w, row_y);
            const bool rot_focus = ui_.InputText(
                "", rot_step_field_[axis].text, kAxisInputCapacity,
                jpov::UiRect{{col_stepbx, row_y}, {stepbox_w, kRowH}});
            if (rot_step_field_[axis].focused_prev && !rot_focus) {
                CommitNumberField(&rot_step_field_[axis], ClampRotStep,
                                  &rot_step_[axis]);
            }
            rot_step_field_[axis].focused_prev = rot_focus;

            if (ui_.Button("<", jpov::UiRect{{col_btn_in, row_y}, {btn_w, kRowH}})) {
                StepRotation(axis, -1.0f);
            }
            if (ui_.Button(">", jpov::UiRect{{col_btn_in2, row_y}, {btn_w, kRowH}})) {
                StepRotation(axis, +1.0f);
            }
            row_y += step_y;
        }

        // ---- 缩放行：系数输入框 + "−" "+" + 当前累计缩放只读 ----
        const float scale_lbl_w = 72.0f;
        DrawLabel("缩放系数", col_axis, scale_lbl_w, row_y);
        const float scale_bx = col_axis + scale_lbl_w + gap;
        const bool scale_focus = ui_.InputText(
            "", scale_step_field_.text, kAxisInputCapacity,
            jpov::UiRect{{scale_bx, row_y}, {stepbox_w, kRowH}});
        if (scale_step_field_.focused_prev && !scale_focus) {
            CommitNumberField(&scale_step_field_, ClampScaleStep, &scale_step_);
        }
        scale_step_field_.focused_prev = scale_focus;
        const float scale_minus_x = scale_bx + stepbox_w + gap;
        const float scale_plus_x = scale_minus_x + btn_w + gap;
        if (ui_.Button("-", jpov::UiRect{{scale_minus_x, row_y}, {btn_w, kRowH}})) {
            StepScale(1.0f / scale_step_);
        }
        if (ui_.Button("+", jpov::UiRect{{scale_plus_x, row_y}, {btn_w, kRowH}})) {
            StepScale(scale_step_);
        }
        const float scale_info_x = scale_plus_x + btn_w + gap;
        DrawLabel(Format("x%.3f", static_cast<double>(cloth_scale_)).c_str(),
                  scale_info_x, left + row_w - scale_info_x, row_y);
        row_y += step_y;

        // ---- 保存按钮行 ----
        const float save_btn_w = 140.0f;
        const char* save_label =
            (save_ctrl_.state() == ClothSaveState::kSaving) ? "保存中..."
                                                            : "保存衣服 glb";
        if (ui_.Button(save_label,
                       jpov::UiRect{{left, row_y}, {save_btn_w, kRowH}})) {
            StartSaveCloth();
        }
        row_y += step_y;

        // ---- 保存状态行：单独一行 + 真左对齐（长文案避免压到上一行按钮）。----
        const std::string& save_msg = save_ctrl_.message();
        if (!save_msg.empty()) {
            const jpov::Color kForeground{0.92f, 0.93f, 0.95f, 1.0f};
            cmds->DrawText(save_msg,
                           /*pos*/ {left, row_y + (kRowH - kFontSize) * 0.5f},
                           kFontSize, kForeground,
                           jpov::TextAlignment::kTopLeft, kViewerFontAlias);
        }
        row_y += step_y;

        // ---- 地面高度（米）：[-3,+3] ----
        ui_.SliderFloat("地面高度 y", &ground_y_,
                        jpov::UiRect{{left, row_y}, {row_w, kRowH}},
                        -3.0f, 3.0f, /*decimal_places*/2);
        row_y += step_y;

        // ---- 显示开关（人体参考 / 衣服，各自独立）----
        const float kCheckW = row_w / 2.0f;
        ui_.Checkbox("显示人体 reference", &show_body_,
                     jpov::UiRect{{left, row_y}, {kCheckW, kRowH}});
        ui_.Checkbox("显示衣服", &show_cloth_,
                     jpov::UiRect{{left + kCheckW, row_y}, {kCheckW, kRowH}});
        row_y += step_y;

        // ---- 人体 reference 来源 + primitive 数（只读）----
        const std::string body_line = Format(
            "人体：%s（%zu primitives）", BaseNameOf(body_path_).c_str(),
            body_.size());
        ui_.Text(body_line.c_str(), jpov::UiRect{{left, row_y}, {row_w, kRowH}});
        row_y += step_y;

        // ---- 衣服来源 + primitive 数（只读）----
        const std::string cloth_line = Format(
            "衣服：%s（%zu primitives）", BaseNameOf(cloth_path_).c_str(),
            cloth_.size());
        ui_.Text(cloth_line.c_str(), jpov::UiRect{{left, row_y}, {row_w, kRowH}});
        row_y += step_y;
    }

    // ---- 右上角：仿真动力学 ----
    //
    // 注意：面板**贴右边缘**，x 从**本帧窗口宽度** win_w 反推（不是固定常量），
    // 这样窗口 resize 时右上角面板始终贴住右上角。
    //
    // 行：标题(1) + 重力/质量/力系数/衰减/速度上限 滑条(5) + 人体排斥开关(1)
    //     + buffer(1) + 切向保留系数(1) + 按钮行(1) + 状态行(2) = 12。
    void DrawRightPanel(jpov::RenderCommandList* cmds, float win_w, float win_h) {
        (void)win_h;
        const float kMargin  = 12.0f;
        const float kPad     = 10.0f;
        const float kRowH    = kPanelRowH;
        const float kSpacing = 5.0f;
        const float panel_w  = 0.30f * win_w;
        const float panel_x  = win_w - panel_w - kMargin;  // 贴右边缘
        const float panel_y  = kMargin;
        constexpr int kRows = 12;
        const float panel_h = kPad * 2.0f + kRows * kRowH + (kRows - 1) * kSpacing;
        const jpov::Color kPanelBg{0.0f, 0.0f, 0.0f, 0.5f};
        cmds->DrawRect(/*pos*/ {panel_x, panel_y}, /*size*/ {panel_w, panel_h},
                       kPanelBg);

        const float left = panel_x + kPad;
        const float top  = panel_y + kPad;
        const float row_w = panel_w - kPad * 2.0f;
        const float step_y = kRowH + kSpacing;
        float row_y = top;

        // ---- 标题 ----
        DrawLabel("仿真动力学", left, row_w, row_y);
        row_y += step_y;

        // ---- 重力 g ----
        ui_.SliderFloat("重力 g (m/s²)", &gravity_ui_,
                        jpov::UiRect{{left, row_y}, {row_w, kRowH}},
                        Simulator::kMinGravity, Simulator::kMaxGravity,
                        /*decimal_places*/1);
        row_y += step_y;

        // ---- 总质量 M ----
        ui_.SliderFloat("总质量 M (kg)", &total_mass_ui_,
                        jpov::UiRect{{left, row_y}, {row_w, kRowH}},
                        Simulator::kMinTotalMass, Simulator::kMaxTotalMass,
                        /*decimal_places*/1);
        row_y += step_y;

        // ---- 力系数 F（指数坐标）：滑条位置 t∈[0,1] 线性，F = min*(max/min)^t。----
        ui_.SliderFloat("力系数 F (指数)", &force_coeff_t_ui_,
                        jpov::UiRect{{left, row_y}, {row_w, kRowH}},
                        0.0f, 1.0f, /*decimal_places*/2);
        row_y += step_y;

        // ---- 衰减 k ----
        ui_.SliderFloat("衰减 k (1/s)", &damping_ui_,
                        jpov::UiRect{{left, row_y}, {row_w, kRowH}},
                        Simulator::kMinDamping, Simulator::kMaxDamping,
                        /*decimal_places*/2);
        row_y += step_y;

        // ---- 全局速度上限（m/s；0 = 不限）----
        ui_.SliderFloat("速度上限 (m/s, 0=不限)", &max_speed_ui_,
                        jpov::UiRect{{left, row_y}, {row_w, kRowH}},
                        0.0f, 100.0f, /*decimal_places*/0);
        row_y += step_y;

        // ---- 人体排斥：开关 + buffer（离开体表的最小距离）+ 切向速度保留系数 ----
        ui_.Checkbox("人体排斥", &body_repulsion_ui_,
                     jpov::UiRect{{left, row_y}, {row_w, kRowH}});
        row_y += step_y;
        ui_.SliderFloat("排斥 buffer (m)", &body_buffer_ui_,
                        jpov::UiRect{{left, row_y}, {row_w, kRowH}},
                        Simulator::kMinBodyBuffer, Simulator::kMaxBodyBuffer,
                        /*decimal_places*/ 3);
        row_y += step_y;
        // 切向（平行于表面）速度保留系数：0 = 全消（粘住）/ 1 = 全保留（默认）。
        ui_.SliderFloat("切向速度保留系数", &body_parallel_damping_ui_,
                        jpov::UiRect{{left, row_y}, {row_w, kRowH}},
                        Simulator::kMinBodyParallelDamping,
                        Simulator::kMaxBodyParallelDamping,
                        /*decimal_places*/ 2);
        row_y += step_y;

        // ---- 按钮行：[暂停/继续] [重置衣服] ----
        const float kBtnW = (row_w - kSpacing) * 0.5f;
        const char* pause_label = sim_running_ ? "暂停仿真" : "继续仿真";
        if (ui_.Button(pause_label, jpov::UiRect{{left, row_y}, {kBtnW, kRowH}})) {
            sim_running_ = !sim_running_;
            LOG(INFO) << (sim_running_ ? "仿真：继续" : "仿真：暂停");
        }
        if (ui_.Button("重置衣服",
                       jpov::UiRect{{left + kBtnW + kSpacing, row_y},
                                    {kBtnW, kRowH}})) {
            ResetClothMesh();
        }
        row_y += step_y;

        // ---- 状态行 1：F 实际牛顿数 + 仿真时间 / 步数 ----
        const std::string line1 = Format(
            "F = %.4g N   仿真 t=%.2fs 步=%zu",
            static_cast<double>(ForceTToNewton(force_coeff_t_ui_)),
            sims_.empty() ? 0.0 : sims_.front().time(),
            sims_.empty() ? static_cast<size_t>(0) : sims_.front().step_count());
        ui_.Text(line1.c_str(), jpov::UiRect{{left, row_y}, {row_w, kRowH}});
        row_y += step_y;

        // ---- 状态行 2：仿真点计数（原始 + 虚拟 = 合计）----
        size_t orig = 0;
        size_t virt = 0;
        for (const Simulator& s : sims_) {
            orig += s.original_point_count();
            virt += s.virtual_point_count();
        }
        const std::string line2 = Format(
            "仿真点 原始 %zu + 虚拟 %zu = %zu", orig, virt, orig + virt);
        ui_.Text(line2.c_str(), jpov::UiRect{{left, row_y}, {row_w, kRowH}});
        row_y += step_y;
    }

    // ---- 右下角：软布自动蒙皮（一键）----
    //
    // 贴右下角（x 由窗口宽反推、y 由窗口高反推），与左上（变换/保存）、右上（动力学）
    // 互不遮挡。行：标题(1) + 一键蒙皮按钮(1) + 平滑权重勾选(1) + 状态文本(1) = 4。
    void DrawSkinPanel(jpov::RenderCommandList* cmds, float win_w, float win_h) {
        const float kMargin  = 12.0f;
        const float kPad     = 10.0f;
        const float kRowH    = kPanelRowH;
        const float kSpacing = 5.0f;
        const float panel_w  = 0.30f * win_w;
        constexpr int kRows = 4;
        const float panel_h = kPad * 2.0f + kRows * kRowH + (kRows - 1) * kSpacing;
        const float panel_x = win_w - panel_w - kMargin;  // 贴右边缘
        const float panel_y = win_h - panel_h - kMargin;  // 贴底边缘
        const jpov::Color kPanelBg{0.0f, 0.0f, 0.0f, 0.5f};
        cmds->DrawRect(/*pos*/ {panel_x, panel_y}, /*size*/ {panel_w, panel_h},
                       kPanelBg);

        const float left = panel_x + kPad;
        const float top  = panel_y + kPad;
        const float row_w = panel_w - kPad * 2.0f;
        const float step_y = kRowH + kSpacing;
        float row_y = top;

        DrawLabel("软布自动蒙皮", left, row_w, row_y);
        row_y += step_y;

        // 一键蒙皮按钮：已蒙皮后 RunAutoSkin 自身 no-op（按钮仍可点，文案变明示状态）。
        const char* btn = skinned_ ? "已蒙皮（已冻结）" : "一键蒙皮";
        if (ui_.Button(btn, jpov::UiRect{{left, row_y}, {row_w, kRowH}})) {
            RunAutoSkin();
        }
        row_y += step_y;

        ui_.Checkbox("平滑权重", &skin_smooth_ui_,
                     jpov::UiRect{{left, row_y}, {row_w, kRowH}});
        row_y += step_y;

        // 状态 / 提示（可能为空）。
        if (!skin_msg_.empty()) {
            const jpov::Color kForeground{0.92f, 0.93f, 0.95f, 1.0f};
            cmds->DrawText(skin_msg_,
                           /*pos*/ {left, row_y + (kRowH - kFontSize) * 0.5f},
                           kFontSize, kForeground,
                           jpov::TextAlignment::kTopLeft, kViewerFontAlias);
        }
    }

    // 画一个左对齐、垂直居中的标签（不拉伸：内容居中于给定宽度）。
    void DrawLabel(const char* text, float x, float width, float y) {
        ui_.Text(text, jpov::UiRect{{x, y}, {width, kPanelRowH}}, false, false);
    }

    // 发起保存：把当前衣服几何快照交给保存控制器（按值快照，之后改动不影响本次）。
    // 已自动蒙皮时带上骨架（写 glb 的 skin + inverseBindMatrices）。
    void StartSaveCloth() {
        if (cloth_current_.empty()) {
            LOG(WARNING) << "保存被忽略：衣服几何尚未就绪";
            return;
        }
        std::optional<jpov::SkeletonType> skin;
        if (skinned_ && has_body_skeleton_) {
            skin = body_skeleton_;
        }
        save_ctrl_.Start(cloth_current_, std::move(skin), cloth_path_, "cloth");
    }

    // 一键：软布自动蒙皮（weight transfer，见 weight_transfer.h）。成功后就地给每个
    // 衣物 primitive 写入 JOINTS_0/WEIGHTS_0（并在 flags 置 kJoints），随后**冻结几何**
    // （禁变换 / 禁仿真 / 禁重置），保证权重↔顶点对应不被破坏。
    //
    // 全程主线程同步：顶点数万量级、最近三角形查询近 O(1)，实测亚秒级。
    // Pre-condition: 场景就绪（gpu_uploaded_）。
    void RunAutoSkin() {
        if (skinned_) {
            skin_msg_ = "已蒙皮（几何已冻结，可保存）";
            return;
        }
        if (!has_body_skeleton_) {
            skin_msg_ = "人体 reference 无骨架（skin），无法自动蒙皮";
            LOG(WARNING) << skin_msg_;
            return;
        }
        if (!init_.body_matcher().valid() || init_.body_skin().triangle_count() == 0) {
            skin_msg_ = "人体无蒙皮信息（JOINTS_0/WEIGHTS_0），无法自动蒙皮";
            LOG(WARNING) << skin_msg_;
            return;
        }
        if (cloth_current_.empty()) {
            skin_msg_ = "衣服几何尚未就绪";
            return;
        }
        SyncSimsToCloth();  // 以当前（仿真后）几何为准

        const int passes = skin_smooth_ui_ ? kSkinSmoothPasses : 0;
        const geom::TriangleMatcher3d<double>& matcher =
            init_.body_matcher().matcher.value();
        size_t total_verts = 0;
        size_t total_gap = 0;
        float max_gap_m = 0.0f;
        for (size_t i = 0; i < cloth_current_.size(); ++i) {
            const SkinTransferStats s = TransferSkinWeights(
                init_.body_skin(), matcher, &cloth_current_[i].mesh,
                kSkinGapThresholdM, kSkinMaxInfluences, passes);
            total_verts += s.vertex_count;
            total_gap += s.gap_vertex_count;
            max_gap_m = std::max(max_gap_m, s.max_gap_m);
        }
        skinned_ = true;
        sim_running_ = false;
        skin_msg_ = Format("已蒙皮 %zu 顶点 / gap %zu（max %.1f mm）/ 平滑 %d 轮",
                           total_verts, total_gap,
                           static_cast<double>(max_gap_m * 1000.0f), passes);
        LOG(INFO) << "软布自动蒙皮完成：" << skin_msg_;
    }

    // ---- 资产包围盒的并集（相机自适应用）。----
    struct BoundsUnion {
        float min[3] = {0.0f, 0.0f, 0.0f};
        float max[3] = {0.0f, 0.0f, 0.0f};
        bool valid = false;
    };

    static void AccumulateBounds(const jpov::GltfObject& obj,
                                 BoundsUnion* out /*inout*/) {
        CHECK(out != nullptr);
        if (!obj.bounds_valid) {
            return;
        }
        for (int i = 0; i < 3; ++i) {
            if (!out->valid) {
                out->min[i] = obj.bounds_min[i];
                out->max[i] = obj.bounds_max[i];
            } else {
                out->min[i] = std::min(out->min[i], obj.bounds_min[i]);
                out->max[i] = std::max(out->max[i], obj.bounds_max[i]);
            }
        }
        out->valid = true;
    }

    // 按「人体 ∪ 衣服」两个资产的包围盒，把相机距离 R 自适当前夹。
    void FitViewToScene() {
        BoundsUnion bounds;
        AccumulateBounds(body_, &bounds);
        AccumulateBounds(cloth_, &bounds);
        if (!bounds.valid) {
            return;
        }
        // 把**地面高度**也纳入 y 范围：衣服会在重力下落到地面，若只按模型包围盒取景，
        // 落地过程可能跑出画面（无法一眼看到“砸地”效果）。
        bounds.min[1] = std::min(bounds.min[1], ground_y_);
        view_.R = ViewConfig::FitRadius(bounds.min, bounds.max, /*fov_deg*/ 60.0);
        LOG(INFO) << "场景包围盒 [" << bounds.min[0] << "," << bounds.min[1] << ","
                  << bounds.min[2] << "] ~ [" << bounds.max[0] << "," << bounds.max[1]
                  << "," << bounds.max[2] << "]，初始 R=" << view_.R;
        if (body_.bounds_valid) {
            LOG(INFO) << "  人体包围盒 [" << body_.bounds_min[0] << "," << body_.bounds_min[1]
                      << "," << body_.bounds_min[2] << "] ~ [" << body_.bounds_max[0]
                      << "," << body_.bounds_max[1] << "," << body_.bounds_max[2] << "]";
        }
        if (cloth_.bounds_valid) {
            LOG(INFO) << "  衣服包围盒 [" << cloth_.bounds_min[0] << "," << cloth_.bounds_min[1]
                      << "," << cloth_.bounds_min[2] << "] ~ [" << cloth_.bounds_max[0]
                      << "," << cloth_.bounds_max[1] << "," << cloth_.bounds_max[2] << "]";
        }
    }

    // 后台初始化进度页：整屏黑底（不透明）+ 居中白字。尺寸用**本帧窗口尺寸**。
    void DrawInitProgressScreen(const jpov::WindowInfo& winfo,
                                jpov::RenderCommandList* cmds) {
        const jpov::Color kBlack{0.0f, 0.0f, 0.0f, 1.0f};
        const jpov::Color kWhite{1.0f, 1.0f, 1.0f, 1.0f};
        cmds->DrawRect(/*pos*/ {0.0f, 0.0f}, /*size*/ {winfo.width, winfo.height},
                       kBlack);
        std::string msg = init_.progress_message();
        if (init_.state() == InitState::kDone && !gpu_uploaded_) {
            msg = "正在上传 GPU 资源...";
        }
        cmds->DrawText(msg.empty() ? "正在初始化..." : msg,
                       /*pos*/ {winfo.width * 0.5f, winfo.height * 0.5f},
                       /*font_size*/ 24.0f, kWhite,
                       jpov::TextAlignment::kCenter, kViewerFontAlias);
    }

    // 后台初始化失败页：整屏黑底 + 居中白字（出错原因）。尺寸用**本帧窗口尺寸**。
    void DrawInitFailedScreen(const jpov::WindowInfo& winfo,
                              jpov::RenderCommandList* cmds) {
        const jpov::Color kBlack{0.0f, 0.0f, 0.0f, 1.0f};
        const jpov::Color kWhite{1.0f, 1.0f, 1.0f, 1.0f};
        cmds->DrawRect(/*pos*/ {0.0f, 0.0f}, /*size*/ {winfo.width, winfo.height},
                       kBlack);
        const std::string msg = init_.error_message();
        cmds->DrawText(msg.empty() ? "初始化失败" : ("初始化失败：" + msg),
                       /*pos*/ {winfo.width * 0.5f, winfo.height * 0.5f},
                       /*font_size*/ 20.0f, kWhite,
                       jpov::TextAlignment::kCenter, kViewerFontAlias);
    }

    // 取路径的文件名部分（去目录），用于面板只读行避免长路径溢出。
    static std::string BaseNameOf(const std::string& path) {
        const size_t slash = path.find_last_of("/\\");
        return (slash == std::string::npos) ? path : path.substr(slash + 1);
    }

    // 极简 snprintf 包装（面板只读文本用）。
    static std::string Format(const char* fmt, ...) {
        char buf[512];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        return std::string(buf);
    }

    bool show_panel_ = true;             // 是否绘制面板（headless 出图 = false）
    bool gpu_uploaded_ = false;          // 后台建图完成后是否已上传 GPU 资产（一次性）
    bool upload_screen_drawn_ = false;   // 是否已画过"上传中"页（见 TickInit 两帧安排）
    float ground_y_last_built_ = -3.0f;  // 上次建 quad 用的地面高度（变了才重建）

    jpov::Ui ui_;                        // 跨帧持有（滑条拖动态 / 聚焦态等内部记忆）

    static constexpr float kFontSize = 16.0f;
    static constexpr float kPanelRowH = 26.0f;  // 面板每行高度（DrawPanel / DrawLabel 共用）
};

}  // namespace clothing
}  // namespace jpov

#endif  // JPOV_CLOTHING_CLOTHING_TOOL_APP_H_
