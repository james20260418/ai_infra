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
//   - 2026-10-05：① 蒙皮修复「缝合开裂」——位置重合的重复顶点（导出器在 UV/材质缝合处拆开的）
//       先在权重图上焊接成组（生长/平滑前），缝合两侧权重逐位一致，消除撕裂。焊接阈值做成可配
//       「焊接容差(mm)」（面板输入框，0~10mm，默认 5，0 = 关闭）。② 蒙皮升级为**种子生长**：
//       距离身体 <=「种子半径 seed_eps(mm)」的顶点为种子（权重冻结为直接投影权重），离体顶点
//       由种子冻结的调和扩散生长补出；面板新增「种子半径(mm)」（默认 10）与「权重生长」勾选。
//   - 2026-10-06：焊接判据新增**相对局部边长**模式（面板勾选「焊接用相对局部边长」）——
//       与「绝对距离(mm)」并列：判据是「相距 < 比例 × 局部边长」，与模型缩放 / 局部密度
//       均无关，拉伸后无需重调（详见 weight_transfer.h 的 WeldMode）。
//   - 2026-10-07（本 PR）：**随机摆动测试**——用**真骨架蒙皮**驱动参考人体做程序化随机动作
//       （见 random_pose_driver.h；不导入外部动画 ⇒ **无需重定向**），蒙皮过的衣服绑到
//       **同一骨架实例**后逐帧跟随（零新增同步逻辑）。左下角面板（贴底左）：启用勾选 +
//       播放/暂停 + 「主轴」相位（播放时自动推进、暂停时可拖动查看任意时刻动作）+ 关节幅度
//       + **随机种子输入框**（0~65535，换种子 = 换一套随机动作）。
//       「衣服没蒙皮则不用动」：衣服未蒙皮时该动作不带动衣服（保持静止）。
//   - 2026-10-08：**关联距离 d 旋钮**——右上仿真面板新增「关联距离 d (m)」滑条
//       （0.005~1 m，默认 0.1）+ CLI `--bind_distance`。d 是仿真点的「缝合」半径（关联邻居
//       表 + 长边加密阈值）；此前它硬编码为 kDefaultBindDistance，无旋钮（姊妹 soft_mesh_viewer
//       早有该滑条）。拖它 → 从**启动几何**重建仿真点集合（几何回启动姿态、仿真暂停）。
//   - 2026-10-09：**补洞（推进法）面板**——底部中间新增「执行补洞」按钮 + 细分/平滑
//       勾选 + 「最大洞周长 (m)」滑条（+ CLI `--hole_fill`/`--hole_perimeter`）。补洞在**建仿真器
//       （关联邻居表）之前**跑（"关联前先补洞"），把网格破洞/裂缝补上（见 mesh_hole_fill.h）。
//   - 2026-10-09（本 PR）：**3D 画笔（选区）+ 中键纵向 pan**——顶部中间「3D 画笔」面板
//       （激活/结束按钮 + 「画笔半径 (px)」滑条 5~200，默认 100）。画笔**激活态**下才有「选区」
//       概念：鼠标周围画淡黄半透明圆表示尺寸，左键非 UI 区涂抹把靠近鼠标射线的衣服顶点选入
//       选区（淡黄 2px 方块标记）；ESC / 再点按钮结束并清空选区（见 brush_tool.h /
//       camera_projection.h）。中键纵向 drag pan 相机注视点（半屏 = 1m，见 VerticalPanDeltaY）。
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

#include "tools/jpov/assets/models/models_path.h"
#include "tools/jpov/clothing/brush_tool.h"
#include "tools/jpov/clothing/camera_projection.h"
#include "tools/jpov/clothing/clothing_axis_input.h"
#include "tools/jpov/clothing/clothing_init.h"
#include "tools/jpov/clothing/clothing_save.h"
#include "tools/jpov/clothing/clothing_transform.h"
#include "tools/jpov/clothing/mesh_hole_fill.h"
#include "tools/jpov/clothing/random_pose_driver.h"
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
using jpov_viewer::VerticalPanDeltaY;
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

// 相机垂直 fov（度）：渲染相机与画笔「射线/投影」必须共用同一值，否则顶点标记 /
// 画笔圆会与画面错位。
inline constexpr float kCameraFovDeg = 60.0f;

// UI 文本默认字体 = CJK（面板标签显中文）。
inline constexpr const char* kViewerFontAlias = jpov::kFontBuiltinCJK;

// 未显式指定 --body_reference_path 时的演示用人体 reference。
// 项目内自带的 Mixamo 男性人体资产（rest/T-pose），便于快速跑通。
inline constexpr const char* kDefaultBodyReferencePath = jpov::kDefaultCharacterGlb;

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

// ── 软布自动蒙皮（weight transfer + 种子生长）参数 ──
// 种子半径 seed_eps（mm）：衣物顶点到身体最近距离 <= 该值即视为「贴身」→ 生长锚点（种子），
// 其权重冻结为直接投影权重；> 该值 → 非种子，由生长从种子扩散补出。默认 10mm（1cm，照顾
// 不那么贴身的衣物）。面板输入框可调。
inline constexpr float kSkinSeedEpsMm = 10.0f;
// 每顶点最大影响骨数（与 MeshData 的 4 组 joint/weight 对齐）。
inline constexpr int kSkinMaxInfluences = 4;
// 生长迭代上限（内部弱收敛会提前停）；0 = 不生长（只保留逐顶点直接投影权重）。
inline constexpr int kSkinGrowthPasses = 300;
// 「焊接容差」面板默认值（mm）：与 weight_transfer.h 的默认焊接阈值单一来源（ = 5mm）。
// 位置相距 <= 该值的顶点视为「同一缝合点」（导出器在 UV/材质缝合处拆开的重复顶点），
// 在权重图上焊接成组后再生长/平滑，消除接缝开裂。0 = 关闭焊接。
inline constexpr float kSkinWeldToleranceMm = kWeldToleranceM * 1000.0f;
// 「相对局部边长焊接」面板默认比例（× 局部边长）：与 weight_transfer.h 单一来源。
// 与「绝对距离」并列的另一种焊接判据：与模型缩放 / 局部密度无关，拉伸后无需调参。
inline constexpr float kSkinWeldRatio = kDefaultLocalEdgeWeldRatio;

// ── 随机摆动测试（2026-10-07 Danis）：主轴相位 / 速度 / 幅度的默认与烘焙布局 ──
//
// 骨架注册表已加 ReleaseSkeleton（槽位复用，本 PR）⇒ 面板「随机种子」变更时可按新种子
// **重烘焙**并替换骨架（见 RebakeMotionPoses）。幅度仍是烘焙维度（19 档一次烘完，
// 切幅度无需重烘），按 kMotionAmplitudeStepDeg 量化。
// pose 下标布局：level * kMotionPhasesPerCycle + 相位序号。

// 一个主轴周期内均匀烘焙的相位帧数（足够采样 3 倍频正弦；运行时相邻两帧插值）。
inline constexpr int kMotionPhasesPerCycle = 60;
// 幅度档位步长（度）与上限（度）→ 档位数 = 上限/步长 + 1（0,5,…,90 共 19 档）。
inline constexpr float kMotionAmplitudeStepDeg = 5.0f;
inline constexpr float kMotionAmplitudeMaxDeg = 90.0f;
inline constexpr int kMotionAmplitudeLevels =
    static_cast<int>(kMotionAmplitudeMaxDeg / kMotionAmplitudeStepDeg) + 1;
// 默认随机种子（0~65535；面板输入框可改，改后按新种子重烘焙姿态集）。
inline constexpr int kDefaultMotionSeed = 0;
// 种子上界（含）：面板输入 clamp 到 [0, kMotionSeedMax]。
inline constexpr int kMotionSeedMax = 65535;
// 默认关节幅度（度，取 5 的倍数以对齐档位）。
inline constexpr float kDefaultMotionAmplitudeDeg = 30.0f;
// 播放推进速度（Hz = 每秒推进的主轴周期数）。固定值 —— 快慢不影响判断蒙皮效果，故不做滑条。
inline constexpr float kMotionPlaySpeedHz = 0.5f;

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

    // ── 相机中键纵向 pan：注视点世界 Y 偏移（米）。相机 position 与 target **同步**平移
    //    该量（朝向不变 = 纯平移）。半屏 pan = 1m（见 VerticalPanDeltaY）。──
    float cam_target_y_ = 0.0f;

    // ── 3D 画笔（选区）──
    //   brush_ 存画笔激活态 + 半径 + 选区（衣服各 primitive 的顶点 index）。
    //   panel_rects_ 每帧收集各面板的屏幕矩形，供「鼠标是否落在 UI 上」判定（画笔用）。
    BrushTool brush_;
    std::vector<jpov::UiRect> panel_rects_;

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
    bool skin_grow_ui_ = true;         // 「权重生长」勾选（默认开；关掉 = 只留直接投影权重）
    float seed_eps_mm_ = kSkinSeedEpsMm;  // 种子半径（mm）
    NumberField seed_eps_field_ = NumberField(kSkinSeedEpsMm);
    float weld_tolerance_mm_ = kSkinWeldToleranceMm;  // 缝合焊接容差（mm；0 = 关闭）
    NumberField weld_tolerance_field_ = NumberField(kSkinWeldToleranceMm);
    // 焊接判据模式：false = 绝对距离(mm)（默认，保持既有行为）；true = 相对局部边长(比例)。
    bool weld_relative_ui_ = false;
    float weld_ratio_ = kSkinWeldRatio;  // 相对模式比例（× 局部边长；0 = 关闭）
    NumberField weld_ratio_field_ = NumberField(kSkinWeldRatio);

    // ══════════════ 随机摆动测试（2026-10-07 Danis）══════════════
    //
    // 用**真骨架蒙皮**驱动参考人体做程序化随机动作（random_pose_driver.h）；蒙皮过的衣服绑到
    // **同一骨架实例**后逐帧跟随。衣服未蒙皮时该动作不带动衣服（保持静止）。
    bool motion_test_enabled_ = false;  // 勾选：启用（人体走 GPU 蒙皮渲染路径）
    bool motion_playing_ = false;       // 播放/暂停（播放时主轴相位自动推进，速度=固定常量）
    float motion_phase_ = 0.0f;         // 主轴相位 ∈ [0,1)（1 = 一个周期）
    float motion_amplitude_deg_ = kDefaultMotionAmplitudeDeg;  // 关节幅度（度；按 5° 量化）
    int motion_seed_ = kDefaultMotionSeed;  // 随机种子 [0, kMotionSeedMax]（变则重烘焙姿态集）
    NumberField motion_seed_field_ = NumberField(static_cast<float>(kDefaultMotionSeed));
    uint32_t body_skel_id_ = 0;         // 人体骨架句柄（RegisterSkeleton 产物；0 = 未注册）
    std::vector<jpov::SkeletonPose> body_motion_poses_;  // 预烘焙 pose（幅度档 × 相位）
    std::string motion_msg_;            // 面板状态提示（每帧刷新）

    // ══════════════ 软体仿真（Step 1）══════════════
    //
    // 每 clothes primitive 一个仿真器（多为单 primitive）。生命周期：
    //   - 场景就绪（GPU 上传）后按启动几何 Init（绑定姿态）；
    //   - 平移 / 旋转 / 缩放**即时作用于仿真器状态**（不重建、不中断，见 ApplyXxx）；
    //   - 「重置」= sim.Reset()（回绑定姿态）；「推进仿真」时逐帧 Step。
    //   ⇒ 仿真中也能点击变换，衣服会带着速度继续演化。
    std::vector<Simulator> sims_;

    // 仿真点的**启动几何**快照（每 primitive 一份）：关联距离 d 变更需重建仿真点集合，
    // 重建必须基于启动几何（而非 cloth_current_，后者会被仿真/变换改过——拿形变当绑定姿态）。
    std::vector<jpov::MeshData> sim_startup_mesh_;

    // ── 补洞（推进法）面板状态 ──
    // 补洞在「建仿真器（关联邻居表）之前」跑，把网格里的破洞/裂缝补上（见 mesh_hole_fill.h）。
    bool hole_refine_ui_ = true;        // 细分（按周围边长拆分补丁长边）
    bool hole_fair_ui_ = true;          // fairing（Laplacian 平滑新增点）
    float hole_max_perimeter_ = 0.2f;   // 只补周长 ≤ 该值（米）的洞；0 = 不限制
    std::string hole_fill_msg_;         // 面板状态回显

    bool sim_running_ = false;       // 是否推进仿真（暂停按钮的反相）

    // 动力学滑条镜像值（UI 写、每帧同步到仿真器）。
    // 关联距离 d（米）：决定仿真点集合（长边加密）与关联邻居表（= 仿真点"缝合"半径）。
    // 拖动它 → 与 sim.bind_distance() 不一致时重建仿真点集合（见 SyncSimParams）。
    float bind_distance_ui_ = Simulator::kDefaultBindDistance;
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

    // 供 headless / 脚本用：立即执行一键蒙皮（等价面板「一键蒙皮」按钮）。
    void RunAutoSkinNow() { RunAutoSkin(); }

    // 供 headless / 脚本用：立即执行补洞（等价面板「执行补洞」按钮）。
    void RunHoleFillNow() { RunHoleFill(); }

    // 供 headless / CLI 设置补洞参数（在 RunHoleFillNow 前设好）。
    void SetHoleFillParams(float max_hole_perimeter_m, bool refine, bool fair) {
        hole_max_perimeter_ = max_hole_perimeter_m;
        hole_refine_ui_ = refine;
        hole_fair_ui_ = fair;
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
        // 3D 画笔：选区容器对齐衣服 primitive 数（初始为空选区）。
        brush_.EnsurePrimitives(cloth_current_.size());

        // 建仿真器（绑定姿态 = 启动几何）。仿真器的人体排斥匹配器（Step 2）由
        // InitSimulators() 内部统一挂载（见 AttachBodyMatcherToSims；匹配器在 init_ 里，
        // 比 sims_ 活得久：init_ 声明在 sims_ 之前 ⇒ 后析构）。
        InitSimulators();

        // 仅当匹配器不可用时提示（人体排斥将无效果）。
        if (!init_.body_matcher().valid()) {
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
            LOG(WARNING) << "人体 reference 无骨架（skin），软布自动蒙皮 / 随机摆动测试"
                            "不可用: "
                         << body_path_;
        }
        // 随机摆动测试：预烘焙周期 pose 序列 + 注册骨架（有骨架时一次注册，见 renderer.h
        // 骨架注册表无释放接口）。
        if (has_body_skeleton_) {
            BakeMotionPoses();
            body_skel_id_ = RegisterSkeleton(body_skeleton_, body_motion_poses_);
            CHECK_NE(body_skel_id_, 0u) << "随机摆动测试：RegisterSkeleton 失败";
            LOG(INFO) << "随机摆动测试：已注册骨架 skeleton_id=" << body_skel_id_
                      << "，预烘焙 pose " << body_motion_poses_.size() << " 帧（"
                      << kMotionAmplitudeLevels << " 幅度档 × " << kMotionPhasesPerCycle
                      << " 相位）";
        }
        LOG(INFO) << "人体 reference: " << body_path_ << "（" << body_.size()
                  << " primitives）";
        LOG(INFO) << "衣服模型: " << cloth_path_ << "（" << cloth_.size()
                  << " primitives）";
        FitViewToScene();
    }

    // 相机注视点（叠加中键纵向 pan 的 Y 偏移）。渲染相机与画笔射线/投影都用它，保证一致。
    jpov::Vec3f CameraTarget() const {
        return jpov::Vec3f(0.0f, cam_target_y_, 0.0f);
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

        // 随机摆动测试：播放时主轴相位自动推进（按 1 周期取模；周期函数保证循环无缝）。
        if (motion_playing_ && MotionTestActive()) {
            motion_phase_ += kMotionPlaySpeedHz / kViewerFps;
            while (motion_phase_ >= 1.0f) {
                motion_phase_ -= 1.0f;
            }
            while (motion_phase_ < 0.0f) {
                motion_phase_ += 1.0f;
            }
        }

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

            // 中键纵向 pan：平移相机注视点（相机 position 与 target 同步跟随，朝向不变）。
            // 半屏 pan = 1m（见 jpov_viewer::VerticalPanDeltaY）。
            if (input.middle.IsDrag()) {
                cam_target_y_ += VerticalPanDeltaY(input.mouse_dy, winfo.height);
            }
        }

        // ── 滑块 / 参数同步到仿真器（每个 primitive 一份）。──
        SyncSimParams();

        // ── 仿真推进：勾选运行时逐帧 Step（headless 由 AdvanceSimulationSteps 驱动）。──
        if (sim_running_) {
            StepSimulationOnce();
        }

        // ── 相机：由 view_ 推导（叠加中键 pan 的注视点 Y 偏移；position 与 target 同步平移）。──
        const jpov::Vec3f cam_target = CameraTarget();
        cmds->camera.position = view_.Position() + cam_target;
        cmds->camera.target   = cam_target;
        cmds->camera.up       = {0.0f, 1.0f, 0.0f};
        cmds->camera.fov      = kCameraFovDeg;
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

        // 随机摆动测试生效时：人体 / 衣服共用**同一份**实例状态（摆放 + pose），故逐帧同步。
        const bool motion_active = MotionTestActive();
        const jpov::SkinnedInstanceState motion_inst =
            motion_active ? MotionInstanceState() : jpov::SkinnedInstanceState();

        // 人体 reference 固定画在原点（不参与衣服调节）。
        if (show_body_) {
            if (motion_active) {
                for (const jpov::GltfPrimitive& prim : body_.primitives) {
                    cmds->DrawMeshWithSkeleton(prim.mesh_id, body_skel_id_,
                                               prim.material, {motion_inst});
                }
            } else {
                cmds->DrawGltfObject(body_,
                                     /*center*/ {0.0f, 0.0f, 0.0f},
                                     /*up*/     {0.0f, 1.0f, 0.0f},
                                     /*front*/  {0.0f, 0.0f, 1.0f});
            }
        }
        // 衣服：静态绘制用恒等放置（顶点已含变换 / 仿真形变）；摆动测试 + 已蒙皮时用
        // **同一骨架 + 同一实例状态**画（⇒ 与人体逐帧同步跟随）。未蒙皮则不动（静态绘制）。
        if (show_cloth_) {
            if (motion_active && skinned_) {
                for (const jpov::GltfPrimitive& prim : cloth_.primitives) {
                    cmds->DrawMeshWithSkeleton(prim.mesh_id, body_skel_id_,
                                               prim.material, {motion_inst});
                }
            } else {
                cmds->DrawGltfObject(cloth_,
                                     /*center*/ {0.0f, 0.0f, 0.0f},
                                     /*up*/     {0.0f, 1.0f, 0.0f},
                                     /*front*/  {0.0f, 0.0f, 1.0f});
            }
        }

        // ── 面板（仅交互窗口；headless 是纯 3D 截图）──
        if (show_panel_) {
            DrawPanels(input, winfo, cmds);
            ui_.End();
            ui_.Emit(cmds);
            // 画笔在面板之后处理（此刻已知本帧面板矩形 + 画笔激活态），并把画笔圆 /
            // 顶点标记画在面板之上。
            ProcessBrush(input, winfo, cmds);
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
        // 启动几何快照：关联距离 d 变更时据此重建（见 ReinitSimulators）。
        // 场景首次就绪时 cloth_current_ == 启动几何，故此处拷贝即「启动姿态」。
        sim_startup_mesh_.resize(cloth_current_.size());
        for (size_t i = 0; i < sims_.size(); ++i) {
            sim_startup_mesh_[i] = cloth_current_[i].mesh;
            sims_[i].Init(cloth_current_[i].mesh, bind_distance_ui_);
            PushSimParams(&sims_[i]);
        }
        AttachBodyMatcherToSims();
        LOG(INFO) << "已建软体仿真器 × " << sims_.size() << "（绑定姿态 = 启动衣服几何，d="
                  << bind_distance_ui_ << " m）";
    }

    // 用当前 d 滑条值**重建**所有仿真器（从启动几何出发，丢弃仿真/变换带来的形变）。
    // 与 soft_mesh_viewer 的 ReinitSimulation 同语义：d 变了 ⇒ 仿真点集合与邻居表都变，
    // 只能重建。重建后回到启动姿态（缩放归 1、仿真暂停），再推 GPU。
    // Pre-condition: 已 InitSimulators；未蒙皮（蒙皮后几何冻结，禁止重建）。
    void ReinitSimulators() {
        CHECK_EQ(sims_.size(), sim_startup_mesh_.size());
        for (size_t i = 0; i < sims_.size(); ++i) {
            sims_[i].Init(sim_startup_mesh_[i], bind_distance_ui_);
            PushSimParams(&sims_[i]);
        }
        cloth_scale_ = 1.0f;
        sim_running_ = false;   // 重建回启动姿态，停机回到可重调状态
        SyncSimsToCloth();
        if (!sims_.empty()) {
            LOG(INFO) << "重建仿真点（d=" << bind_distance_ui_ << " m）：仿真点 原始 "
                      << sims_.front().original_point_count() << " + 虚拟 "
                      << sims_.front().virtual_point_count() << " = "
                      << sims_.front().sim_point_count();
        }
    }

    // 把人体「最近三角形」匹配器借给各仿真器（人体排斥用）。重建仿真器后也要重挂。
    // 匹配器在 init_ 里（比 sims_ 活得久）；未就绪时 no-op。
    void AttachBodyMatcherToSims() {
        if (!init_.body_matcher().valid()) {
            return;
        }
        for (Simulator& sim : sims_) {
            sim.SetBodyMatcher(&init_.body_matcher().matcher.value());
        }
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
        // 关联距离 d 不是逐点 setter：它决定仿真点集合与邻居表，变了必须重建。
        // 已蒙皮时几何冻结：忽略并回写滑条镜像值（保持面板与实际一致）。
        if (!sims_.empty() && bind_distance_ui_ != sims_.front().bind_distance()) {
            if (skinned_) {
                bind_distance_ui_ = sims_.front().bind_distance();
            } else {
                ReinitSimulators();
                return;  // 重建已同步了全部状态
            }
        }
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

    // ==================== 面板（绘制 / UI）====================
    // 代码量大（约占本文件 1/3），拆到 `clothing_tool_panels.inc`，用「部分类」写法在**类体内**
    // #include 展开（仍是同一个 ClothingToolApp，零接口改动）。见该文件头说明。
    #include "tools/jpov/clothing/clothing_tool_panels.inc"

    // 提交「随机种子」输入：解析 → clamp [0, kMotionSeedMax] →（变了则）重烘焙姿态集。
    // 文本框规范化为**最终生效值**（用户可见 clamp 后的结果）。
    void CommitMotionSeedField() {
        float parsed = 0.0f;
        int next = motion_seed_;
        if (ParseAxisValue(motion_seed_field_.text, &parsed)) {
            next = static_cast<int>(std::clamp(
                std::lround(parsed), 0L, static_cast<long>(kMotionSeedMax)));
        }
        snprintf(motion_seed_field_.text, kAxisInputCapacity, "%d", next);
        if (next != motion_seed_) {
            motion_seed_ = next;
            RebakeMotionPoses();
        }
    }

    // 按当前种子重烘焙姿态集并**替换**骨架（ReleaseSkeleton + RegisterSkeleton；骨架槽位复用，
    // 不会泄漏 GL 资源）。先释放旧骨架再注册（复用同一槽位，id 稳定），中间不做任何 Draw。
    // Pre-condition: has_body_skeleton_。
    void RebakeMotionPoses() {
        if (!has_body_skeleton_) {
            return;
        }
        BakeMotionPoses();
        ReleaseSkeleton(body_skel_id_);
        body_skel_id_ = RegisterSkeleton(body_skeleton_, body_motion_poses_);
        CHECK_NE(body_skel_id_, 0u) << "随机摆动测试：RegisterSkeleton 失败";
        LOG(INFO) << "随机摆动测试：种子=" << motion_seed_ << "，重烘焙 pose "
                  << body_motion_poses_.size() << " 帧（skeleton_id=" << body_skel_id_
                  << "）";
    }

    // 幅度按 kMotionAmplitudeStepDeg 量化为档位（与预烘焙 pose 集对齐）。
    static float SnapAmplitudeDeg(float deg) {
        const float level = std::round(deg / kMotionAmplitudeStepDeg);
        const float clamped =
            std::clamp(level, 0.0f, static_cast<float>(kMotionAmplitudeLevels - 1));
        return clamped * kMotionAmplitudeStepDeg;
    }

    // 随机摆动测试是否生效（启用 + 有已注册骨架）。衣服是否跟随由 skinned_ 另行判定。
    bool MotionTestActive() const {
        return motion_test_enabled_ && body_skel_id_ != 0;
    }

    // 当前幅度对应的烘焙档位（0..kMotionAmplitudeLevels-1）。
    int MotionAmplitudeLevel() const {
        const int level = static_cast<int>(
            std::lround(motion_amplitude_deg_ / kMotionAmplitudeStepDeg));
        return std::clamp(level, 0, kMotionAmplitudeLevels - 1);
    }

    // 本帧人体 / 衣服共享的实例状态：摆放在原点（与静态绘制一致），pose = 预烘焙序列里
    // 「当前幅度档 + 主轴相位」相邻两帧之间插值（末尾环绕到本档首帧，无缝）。
    jpov::SkinnedInstanceState MotionInstanceState() const {
        const int total = kMotionPhasesPerCycle;
        const int level = MotionAmplitudeLevel();
        float scaled = motion_phase_ * static_cast<float>(total);
        if (scaled < 0.0f) {
            scaled = 0.0f;
        }
        int a = static_cast<int>(scaled);
        float frac = scaled - static_cast<float>(a);
        if (a >= total) {
            a = 0;
            frac = 0.0f;
        }
        const int b = (a + 1) % total;
        jpov::SkinnedInstanceState inst;
        inst.pose_a = level * total + a;
        inst.pose_b = level * total + b;
        inst.ratio = frac;
        return inst;
    }

    // 预烘焙「一个主轴周期」的姿态序列（幅度档 × 相位），交给 RegisterSkeleton。
    // pose 下标 = level * kMotionPhasesPerCycle + 相位序号。
    // Pre-condition: has_body_skeleton_。
    void BakeMotionPoses() {
        CHECK(has_body_skeleton_) << "BakeMotionPoses: 人体无骨架";
        body_motion_poses_.clear();
        body_motion_poses_.reserve(
            static_cast<size_t>(kMotionAmplitudeLevels * kMotionPhasesPerCycle));
        for (int level = 0; level < kMotionAmplitudeLevels; ++level) {
            RandomMotionParams params;
            params.amplitude_deg = static_cast<float>(level) * kMotionAmplitudeStepDeg;
            params.seed = motion_seed_;
            for (int k = 0; k < kMotionPhasesPerCycle; ++k) {
                const float phase =
                    static_cast<float>(k) / static_cast<float>(kMotionPhasesPerCycle);
                body_motion_poses_.push_back(RandomPoseAt(body_skeleton_, phase, params));
            }
        }
    }

    // 画一个左对齐、垂直居中的标签（不拉伸：内容居中于给定宽度）。
    // 记录一块面板的屏幕矩形（画笔判定「鼠标是否落在 UI 上」用；每帧 DrawPanels 先 clear）。
    void RecordPanel(float x, float y, float w, float h) {
        panel_rects_.push_back(jpov::UiRect{{x, y}, {w, h}});
    }

    void DrawLabel(const char* text, float x, float width, float y) {
        ui_.Text(text, jpov::UiRect{{x, y}, {width, kPanelRowH}}, false, false);
    }

    // 焊接容差（mm）clamp：0（关闭）~ 上界（对应 kWeldToleranceMaxM）。
    static float ClampWeldToleranceMm(float mm) {
        return std::min(std::max(mm, 0.0f), kWeldToleranceMaxM * 1000.0f);
    }

    // 焊接比例（× 局部边长）clamp：0（关闭）~ kMaxLocalEdgeWeldRatio。
    static float ClampWeldRatio(float ratio) {
        return std::min(std::max(ratio, kMinLocalEdgeWeldRatio),
                        kMaxLocalEdgeWeldRatio);
    }

    // 种子半径（mm）clamp：[0.1, 200]（种子半径必须为正）。
    static float ClampSeedEpsMm(float mm) {
        return std::min(std::max(mm, 0.1f), 200.0f);
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

    // 补洞（推进法，见 mesh_hole_fill.h）：对每个衣物 primitive 跑补洞，把网格里的破洞 /
    // 裂缝补上。补洞在**建仿真器（关联邻居表）之前**做（"关联前先补洞"）：补完把结果当作
    // 新的启动姿态，重建仿真器 + 推 GPU（重算法线）。已蒙皮后几何冻结 → no-op。
    void RunHoleFill() {
        if (skinned_) {
            hole_fill_msg_ = "已蒙皮（几何已冻结），补洞被忽略";
            return;
        }
        if (cloth_current_.empty()) {
            hole_fill_msg_ = "衣服几何尚未就绪";
            return;
        }
        HoleFillOptions opts;
        opts.max_hole_perimeter = hole_max_perimeter_;
        opts.refine = hole_refine_ui_;
        opts.fair = hole_fair_ui_;
        size_t loops_total = 0;
        size_t loops_filled = 0;
        size_t tris_added = 0;
        size_t verts_added = 0;
        float max_perim = 0.0f;
        for (size_t i = 0; i < cloth_current_.size(); ++i) {
            MeshData filled;
            HoleFillStats st;
            if (FillMeshHoles(cloth_current_[i].mesh, opts, &filled, &st)) {
                cloth_current_[i].mesh = std::move(filled);
            }
            loops_total += st.loops_total;
            loops_filled += st.loops_filled;
            tris_added += st.triangles_added;
            verts_added += st.vertices_added;
            max_perim = std::max(max_perim, st.max_filled_perimeter);
        }
        // 几何变了 → 以新几何为启动姿态重建仿真器（关联前先补洞）+ 推 GPU（重算法线）。
        InitSimulators();
        SyncSimsToCloth();
        hole_fill_msg_ =
            Format("补洞：环 %zu 补 %zu，+三角 %zu，+顶点 %zu（最大环周长 %.3f m）",
                   loops_total, loops_filled, tris_added, verts_added,
                   static_cast<double>(max_perim));
        LOG(INFO) << hole_fill_msg_;
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

        const int passes = skin_grow_ui_ ? kSkinGrowthPasses : 0;
        const geom::TriangleMatcher3d<double>& matcher =
            init_.body_matcher().matcher.value();
        // 焊接判据：相对局部边长（比例）或绝对距离（mm）。
        const WeldSpec weld =
            weld_relative_ui_ ? WeldSpec::Relative(weld_ratio_)
                              : WeldSpec::Absolute(weld_tolerance_mm_ * 0.001f);
        size_t total_verts = 0;
        size_t total_seed = 0;
        size_t total_non_seed = 0;
        size_t total_weld = 0;
        size_t max_growth = 0;
        float max_body_dist_m = 0.0f;
        for (size_t i = 0; i < cloth_current_.size(); ++i) {
            const SkinTransferStats s = TransferSkinWeights(
                init_.body_skin(), matcher, &cloth_current_[i].mesh,
                seed_eps_mm_ * 0.001f, weld, kSkinMaxInfluences, passes);
            total_verts += s.vertex_count;
            total_seed += s.seed_vertex_count;
            total_non_seed += s.non_seed_vertex_count;
            total_weld += s.weld_merged_vertex_count;
            max_growth = std::max(max_growth, s.growth_passes);
            max_body_dist_m = std::max(max_body_dist_m, s.max_body_distance_m);
        }
        skinned_ = true;
        sim_running_ = false;
        // 衣服几何现在带上 JOINTS/WEIGHTS（kJoints）→ 换成**可蒙皮**的 GPU mesh：先释放原
        // 静态 mesh，再用带骨几何重注册。静止外观不变；摆动测试时按骨架蒙皮绘制。
        // （骨架注册表无释放接口，故这里只换 mesh，不重建骨架。）
        for (size_t i = 0; i < cloth_.size(); ++i) {
            ReleaseMesh(cloth_.primitives[i].mesh_id);
            cloth_.primitives[i].mesh_id = RegisterMesh(cloth_current_[i].mesh);
        }
        skin_msg_ = Format(
            "已蒙皮 %zu 顶点 / 种子 %zu（非种子 %zu, max %.1f mm）/ 焊接 %s %zu / 生长 %zu 轮",
            total_verts, total_seed, total_non_seed,
            static_cast<double>(max_body_dist_m * 1000.0f),
            weld_relative_ui_ ? "(相对局部边长)" : "(绝对距离)", total_weld,
            max_growth);
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
