// JPOV 穿衣工具 — 配置常量 + 面板布局 + 数值输入框状态（header-only）。
//
// 目的：把默认常量 / 面板布局常量 / 跨帧输入框状态集中一处，让 clothing_tool_app.h 聚焦
// 「场景与逻辑」。本文件被 clothing_tool_app.h include，也可被其它模块直接引用这些常量。
// 约定：默认值尽量与「单一来源」（weight_transfer / soft_mesh_simulator 等）对齐，不重复硬编码。

#ifndef JPOV_CLOTHING_CLOTHING_TOOL_CONFIG_H_
#define JPOV_CLOTHING_CLOTHING_TOOL_CONFIG_H_

#include <cstddef>
#include <cstdio>

#include "tools/jpov/assets/models/models_path.h"    // kDefaultCharacterGlb
#include "tools/jpov/clothing/weight_transfer.h"      // kWeldToleranceM / kDefaultLocalEdgeWeldRatio
#include "tools/jpov/interface/render_command.h"      // kFontBuiltinCJK

namespace jpov {
namespace clothing {

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


// ── 面板布局（各面板共用；配合 ClothingToolApp::BeginPanel 使用）──
inline constexpr float kPanelMargin  = 12.0f;   // 面板到窗口边缘的间距
inline constexpr float kPanelPad     = 10.0f;   // 面板内边距
inline constexpr float kPanelSpacing = 5.0f;    // 行间距

// 一块面板的布局值集合：BeginPanel 返回它（各面板把它照抄成局部 left/row_w/step_y/row_y）。
struct PanelFrame {
    float left   = 0.0f;   // 控件左缘 x
    float row_w  = 0.0f;   // 控件可用宽
    float step_y = 0.0f;   // 行高 + 行间距
    float row_y  = 0.0f;   // 首行 y
};

}  // namespace clothing
}  // namespace jpov

#endif  // JPOV_CLOTHING_CLOTHING_TOOL_CONFIG_H_
