// JPOV ARAP 查看器 — 渲染核心 App（可交互的软体动力学仿真宿主）
//
// 与 model viewer 并列的第二个交互式查看器：同样「加载 glTF + 可调视角/光照/地面」，
// 额外多出一套**动力学仿真**能力——把被加载的网格当作可变形软体（胡克弹簧 + ARAP
// 形状力 + 重力 + 阻尼），在重力场里从高处摔到地面上，肉眼观察它的形变。
//
// 交互面板（底部 5 滑条 + 顶部 2 按钮）：
//   ① 太阳仰角 °  [0,90]  ② 浊度 turb [2,8]  ③ 季节 R [0.5,2.0]
//   ④ 地面高度 y  [自适应]  ⑤ **重力 g [0,30]（默认 1.0）** —— 唯一暴露的物理量
//   [动力学：运行/暂停] —— 仿真启停（暂停时不推进物理，仅保持当前形变）
//   [重置 mesh]         —— 把网格恢复到 bind pose（清空速度与形变）
//
// ═══ 物理与单位（需求方 Danis 2026-09-23 定稿；MKS，长度 = 米）═══
//   glb 的 cm 资产在装载边界已换算成米（见 gltf_loader）。
//   **材质 = 三个可从材料手册查出的常数**（面板不可改，见 arap_sim.h 文件头推导）：
//     · ρ  面密度 [kg/m²]  = 体密度 × 厚度      ⇒ m_i = ρ·a_i（a_i = 顶点摊到的面积）
//     · c  胡克  [N/m³]    = 杨氏模量 E × 厚度   ⇒ k_e = c·√(a_i·a_j)
//     · c' ARAP  [N/m³]    = 剪切模量 G × 厚度   ⇒ β_i = c'·a_i
//   三者同阶 ∝ 1/N ⇒ k_e/m、β/m 与网格密度无关（换网格密度效果不漂）。
//   默认 = 厚度 1 cm 的橡胶皮：ρ=11、c=1.5e4、c'=5e3。
//   **面板可调**：重力 g（默认 1.0）+ 阻尼 u（默认 0.1，加速度域 a_damp = −u·v，与质量无关）。
//
// 物理步长：目标 **1/60 s**（需求方限定"子步最多接受 60 Hz"，故自动模式不加子步）。
//   渲染帧也是 1/60 s ⇒ 每帧推进一步。若材质太硬使 1/60 s 不稳，则自动**放大步长**
//   （慢动作，力模型不改），而不是靠内部细分堆算力。
//
// ═══ 数值保护（两类要分清）═══
//   ① **数值爆炸**（几何出现 NaN/inf）= 积分不稳定 ⇒ 回滚到本帧开始的状态 + 暂停 +
//      ERROR 日志（说明步长与稳定上限）。
//   ② **物理塌陷**（几何完好但被压扁）= 真实结果（如 1cm 橡胶皮撞地）⇒ 只 WARNING
//      一次，**不暂停不回滚** —— 把正确的物理当错误会让用户看不到真相。
//
// ═══ 每帧链路（变形 → 渲染，顺序不可换）═══
//   Step(dt) → WriteBackPositions → RecomputeTangentSpace → UpdateMesh → Draw
//   其中 RecomputeTangentSpace 是**必做**项而非优化：法线/切线是位置的派生量，
//   几何变形后必须用新位置重算，否则光照用「旧形状法线」照「新形状几何」。
//
// ═══ 多 primitive 的处理 ═══
//   **全部 primitive 合成【一个】物理物体**：跨 primitive 焊接（位置重合处焊成同一
//   质点）。理由（Danis 2026-09-22 实测）：资产常按材质切开（lantern.glb = 杆/链/
//   灯罩 3 个 primitive），若各自独立仿真，各部件互不支撑 ⇒ 看起来像模型散架。
//   渲染侧仍逐 primitive 绘制，材质各自独立。

#ifndef JPOV_DEMO_ARAP_VIEWER_APP_H_
#define JPOV_DEMO_ARAP_VIEWER_APP_H_

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/demo/arap_sim.h"
#include "tools/jpov/demo/view_config.h"
#include "tools/jpov/demo/viewer_app.h"   // 复用 kViewerWidth/Height/Fps/FontAlias
#include "tools/jpov/interface/mesh_geometry.h"
#include "tools/jpov/interface/ui.h"
#include "tools/jpov/src/gltf_loader.h"   // LoadGltfScene（CPU 侧几何，供仿真）

namespace jpov_arap_viewer {

using jpov_viewer::kViewerWidth;
using jpov_viewer::kViewerHeight;
using jpov_viewer::kViewerFps;
using jpov_viewer::kViewerFontAlias;

// 一个可仿真 primitive：CPU 实时几何 + 仿真器 + 动态 GPU 网格。
//
// mesh 是**唯一的几何事实源**：仿真写回它的 positions，随后重算其法线/切线，
// 再整份 UpdateMesh 上传。材质不在此持有（直接引用 GltfObject 里的那份，避免复制
// 出两个纹理句柄来源）。
//
// 生命周期：LoadModel 填充 → 每帧被 Step/WriteBack/Recompute/UpdateMesh → 由
// ReleaseModel 释放在 GL 侧的 mesh_id（材质纹理归 GltfObject 管理）。
struct SimPrimitive {
    jpov::MeshData mesh;         // CPU 实时几何（bind pose 起步，逐帧被形变覆盖）
    jpov::PBRMaterial material;  // 绘制材质（从 GltfObject 拷贝句柄；纹理本体归其所有）
    uint32_t mesh_id = 0;        // 动态 GPU 网格（RegisterMesh 得到，逐帧 UpdateMesh）
};

// ARAP 查看器渲染核心 App。
class ArapViewerApp : public JPOV {
public:
    using JPOV::JPOV;

    // 加载模型：GPU 侧（材质/纹理）+ CPU 侧（仿真几何）双路装载，并按 primitive
    // 建立各自的仿真器。
    //
    // Pre-condition:  Init() 已调用；path 非空且指向合法 .gltf/.glb。
    // Post-condition: 成功返回 true，primitives() 非空；失败返回 false（已 LOG ERROR）。
    bool LoadModel(const std::string& path);

    // 装载一个内置单位方块（无资产依赖）。
    //
    // 用途：验证动力学本身的「零模型」——方块从水平姿态落下时**不可能翻倒**，
    // 只能靠压缩来响应碰撞，因此弹性/回弹行为在这里能干净地看到；而真实资产的形状
    // 往往可以选择「翻倒」这条零能量路径（ARAP 允许等距弯曲/旋转），于是看不到压缩。
    // 也可以作为 ARAP 局部形状力的验证面——强压方块，看形状力是否把它推回原样。
    //
    // Pre-condition: Init() 已调用；本函数会在推入方块前清空 prims_，
    //                 故只能在启动时调用一次（不支持运行时热切换模型）。
    bool LoadBuiltinBox();

    // 释放模型占用的 GPU 资源（GltfObject 的网格/纹理 + 本 App 注册的动态网格）。
    // 必须在 Finalize() 之前调用（需要存活的 GL context）。
    void ReleaseModel();

    // 交互面板是否绘制（headless 出图 = false，截图即纯 3D 场景）。
    void SetShowPanel(bool show) { show_panel_ = show; }

    // 相机取景中心的 y 坐标（x=z=0）。
    //
    // 为什么需要它：ViewConfig 的球面角相机恒以原点为目标点，但本查看器的场景是
    // 「模型悬在上、地面在下」，取景中心放在原点会让落点（地面）被切出画面。
    // 故把目标点下移到「模型顶 ↔ 地面」的中点（x/z 仍为 0），保持 ApplyInput 不受影响
    // （它只改 phi/theta/R，与目标点无关）。
    float camera_target_y_ = 0.0f;
    // 仿真是否推进（暂停时保持当前形变，只停物理不重置）。
    void SetDynamicsRunning(bool running) { dynamics_running_ = running; }
    bool dynamics_running() const { return dynamics_running_; }

    // 注入真实字体文本测量回调（UI 布局用），Init() 后调一次。
    void InstallTextMeasure() {
        ui_.SetTextMeasure(&ArapViewerApp::AppTextWidth, this);
    }

    // ── 视角 / 光照 / 地面状态（装配后由 main 初始化；交互面板实时改）──
    jpov_viewer::ViewConfig view_;
    float elev_deg_  = 55.0f;    // 太阳仰角（度 [0,90]）
                                 //   默认不取 90°（天顶）：正顶光会让所有竖直面 N·L≈0
                                 //   只吃 ambient 而发黑，查看方块的侧面时观感差。
    float turbidity_ = 2.0f;     // 大气浊度 [2,8]
    float season_r_  = 1.0f;     // 季节 R 色温乘子 [0.5,2.0]
    float ground_y_  = -1.5f;    // 地面高度（世界坐标；装载时按模型包围盒自动定位）
    // 地面滑条范围：装载时按模型尺寸自适应（大模型不能塞进固定 [-3,3]）。
    float ground_y_min_ = -3.0f;
    float ground_y_max_ = 3.0f;

    // 物理参数（三个材料常数在代码里 + 三个面板滑条）。
    //   **代码常量**：胡克 c、ARAP c'（材料属性，不该随手改）。
    //   **面板可调**：面密度 ρ（倍率）、重力 g、阻尼 u。
    //     加 ρ 滑条的理由（需求方 2026-09-23）：ρ 与 g 都影响"自重 vs 刚度"⇒ 都能救塌陷。
    //     ⚠️ 但两者副作用**相反**（关键，必须写清）：
    //       · 降 g：自重↓ ⇒ 更容易撑住；**稳定上限不变**（稳定只看 c/ρ，与 g 无关）—— 纯赚。
    //       · 降 ρ：自重↓ ⇒ 更容易撑住；但 k_e/m = c/ρ ↑ ⇒ **稳定上限↓、更容易炸**。
    //         ⇒ "撑住"与"稳定"对 ρ 的要求是相反的（想撑住要轻、想稳要重）。
    jpov_arap::ArapSimConfig sim_config_;
    float gravity_ = 1.0f;      // 面板滑条：重力大小（方向恒为 −y）
    float damping_ = 0.1f;      // 阻尼 u（1/s；加速度域 −u·v，与质量无关）
    // 面密度滑条 = **相对基准值的倍率**（0.1× ~ 10×）。用倍率而非绝对值，便于换材料后复用。
    float density_scale_ = 1.0f;
    // 面密度**基准**：装载完成时从 sim_config_ 抓一次（这样命令行/程序化的设置会成为基准），
    // 之后面板的倍率滑条都是相对它。0 = 尚未抓取（BuildSim 末尾会填）。
    float density_base_kg_per_m2_ = 0.0f;

    // 已加载的 primitive 列表（main 用来算包围盒做相机自适应）。
    const std::vector<SimPrimitive>& primitives() const { return prims_; }

    // 模型包围盒（在 BuildSim 里由装载后的 CPU 网格算出）。
    // 地面自动定位与相机取景都读它——两处必须同源，否则会分叉。
    const jpov::Vec3f& model_min() const { return model_min_; }
    const jpov::Vec3f& model_max() const { return model_max_; }

    // 固定物理子步数（≤ 0 / 未调用 = 按 max_stable_dt() 自动派生）。
    // 命令行 --substeps 用它覆盖，便于做“同一步长下多扫几个子步”的对照实验。
    void SetFixedSubsteps(int substeps) { fixed_substeps_ = substeps; }

    // 全局唯一的仿真器（全部 primitive 合成的一个物理物体）。供 headless 诊断读取
    // 质点/畸变/退化旋转等量。
    const jpov_arap::ArapSim& sim() const { return sim_; }

    // 统一渲染体：交互逐帧与 headless 逐帧共用（零分叉）。
    void OneIteration(int64_t frame_count, const jpov::InputSnapshot& input,
                      const jpov::WindowInfo& winfo,
                      jpov::RenderCommandList* cmds /*output*/) override;

private:
    // UI 文本宽度回调适配。
    static float AppTextWidth(const char* text, float font_size,
                              const char* /*font_alias*/, void* userdata);

    // 把本帧的物理推进一格，并把形变结果同步到 GPU（写回 → 重算 TN → 上传）。
    // 仅在 dynamics_running_ 为真时调用。
    //
    // 内部会自己算“安全步长 physics_dt”（自动模式下 = min(1/60 s, 2.5·max_stable_dt)），
    // 外部无需关心。
    //
    // frame_dt 是渲染帧时长（1/60 s）；物理固定步长 kPhysicsDt = 0.05 s，用累加器
    // 凑够才推进一步（物理按真实时间演化，且与帧率抖动无关）。
    void AdvanceDynamics(float frame_dt);

    // 绘制底部左列 4 光照/标高滑条 + 右列 4 物理滑条 + 顶部 2 按钮 + 状态文本（仅交互窗口）。
    void DrawPanel(const jpov::InputSnapshot& input);

    // 把一个（已备好 flag/法线/切线的）CPU 网格接成一个可仿真 primitive：
    // 建仿真器 + 注册动态 GPU 网格 + 推入 prims_。文件装载与内置方块共用此路径。
    // 返回 true 表示已成功接入。
    //
    // Pre-condition: Init() 已调用；mesh 非空且有索引；material 的纹理句柄已注册。
    bool AddPrimitive(jpov::MeshData mesh, const jpov::PBRMaterial& material,
                      const char* label /*诊断用名称，非空*/);

    // 把当前全部 primitive 合成【一个】物理物体建仿真（跨 primitive 焊接）。
    // Pre-condition: prims_ 非空（AddPrimitive 已完成各 primitive 的注册）。
    void BuildSim();

    // 由 prims_ 的当前位置算模型包围盒 → model_min_ / model_max_。
    // Pre-condition: prims_ 非空。
    void ComputeModelBounds();

    // 释放某个 primitive 的 GL 网格（幂等）。
    void ReleasePrimitiveMesh(SimPrimitive* prim);

    std::vector<SimPrimitive> prims_;
    // 全部 primitive 共用【一个】仿真器：跨 primitive 焊接（见 arap_sim.h 文件头）。
    jpov_arap::ArapSim sim_;
    jpov::Vec3f model_min_ = jpov::Vec3f(0.0f, 0.0f, 0.0f);
    jpov::Vec3f model_max_ = jpov::Vec3f(0.0f, 0.0f, 0.0f);
    int fixed_substeps_ = 0;         // 0 = 自动派生（见 AdvanceDynamics）
    int substeps_in_use_ = 1;        // 当前实际用的子步数（面板显示用）
    jpov::GltfObject gltf_;          // 持有材质/纹理的 GPU 资源（本 App 不释放其网格）
    bool has_gltf_ = false;

    uint32_t ground_mesh_ = 0;                 // 地面 quad 的 GPU handle
    jpov::PBRMaterial ground_mat_;             // 地面材质（高粗糙灰）
    float ground_y_prev_ = 0.0f;               // 上一帧地面高度（变化才重建 quad）

    bool show_panel_ = true;                   // 是否绘制交互面板
    bool dynamics_running_ = false;            // 仿真是否推进（默认暂停，按按钮启动）
    bool diverged_ = false;                    // 数值爆炸后已回滚暂停（重置/再次点“运行”时清除）
    bool large_deform_warned_ = false;         // “形变很大”只提示一次（避每帧刷屏）
    float physics_accum_ = 0.0f;               // 物理时间累加器（默认步长 0.05 s）
    bool slow_motion_warned_ = false;           // 慢动作提示只打一次
    // 每帧开始时的状态快照：发散时用它回滚（保证屏幕不出现 NaN 几何）。
    std::vector<jpov::Vec3f> snapshot_pos_;
    std::vector<jpov::Vec3f> snapshot_vel_;

    // 物理积分步长（秒）。默认 0.05 s（需求方 2026-09-23 指定）。
    static constexpr float kPhysicsDt = 0.05f;
    // 子步数上限。**需求方 2026-09-23 明确：最多接受 60 Hz（h ≥ 1/60 s）**，
    // 再细分不可接受 ⇒ 自动模式下不做"加子步"这种补救（材质自己得满足稳定条件）；
    // 需要更多子步必须用 --substeps 显式指定（自行承担"违反 60 Hz 约束"）。
    static constexpr int kMaxAutoSubsteps = 1;

    // 由稳定上限派生本帧子步数：h = kPhysicsDt/substeps ≤ 0.5·上限（留一倍安全余量）。
    // fixed > 0（命令行强制）时直接用 fixed；上限 ≤ 0（无刚度）时返回 1。
    static int SimSubstepsFor(float bound_s, int fixed);

    jpov::Ui ui_;                              // 跨帧持有（滑条/按钮状态）
    static constexpr float kPanelFontSize = 16.0f;
    static constexpr float kPanelRowH = 30.0f;
    static constexpr float kPanelSpacing = 12.0f;
};

}  // namespace jpov_arap_viewer

// ───────────────────────────── 实现 ─────────────────────────────
//
// 本 App 的实现内联在头文件里（与 viewer_app.h / skylight_viewer_app.h 同款约定：
// 查看器 App 是"装配体"，逻辑短且与 JPOV 基类生命周期紧耦合，独立 .cc 反而增加
// 构建/依赖噪音）。纯算法（仿真、TN 重算）已分别落在 arap_sim.cc / mesh_geometry.h。

#include <cmath>

#include <glog/logging.h>

namespace jpov_arap_viewer {

// LoadGltfScene 的回调上下文：把每个 primitive 的 CPU 网格收进 vector。
namespace {

struct SceneCollectCtx {
    std::vector<jpov::MeshData>* meshes;
};

void CollectMeshEntry(const jpov::GltfMeshEntry* entry, void* user_data) {
    CHECK_NOTNULL(entry);
    SceneCollectCtx* ctx = static_cast<SceneCollectCtx*>(user_data);
    ctx->meshes->push_back(entry->mesh);  // 拷贝：MeshData 可整体拷贝（mesh.h 已保证）
}

}  // namespace

inline float ArapViewerApp::AppTextWidth(const char* text, float font_size,
                                         const char* /*font_alias*/,
                                         void* userdata) {
    ArapViewerApp* app = static_cast<ArapViewerApp*>(userdata);
    return app->MeasureTextWidth(/*alias=*/std::string(),
                                 /*text=*/text ? text : "", font_size);
}

inline int ArapViewerApp::SimSubstepsFor(float bound_s, int fixed) {
    (void)bound_s;
    if (fixed > 0) {
        return fixed;   // 命令行显式指定：由调用方承担（可能违反 60 Hz 约束）
    }
    // 自动模式下**不加子步**：需求方限制 60 Hz，所以步长就是 1/60 s 起，
    // 稳定与否完全由材质（ρ / c / c'）决定。不够稳就慢动作（见 AdvanceDynamics），
    // 而不是靠内部细分把算力堆上去。
    return 1;
}

inline void ArapViewerApp::ComputeModelBounds() {
    CHECK(!prims_.empty());
    bool first = true;
    for (const SimPrimitive& prim : prims_) {
        for (const jpov::Vec3f& p : prim.mesh.positions) {
            if (first) {
                model_min_ = p;
                model_max_ = p;
                first = false;
                continue;
            }
            model_min_ = jpov::Vec3f(std::min(model_min_.x(), p.x()),
                                     std::min(model_min_.y(), p.y()),
                                     std::min(model_min_.z(), p.z()));
            model_max_ = jpov::Vec3f(std::max(model_max_.x(), p.x()),
                                     std::max(model_max_.y(), p.y()),
                                     std::max(model_max_.z(), p.z()));
        }
    }
    CHECK(!first) << "ArapViewerApp::ComputeModelBounds: 所有 primitive 都没有顶点";
}

inline void ArapViewerApp::BuildSim() {
    CHECK(!prims_.empty());
    std::vector<jpov::MeshData> meshes;
    meshes.reserve(prims_.size());
    for (const SimPrimitive& p : prims_) {
        meshes.push_back(p.mesh);   // 拷贝：MeshData 可整体拷贝
    }

    // ── 模型包围盒 + 地面自动定位（**必须在 Build 之前**）──
    //   Build → Reset 会做一次「初始地面投射」。若此刻 ground_y 还是默认值、而模型
    //   底部低于它，模型会被硬压扁 ⇒ 纯人为的初始条件错误（实测：灯罩最低点
    //   y ≈ −3.0 而默认 ground_y = −3.0，t=0 就被压出 5% 边长畸变 + 7% 退化旋转）。
    //   故先把地面放到「模型底部下方 0.5 个模型高度」（⇒ 摔落高度 = 0.5 个模型高）。
    ComputeModelBounds();
    const float model_h = std::max(1e-4f, model_max_.y() - model_min_.y());
    ground_y_ = model_min_.y() - 0.5f * model_h;
    // 滑条范围与模型尺度挂钩（固定 [-3,3] 装不下 25 m 高的路灯）。
    ground_y_min_ = model_min_.y() - 3.0f * model_h;
    ground_y_max_ = model_min_.y() + 0.5f * model_h;
    ground_y_prev_ = ground_y_;
    sim_config_.ground_y = ground_y_;

    // ── 质量诊断 + 稳定步长的**先验**提示（都在 Build 前完成）──
    LOG(INFO) << "模型包围盒（米）：[" << model_min_.x() << "," << model_min_.y()
              << "," << model_min_.z() << "] ~ [" << model_max_.x() << ","
              << model_max_.y() << "," << model_max_.z() << "]";
    const jpov_arap::MassDiagnostics mass = jpov_arap::AnalyzeMasses(meshes, sim_config_);
    LOG(INFO) << "面密度诊断（构建前）：总面积 " << mass.total_area_m2
              << " m² ⇒ 总质量 " << mass.total_mass_kg << " kg（"
              << sim_config_.area_density_kg_per_m2 << " kg/m²）";
    // 稳定步长**先验**（不建仿真器）：面积定标下 row_i ≈ n·c/ρ + c'/ρ·n/(n+1)，
    // **与网格密度、与尺度都无关**，所以直接由材质常数给一个量级估计就够用。
    if (mass.total_area_m2 > 0.0f) {
        const float rho = sim_config_.area_density_kg_per_m2;
        const float c = sim_config_.spring_stiffness_per_area;
        const float cp = sim_config_.arap_stiffness_per_area;
        // 平均 1-ring 度按三角网格取 6（角点/边界会小些，这里只要量级）。
        const float approx_row = 6.0f * c / rho + cp / rho * (6.0f / 7.0f);
        const float approx_bound =
            approx_row > 0.0f ? 2.0f / std::sqrt(approx_row) : 1e9f;
        LOG(INFO) << "稳定步长先验（面积定标 ⇒ 与网格密度无关）：row ≈ "
                  << approx_row << " ⇒ 上限 ≈ " << approx_bound
                  << " s（需求方要求 h ≥ 1/60 = " << (1.0f / 60.0f) << " s）";
        if (approx_bound < 1.0f / 60.0f) {
            LOG(WARNING)
                << "材质太硬：先验上限 " << approx_bound << " s < 1/60 s ⇒ 60 Hz 会发散。"
                   "只能降 c（更软材料）或降 c'/ρ（更薄/更重）—— 换更硬的子步不可行（需求方限 60 Hz）";
        }
    }

    sim_.Build(meshes, sim_config_);
    // 把**当前** ρ 记作基准（命令行 --area_density 会在这之前设进 sim_config_，
    // 故它成为基准、面板倍率随后相对它生效）。0 保护：避免基准为 0 导致质量恒为 0。
    if (density_base_kg_per_m2_ <= 0.0f) {
        density_base_kg_per_m2_ = sim_config_.area_density_kg_per_m2;
    }
    CHECK_GT(density_base_kg_per_m2_, 0.0f)
        << "ArapViewerApp: 面密度基准必须为正";
    LOG(INFO) << "物理物体：网格 " << sim_.mesh_count() << " 个，顶点 "
              << sim_.vertex_count() << "，焊接质点 " << sim_.particle_count()
              << "（跨 primitive 焊接 ⇒ 各部件互相支撑）";
    LOG(INFO) << "材质（面积定标）: ρ=" << sim_config_.area_density_kg_per_m2
              << " kg/m²（基准 " << density_base_kg_per_m2_ << " × 倍率 "
              << density_scale_ << "）⇒ 总质量 " << sim_.total_mass() << " kg / 总面积 "
              << sim_.surface_area() << " m²; 胡克 c="
              << sim_config_.spring_stiffness_per_area << " N/m³; ARAP c'="
              << sim_config_.arap_stiffness_per_area << " N/m³; 阻尼 u="
              << sim_config_.damping_per_second << "/s; 重力 g=" << gravity_
              << " m/s²; 子步=" << sim_config_.substeps;
    const float bound = sim_.max_stable_dt();
    LOG(INFO) << "显式积分稳定上限估计 max_stable_dt=" << bound
              << " s（目标步长 1/60 = " << (1.0f / 60.0f)
              << " s；需求方上限 60 Hz，不加子步）";
    // 与 AdvanceDynamics 的安全步长判据**保持同一依据**（safe_dt = 0.7·bound，
    // 实测标定，见那里的注释），避免"一处说会慢动作、另一处说没问题"的自相矛盾。
    constexpr float kTargetDtLog = 1.0f / 60.0f;
    constexpr float kSafeDtFactorLog = 0.7f;
    if (bound > 0.0f && kTargetDtLog > kSafeDtFactorLog * bound) {
        LOG(WARNING) << "材质偏硬：稳定上限 " << bound
                     << " s，安全步长 " << (kSafeDtFactorLog * bound) << " s < 1/60 s"
                     << " ⇒ 会进入慢动作（不会出 NaN，但物理变慢）。"
                        "要全速需降 c（更软材料）或升 ρ（更重/更厚）";
    }
}

inline bool ArapViewerApp::AddPrimitive(jpov::MeshData mesh,
                                        const jpov::PBRMaterial& material,
                                        const char* label) {
    CHECK_NOTNULL(label);
    SimPrimitive prim;
    prim.mesh = std::move(mesh);
    prim.material = material;

    // 仿真要求：有位置 + 索引。缺一则该 primitive 无法仿真，直接报错退出
    // （不静默跳过——静默少画一个 primitive 是最难查的坑）。
    CHECK_GT(prim.mesh.positions.size(), 0u)
        << "ArapViewerApp: " << label << " 无顶点";
    CHECK(!prim.mesh.indices.empty())
        << "ArapViewerApp: " << label << " 无索引（non-indexed 网格本 PR 不支持仿真）";

    // 法线：几何管线要求 DrawObject3D 的 mesh 含 kNormal。若资产无 NORMAL，
    // 补上 flag 并立即用几何重算（第 0 帧的 RecomputeTangentSpace）。
    if (!jpov::MeshHasFlag(prim.mesh.flags, jpov::MeshVertexFlags::kNormal)) {
        LOG(WARNING) << label << " 资产无 NORMAL，改用几何自推法线";
        prim.mesh.flags = static_cast<jpov::MeshVertexFlags>(
            static_cast<uint8_t>(prim.mesh.flags) |
            static_cast<uint8_t>(jpov::MeshVertexFlags::kNormal));
    }
    // 切线：法线贴图必需（object3d_renderer 在 normal_tex != 0 时 CHECK 该 flag）。
    // 有 UV 时就声明 kTangent（与既有 loader 行为一致），随后由
    // RecomputeTangentSpace 填值。
    const bool has_uv =
        jpov::MeshHasFlag(prim.mesh.flags, jpov::MeshVertexFlags::kUV) &&
        prim.mesh.uvs.size() == prim.mesh.positions.size();
    if (has_uv &&
        !jpov::MeshHasFlag(prim.mesh.flags, jpov::MeshVertexFlags::kTangent)) {
        prim.mesh.flags = static_cast<jpov::MeshVertexFlags>(
            static_cast<uint8_t>(prim.mesh.flags) |
            static_cast<uint8_t>(jpov::MeshVertexFlags::kTangent));
    }
    // 用几何重算 TN（load 时的「第 0 次」；之后每帧复用同一个函数）。
    jpov::RecomputeTangentSpace(&prim.mesh);

    // 材质若带法线贴图但网格无有效切线 → 摘掉法线贴图（否则渲染侧 CHECK 崩溃）。
    if (prim.material.normal_tex != 0 &&
        !jpov::MeshHasFlag(prim.mesh.flags, jpov::MeshVertexFlags::kTangent)) {
        LOG(WARNING) << label << " 无有效 UV/切线，无法构建 TBN；摘除法线贴图";
        prim.material.normal_tex = 0;
    }

    prim.mesh.Validate();
    prim.mesh_id = RegisterMesh(prim.mesh);
    CHECK_NE(prim.mesh_id, 0u) << "RegisterMesh 返回 0";

    LOG(INFO) << label << ": 顶点 " << prim.mesh.positions.size() << "，三角形 "
              << prim.mesh.indices.size() / 3;
    prims_.push_back(std::move(prim));
    return true;
}

inline bool ArapViewerApp::LoadModel(const std::string& path) {
    CHECK(!path.empty()) << "ArapViewerApp::LoadModel: path 不能为空";

    // ── 1. GPU 侧：材质 + 纹理（GltfObject 独占这些句柄，本 App 负责整体释放）。──
    gltf_ = LoadGltf(path);
    if (gltf_.empty()) {
        LOG(ERROR) << "ArapViewerApp::LoadModel: LoadGltf 失败: " << path;
        return false;
    }
    has_gltf_ = true;

    // ── 2. CPU 侧：逐 primitive 的几何（供仿真）。──
    std::vector<jpov::MeshData> meshes;
    SceneCollectCtx ctx{&meshes};
    if (!jpov::LoadGltfScene(path, &CollectMeshEntry, &ctx) || meshes.empty()) {
        LOG(ERROR) << "ArapViewerApp::LoadModel: LoadGltfScene 失败: " << path;
        ReleaseModel();
        return false;
    }

    // 两条路径都遍历同一份 scene 的 primitive，顺序一致（Renderer::LoadGltf 内部
    // 也走 LoadGltfScene 收集）；数量不等说明装载链路不一致，拒绝静默继续。
    CHECK_EQ(meshes.size(), gltf_.primitives.size())
        << "ArapViewerApp::LoadModel: CPU primitive 数(" << meshes.size()
        << ") != GPU primitive 数(" << gltf_.primitives.size()
        << ")，装载链路不一致";

    prims_.clear();
    prims_.reserve(meshes.size());
    for (size_t i = 0; i < meshes.size(); ++i) {
        // 材质在此【拷贝】一份到 SimPrimitive（含纹理句柄）。
        // 纹理本体仍归 gltf_ 所有（由 ReleaseGltf 释放）；拷贝的只是句柄值，
        // 生命周期以内 gltf_ 存活为前提（本 App 保证 ReleaseModel 前不改 gltf_）。
        const std::string label =
            "primitive " + std::to_string(i);
        AddPrimitive(std::move(meshes[i]), gltf_.primitives[i].material,
                     label.c_str());
    }
    BuildSim();   // 全部 primitive 合成【一个】物理物体

    ground_mat_ = jpov_viewer::GroundMaterial();
    ground_mesh_ = RegisterMesh(jpov_viewer::MakeGroundQuad(ground_y_));
    ground_y_prev_ = ground_y_;
    return true;
}

inline bool ArapViewerApp::LoadBuiltinBox() {
    prims_.clear();
    // 方块整体尺度 ≈ 真实资产量级（边长 0.9），便于与加载 glb 的观感直接对比。
    const jpov::MeshData box = jpov::MeshData::MakeBox(/*front*/ 0.45f,
                                                      /*up*/ 0.45f,
                                                      /*left*/ 0.45f);
    const jpov::PBRMaterial mat = jpov::PBRMaterial::SolidColorMR(
        /*color*/ {0.55f, 0.62f, 0.72f, 1.0f}, /*metallic*/ 0.0f,
        /*roughness*/ 0.6f);
    AddPrimitive(box, mat, "内置方块");
    BuildSim();

    ground_mat_ = jpov_viewer::GroundMaterial();
    ground_mesh_ = RegisterMesh(jpov_viewer::MakeGroundQuad(ground_y_));
    ground_y_prev_ = ground_y_;
    return !prims_.empty();
}

inline void ArapViewerApp::ReleasePrimitiveMesh(SimPrimitive* prim) {
    if (prim->mesh_id != 0) {
        ReleaseMesh(prim->mesh_id);
        prim->mesh_id = 0;
    }
}

inline void ArapViewerApp::ReleaseModel() {
    for (SimPrimitive& prim : prims_) {
        ReleasePrimitiveMesh(&prim);
    }
    prims_.clear();
    sim_ = jpov_arap::ArapSim{};
    if (ground_mesh_ != 0) {
        ReleaseMesh(ground_mesh_);
        ground_mesh_ = 0;
    }
    if (has_gltf_) {
        ReleaseGltf(gltf_);
        gltf_ = jpov::GltfObject{};
        has_gltf_ = false;
    }
}

inline void ArapViewerApp::AdvanceDynamics(float frame_dt) {
    // 面板上的 ρ / g / u / 地面高度每帧同步进物理（改动即立刻生效）；
    // 胡克 c 与 ARAP c' 是材料常数，只在 sim_config_ 初值处配置。
    sim_config_.gravity_magnitude = gravity_;
    sim_config_.damping_per_second = damping_;
    // 面密度 = 基准 × 倍率。滑条已把下限夹在 0.1×，这里再兜一层（防 0/负 ⇒ 质量 0 ⇒ 数值飞走）。
    sim_config_.area_density_kg_per_m2 =
        std::max(density_base_kg_per_m2_ * density_scale_, 1e-3f);
    sim_config_.ground_y = ground_y_;

    // 子步数：当前参数下的稳定上限可能小于 0.05 s ⇒ 每帧重算（同一格式，只改分辨率）。
    sim_.SetConfig(sim_config_);
    substeps_in_use_ = SimSubstepsFor(/*bound_s=*/sim_.max_stable_dt(),
                                      /*fixed=*/fixed_substeps_);
    sim_config_.substeps = substeps_in_use_;
    sim_.SetConfig(sim_config_);

    // ── 安全步长（自动模式下）──
    //   若封顶后 h 仍超过稳定上限，就**放大实际步长**（慢动作）而不是硬推：
    //   显式积分一旦 h > 上限 就必然发散（NaN/飞走），对用户就是「模型消失」。
    //   力模型、积分格式、substeps 都不变，只是物理时间走得慢一点。
    // 本帧开始时的状态快照（发散回滚用；只在真要推进时才存）。
    if (dynamics_running_) {
        sim_.SnapshotState(&snapshot_pos_, &snapshot_vel_);
    }

    // 目标步长 = 1/60 s（需求方允许的最细物理频率，正好一个渲染帧）。
    constexpr float kTargetDt = 1.0f / 60.0f;
    float physics_dt = kTargetDt;
    if (fixed_substeps_ <= 0) {
        // 自动模式：材质不够软时**放大步长（慢动作）**，而不是加子步。
        //
        // 安全步长：以 max_stable_dt() 为基准、乘**实测标定的安全系数**。
        //
        // ⚠️ 这个系数不是理论值，是量出来的：max_stable_dt() 用 Gershgorin 行和估 ω²，
        //   它假设的是**纯弹簧**系统；而本模型还含 ARAP 形状力（耦合整个 1-ring，
        //   非对角耦合比行和估计强）与地面硬钳位（制造瞬时压缩）。实测两组材质：
        //     · c=15000（橡胶皮）⇒ 报 0.01015 s，实测稳的边界 ≈ 0.015 s（偏乐观 1.5×）
        //     · c=150 （软 100 倍）⇒ 报 0.0731 s，实测 h=0.05 s 都稳
        //   取 1.0（即直接用 max_stable_dt）在这两组上都偏乐观 ⇒ 不能这么用。
        //   实测校准值：**0.7**（在 c=15000 这组上 0.7·0.01015 = 0.0071 s < 0.015 ✓）。
        //   宁可慢一点也不要炸（炸一下整个画面就没用了）。
        constexpr float kSafeDtFactor = 0.7f;
        const float bound = sim_.max_stable_dt();
        const float safe_dt = kSafeDtFactor * bound;
        if (bound > 0.0f && kTargetDt > safe_dt) {
            physics_dt = std::max(safe_dt, 1e-4f);
            if (!slow_motion_warned_) {
                slow_motion_warned_ = true;
                LOG(WARNING) << "仿真进入慢动作：当前材质（c="
                             << sim_config_.spring_stiffness_per_area << " N/m³, ρ="
                             << sim_config_.area_density_kg_per_m2
                             << " kg/m²）太硬，1/60 s 步长会发散 ⇒ 实际步长 "
                             << physics_dt << " s（慢 " << (kTargetDt / physics_dt)
                             << " 倍；力模型未改，也没加子步）。"
                                "要全速只能换更软/更重的材质（降 c、升 ρ）";
            }
        }
    }

    // 固定步长 + 累加器：渲染帧凑够 physics_dt 才推进一步。
    // physics_dt 目标值 = 1/60 s（**需求方限制的最细物理频率**；正好等于一个渲染帧，
    // 于是每帧推进一步、无累加余数）。若材质太硬导致 1/60 s 不稳定，则放大步长（慢动作）。
    physics_accum_ += frame_dt;
    while (physics_accum_ >= physics_dt) {
        sim_.Step(physics_dt);
        physics_accum_ -= physics_dt;
    }

    // ── 发散保护（挡住 NaN，不让它进 GPU）──
    //   显式积分在 h 超过稳定上限时**必然**发散：位置先爆到 ±1e30，然后变成 NaN，
    //   于是顶点缓冲全 NaN ⇒ 模型在原地消失（实测症状就是这个）。
    // ── 保护：只在“真的数值爆炸”时回滚；不要把“物理上撞塌了”当成发散 ──
    //   两类要分清（我上一版把它们混在一起，导致抛石机一落地就报“发散”）：
    //     ① **数值爆炸**：位置出现 NaN/inf。这是积分不稳定的硬信号，必须回滚。
    //     ② **物理塌陷**：几何完好但被压扁（边长畸变变大）。这不是 bug——
    //        1 cm 橡胶皮撞地就是会塔，判它“发散”等于把正确的物理当错误。
    //        故只打一条 WARNING（且只打一次），不暂停、不回滚，让人看到真实结果。
    bool non_finite = false;
    for (uint32_t i = 0; i < sim_.particle_count(); ++i) {
        const jpov::Vec3f& p = sim_.vertex_positions()[i];
        if (!std::isfinite(p.x()) || !std::isfinite(p.y()) ||
            !std::isfinite(p.z())) {
            non_finite = true;
            break;
        }
    }
    if (non_finite) {
        sim_.RestoreState(snapshot_pos_, snapshot_vel_);
        physics_accum_ = 0.0f;
        dynamics_running_ = false;
        diverged_ = true;
        LOG(ERROR) << "仿真数值爆炸（出现 NaN/inf）：已回滚到本帧开始的状态并暂停。"
                      "这是**积分不稳定**的信号 ⇒ 把步长 （"
                   << physics_dt << " s）降到稳定上限（"
                   << sim_.max_stable_dt() << " s）以下，或降 c / 升 ρ（更软/更重的材质）";
        return;   // 不把 NaN 几何送到 GPU
    }

    // 几何完好但形变很大 ⇒ 这是物理结果，只提示一次（不暂停、不回滚、不每帧刷屏）。
    {
        const float dist = sim_.edge_distortion_rms();
        if (!(dist >= 0.0f) || dist > 1.0f) {
            if (!large_deform_warned_) {
                large_deform_warned_ = true;
                LOG(WARNING) << "模型形变很大（边长畸变 " << dist
                             << "）：几何仍然完好，这是**物理结果**（材质太软/自重/撞地），"
                                "不是数值发散。若觉得不合理，换更硬/更厚的材质（升 c、升 ρ）";
            }
        }
    }

    // 顺序铁律：写回位置 → 重算 TN → 上传。法线/切线是位置的派生量。
    std::vector<jpov::MeshData> meshes;
    meshes.reserve(prims_.size());
    for (const SimPrimitive& p : prims_) {
        meshes.push_back(p.mesh);
    }
    sim_.WriteBackPositions(&meshes);
    for (size_t i = 0; i < prims_.size(); ++i) {
        prims_[i].mesh = std::move(meshes[i]);
        jpov::RecomputeTangentSpace(&prims_[i].mesh);
        UpdateMesh(prims_[i].mesh_id, prims_[i].mesh);
    }
}

inline void ArapViewerApp::OneIteration(int64_t frame_count,
                                        const jpov::InputSnapshot& input,
                                        const jpov::WindowInfo& winfo,
                                        jpov::RenderCommandList* cmds) {
    CHECK_NOTNULL(cmds);
    (void)frame_count;

    cmds->camera.fbo_3d_width_ = kViewerWidth;
    cmds->camera.fbo_3d_height_ = kViewerHeight;

    // 交互输入 → 视角（headless 无用户，不消费输入，保持外部设定的 view_）。
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
        jpov_viewer::ApplyInput(&view_, dx, dy, scroll,
                                static_cast<int>(winfo.width),
                                static_cast<int>(winfo.height));
    }

    // ── 相机 ──
    // 位置 = 取景中心 + 球面角偏移（view_.Position() 是相对目标点的偏移，因为
    // ViewConfig 的目标点恒为原点）。
    const jpov::Vec3f target = {0.0f, camera_target_y_, 0.0f};
    const jpov::Vec3f offset = view_.Position();
    cmds->camera.position = target + offset;
    cmds->camera.target = target;
    cmds->camera.up = {0.0f, 1.0f, 0.0f};
    cmds->camera.fov = 60.0f;
    cmds->camera.near = 0.05f;
    cmds->camera.far = 1000.0f;

    // ── 光照（与 model viewer 同款 sky 推导）──
    const jpov_viewer::NoonLighting light =
        jpov_viewer::MakeLighting(elev_deg_, turbidity_, season_r_);
    cmds->sky = light.sky;
    cmds->sun = light.sun;
    cmds->ambient = light.ambient;
    cmds->tone_mapping = true;

    // ── 物理推进 + 几何同步（本帧唯一改动几何的地方）──
    //   用固定 dt = 1/target_fps（确定性，便于出 gold；不用真实墙钟，避免帧率抖动
    //   让物理结果不可复现）。
    if (dynamics_running_) {
        AdvanceDynamics(1.0f / kViewerFps);
    }

    // ── 地面：高度变化时原地重建 quad ──
    if (ground_y_ != ground_y_prev_) {
        UpdateMesh(ground_mesh_, jpov_viewer::MakeGroundQuad(ground_y_));
        ground_y_prev_ = ground_y_;
    }
    cmds->DrawObject3D(ground_mesh_, ground_mat_,
                       /*center*/ {0.0f, 0.0f, 0.0f},
                       /*up*/ {0.0f, 1.0f, 0.0f},
                       /*front*/ {0.0f, 0.0f, 1.0f});

    // ── 被仿真的模型：每个 primitive 一张动态网格 ──
    for (const SimPrimitive& prim : prims_) {
        cmds->DrawObject3D(prim.mesh_id, prim.material,
                           /*center*/ {0.0f, 0.0f, 0.0f},
                           /*up*/ {0.0f, 1.0f, 0.0f},
                           /*front*/ {0.0f, 0.0f, 1.0f});
    }

    // ── 交互面板 ──
    if (show_panel_) {
        DrawPanel(input);
        ui_.End();
        ui_.Emit(cmds);
    }
}

inline void ArapViewerApp::DrawPanel(const jpov::InputSnapshot& input) {
    const float w = static_cast<float>(kViewerWidth);
    const float h = static_cast<float>(kViewerHeight);
    jpov::UiTheme theme = jpov::UiTheme::Default(kPanelFontSize);
    theme.font_alias = kViewerFontAlias;
    ui_.Begin(input, theme, w, h, /*frame_dt_ms*/ 1000.0f / kViewerFps);

    // ── 顶部两个按钮：动力学启停 / 重置形变 ──
    const float kBtnW = 200.0f;
    const float kBtnH = 36.0f;
    const float kBtnGap = 12.0f;
    const float btn_y = 16.0f;
    const char* run_label = dynamics_running_ ? "动力学：运行中 ⏸" : "动力学：已暂停 ▶";
    if (ui_.Button(run_label, jpov::UiRect{{16.0f, btn_y}, {kBtnW, kBtnH}})) {
        dynamics_running_ = !dynamics_running_;
        // 再次点“运行”= 用户明确要求继续 ⇒ 清掉数值爆炸标记与一次性的提示标记，
        // 让它能重新推进（否则 diverged_ 会把它永久钉在暂停上）。
        if (dynamics_running_) {
            diverged_ = false;
            large_deform_warned_ = false;
            physics_accum_ = 0.0f;
        }
    }
    if (ui_.Button("重置 mesh",
                   jpov::UiRect{{16.0f + kBtnW + kBtnGap, btn_y}, {kBtnW, kBtnH}})) {
        sim_.Reset();
        diverged_ = false;
        physics_accum_ = 0.0f;
        slow_motion_warned_ = false;
        large_deform_warned_ = false;   // 重置后重新允许一次“形变很大”提示
        std::vector<jpov::MeshData> meshes;
        meshes.reserve(prims_.size());
        for (const SimPrimitive& p : prims_) {
            meshes.push_back(p.mesh);
        }
        sim_.WriteBackPositions(&meshes);
        for (size_t i = 0; i < prims_.size(); ++i) {
            prims_[i].mesh = std::move(meshes[i]);
            jpov::RecomputeTangentSpace(&prims_[i].mesh);
            UpdateMesh(prims_[i].mesh_id, prims_[i].mesh);
        }
        // 重置后暂停，便于观察「回到 bind pose」；再按「运行」继续演化。
        dynamics_running_ = false;
    }

    // ── 底部 5 个半屏宽滑条 ──
    //   前 4 个是光照/标高（与 model viewer 同款）；最后一个 **重力 g** 是本查看器
    //   唯一暴露的物理量（其余物理参数固定在代码里，见 arap_viewer_app.h 文件头）。
    const float kSliderWidth = 0.5f * w;
    const float kBottom = 20.0f;
    const float left = (w - kSliderWidth) * 0.5f;
    const float top = h - kBottom - (7.0f * kPanelRowH + 6.0f * kPanelSpacing);

    ui_.SliderFloat("太阳仰角 °", &elev_deg_,
                    jpov::UiRect{{left, top}, {kSliderWidth, kPanelRowH}},
                    0.0f, 90.0f, /*decimal_places*/ 0);
    ui_.SliderFloat("浊度 turb", &turbidity_,
                    jpov::UiRect{{left, top + (kPanelRowH + kPanelSpacing)},
                                 {kSliderWidth, kPanelRowH}},
                    2.0f, 8.0f, /*decimal_places*/ 1);
    ui_.SliderFloat("季节 R", &season_r_,
                    jpov::UiRect{{left, top + 2.0f * (kPanelRowH + kPanelSpacing)},
                                 {kSliderWidth, kPanelRowH}},
                    0.5f, 2.0f, /*decimal_places*/ 2);
    ui_.SliderFloat("地面高度 y", &ground_y_,
                    jpov::UiRect{{left, top + 3.0f * (kPanelRowH + kPanelSpacing)},
                                 {kSliderWidth, kPanelRowH}},
                    ground_y_min_, ground_y_max_, /*decimal_places*/ 2);
    ui_.SliderFloat("重力 g", &gravity_,
                    jpov::UiRect{{left, top + 4.0f * (kPanelRowH + kPanelSpacing)},
                                 {kSliderWidth, kPanelRowH}},
                    0.0f, 30.0f, /*decimal_places*/ 2);
    ui_.SliderFloat("阻尼 u", &damping_,
                    jpov::UiRect{{left, top + 5.0f * (kPanelRowH + kPanelSpacing)},
                                 {kSliderWidth, kPanelRowH}},
                    0.0f, 5.0f, /*decimal_places*/ 2);
    // 面密度倍率：0.1× ~ 10×。下限夹紧：既防"太轻 ⇒ 数值飞走"，也避免"轻到没惯性"。
    // ⚠️ 降 ρ 能救塌陷，但会让稳定上限下降（k_e/m = c/ρ ↑）——副作用与降 g 相反。
    ui_.SliderFloat("面密度 ×ρ", &density_scale_,
                    jpov::UiRect{{left, top + 6.0f * (kPanelRowH + kPanelSpacing)},
                                 {kSliderWidth, kPanelRowH}},
                    0.1f, 10.0f, /*decimal_places*/ 2);

    // ── 状态文本：质点数 + 边长畸变（“有多软”）+ 形状力 + 退化旋转 + 子步数
    //     + 固定住的物理参数（面板不可改，故列出来便于对照）──
    if (!prims_.empty()) {
        char status[400];
        std::snprintf(status, sizeof(status),
                      "网格 %zu / 顶点 %zu / 质点 %zu   边长畸变 %.3f  形状力 %.4f  "
                      "退化旋转 %zu   子步 %d   ρ=%.1f kg/m²  c=%.0f  c'=%.0f N/m³  "
                      "总质量 %.1f kg   稳定上限 %.4f s",
                      sim_.mesh_count(), sim_.vertex_count(),
                      sim_.particle_count(),
                      static_cast<double>(sim_.edge_distortion_rms()),
                      static_cast<double>(sim_.shape_residual_rms()),
                      sim_.degenerate_rotation_count(), substeps_in_use_,
                      static_cast<double>(sim_config_.area_density_kg_per_m2),
                      static_cast<double>(sim_config_.spring_stiffness_per_area),
                      static_cast<double>(sim_config_.arap_stiffness_per_area),
                      static_cast<double>(sim_.total_mass()),
                      static_cast<double>(sim_.max_stable_dt()));
        ui_.Text(status, jpov::UiRect{{16.0f, btn_y + kBtnH + 8.0f},
                                     {1200.0f, 24.0f}},
                 /*stretch_w*/ false, /*stretch_h*/ false);
    }
}

}  // namespace jpov_arap_viewer

#endif  // JPOV_DEMO_ARAP_VIEWER_APP_H_
