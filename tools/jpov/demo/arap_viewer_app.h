// JPOV ARAP 查看器 — 渲染核心 App（可交互的软体动力学仿真宿主）
//
// 与 model viewer 并列的第二个交互式查看器：同样「加载 glTF + 可调视角/光照/地面」，
// 额外多出一套**动力学仿真**能力——把被加载的网格当作可变形软体（ARAP 弹性 + PBD
// 碰撞），在重力场里从高处摔到地面上，肉眼观察它是否呈现「充气城堡」式的回弹。
//
// 交互面板（底部 4 滑条 + 顶部 2 按钮）：
//   ① 太阳仰角 °       [0,90]      —— 与 model viewer 同款光照标定
//   ② 浊度 turb        [2,8]       —— 大气浊度（霾化 + 强度衰减）
//   ③ 季节 R           [0.5,2.0]   —— 色温乘子
//   ④ 地面高度 y       [-3,+3]     —— 摔落目标面的高度（实时改变 → 立刻看落点差异）
//   [动力学：运行/暂停] —— 仿真启停（暂停时不推进物理，仅保持当前形变）
//   [重置 mesh]         —— 把网格恢复到 bind pose（清空速度与形变）
//
// 重力/阻尼/迭代次数等物理参数本 PR 一律**代码内配置**（见 ArapViewerApp 的
// sim_config_ 初值），界面只暴露上面 4 个标高/光照量。
//
// ═══ 每帧链路（变形 → 渲染，顺序不可换）═══
//   Step(dt) → WriteBackPositions → RecomputeTangentSpace → UpdateMesh → Draw
//   其中 RecomputeTangentSpace 是**必做**项而非优化：法线/切线是位置的派生量，
//   几何变形后必须用新位置重算，否则光照用「旧形状法线」照「新形状几何」。
//
// ═══ 多 primitive 的处理 ═══
//   每个 glTF primitive 独立仿真、独立绘制。这不是偷懒而是**正确**：primitive 之间
//   不共享顶点，ARAP 的 Laplacian/1-ring 本来就跨不过 primitive 边界，强行焊接反而
//   会引入虚假连接。材质亦各自独立（不共用首 primitive 的材质）。

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
    // 只能靠压缩来响应碰撞，因此 ARAP 的“弹性/回弹”行为在这里能干净地看到；
    // 而真实资产的形状往往可以选择“翻倒”这条零能量路径（ARAP 允许等距弯曲/旋转），
    // 于是看不到明显压缩。需求方验收“充气城堡”直觉时用这个最清楚。
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

    // 物理参数（本 PR 代码内配置；界面不暴露，便于做"只改一个变量"的对照实验）。
    jpov_arap::ArapSimConfig sim_config_;

    // 已加载的 primitive 列表（main 用来算包围盒做相机自适应）。
    const std::vector<SimPrimitive>& primitives() const { return prims_; }

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

    // 把本帧的物理状态推进一格，并把形变结果同步到 GPU（写回 → 重算 TN → 上传）。
    // 仅在 dynamics_running_ 为真时调用。
    void AdvanceDynamics(float dt_seconds);

    // 绘制底部 4 滑条 + 顶部 2 按钮 + 状态文本（仅交互窗口）。
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

    // 释放某个 primitive 的 GL 网格（幂等）。
    void ReleasePrimitiveMesh(SimPrimitive* prim);

    std::vector<SimPrimitive> prims_;
    // 全部 primitive 共用【一个】仿真器：跨 primitive 焊接（见 arap_sim.h 文件头）。
    jpov_arap::ArapSim sim_;
    jpov::GltfObject gltf_;          // 持有材质/纹理的 GPU 资源（本 App 不释放其网格）
    bool has_gltf_ = false;

    uint32_t ground_mesh_ = 0;                 // 地面 quad 的 GPU handle
    jpov::PBRMaterial ground_mat_;             // 地面材质（高粗糙灰）
    float ground_y_prev_ = 0.0f;               // 上一帧地面高度（变化才重建 quad）

    bool show_panel_ = true;                   // 是否绘制交互面板
    bool dynamics_running_ = false;            // 仿真是否推进（默认暂停，按按钮启动）
    bool diverged_ = false;                    // 已判出发散（见 AdvanceDynamics 的保护）

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

inline void ArapViewerApp::BuildSim() {
    CHECK(!prims_.empty());
    std::vector<jpov::MeshData> meshes;
    meshes.reserve(prims_.size());
    for (const SimPrimitive& p : prims_) {
        meshes.push_back(p.mesh);   // 拷贝：MeshData 可整体拷贝
    }
    sim_.Build(meshes, sim_config_);
    LOG(INFO) << "物理物体：网格 " << sim_.mesh_count() << " 个，顶点 "
              << sim_.vertex_count() << "，焊接质点 " << sim_.particle_count()
              << "（跨 primitive 焊接 ⇒ 各部件互相支撑）";
    LOG(INFO) << "物理参数: g=(" << sim_config_.gravity.x() << ","
              << sim_config_.gravity.y() << "," << sim_config_.gravity.z()
              << ") 阻尼=" << sim_config_.velocity_damping_per_second
              << " 形状恢复率=" << sim_config_.shape_restore_rate_per_second
              << " 边长恢复率=" << sim_config_.stretch_restore_rate_per_second
              << " 摩擦=" << sim_config_.ground_friction_per_second
              << " 子步=" << sim_config_.substeps
              << " 迭代=" << sim_config_.solver_iterations
              << " 开关(shape/stretch/ground)=" << sim_config_.enable_shape
              << "/" << sim_config_.enable_stretch << "/"
              << sim_config_.enable_ground;
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

inline void ArapViewerApp::AdvanceDynamics(float dt_seconds) {
    // 地面高度是界面量，每帧同步进物理参数（改变即立刻影响碰撞）。
    sim_config_.ground_y = ground_y_;
    sim_.SetConfig(sim_config_);
    sim_.Step(dt_seconds);

    // ── 发散保护（诚实兜底，不掩盖问题）──
    //   本仿真在部分真实资产上仍会发散（详见 docs/jpov_arap_viewer_design.md §12
    //   的"已知边界"）。与其让用户看着模型飞走/糊成一团却不知为何，不如**停住并
    //   明说**：一旦边长畸变超过 100%（几何已完全不是原形），自动暂停并告警。
    constexpr float kDivergeDistortion = 1.0f;
    if (!diverged_ && sim_.edge_distortion_rms() > kDivergeDistortion) {
        diverged_ = true;
        dynamics_running_ = false;
        LOG(WARNING) << "仿真发散（边长畸变 " << sim_.edge_distortion_rms()
                     << " > " << kDivergeDistortion
                     << "），已自动暂停；按「重置 mesh」恢复，或调整物理参数重试";
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
    }
    if (ui_.Button("重置 mesh",
                   jpov::UiRect{{16.0f + kBtnW + kBtnGap, btn_y}, {kBtnW, kBtnH}})) {
        sim_.Reset();
        diverged_ = false;
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

    // ── 底部 4 个半屏宽滑条（与 model viewer 同布局）──
    const float kSliderWidth = 0.5f * w;
    const float kBottom = 20.0f;
    const float left = (w - kSliderWidth) * 0.5f;
    const float top = h - kBottom - (4.0f * kPanelRowH + 3.0f * kPanelSpacing);

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

    // ── 状态文本：质点/顶点数 + 边长畸变（判断压扁程度与是否已回弹）──
    if (!prims_.empty()) {
        char status[256];
        std::snprintf(status, sizeof(status),
                      "网格 %zu / 顶点 %zu / 质点 %zu   边长畸变 %.3f  退化旋转 %zu",
                      sim_.mesh_count(), sim_.vertex_count(),
                      sim_.particle_count(),
                      static_cast<double>(sim_.edge_distortion_rms()),
                      sim_.degenerate_rotation_count());
        ui_.Text(status, jpov::UiRect{{16.0f, btn_y + kBtnH + 8.0f},
                                      {520.0f, 24.0f}},
                 /*stretch_w*/ false, /*stretch_h*/ false);
    }
}

}  // namespace jpov_arap_viewer

#endif  // JPOV_DEMO_ARAP_VIEWER_APP_H_
