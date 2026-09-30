// JPOV 穿衣工具 — 渲染核心 App（可视化框架）
//
// 与 jpov_soft_mesh_viewer 是**姊妹工具**（同款渲染核心骨架：场景静态资源 + 交互
// 面板宿主 + 唯一 OneIteration 渲染体），但职责聚焦：**把「一件衣服」摆到「一个人体
// reference」旁边显示出来**，为后续「衣服贴合人体」的穿衣管线提供可视化底座。
//
// 需求（2026-09-27 Danis 定稿；2026-09-29 / 2026-09-30 完善）：
//   - 框架负责：加载人体 reference（--body_reference_path）、加载衣服模型
//     （--cloth_path），并把二者显示在同一场景里。
//   - 2026-09-29：① **后台初始化**（建最近邻三角形匹配器，期间黑底白字进度页）；
//     ② 左上角面板用 x/y/z 填值输入框粗调衣服位置。
//   - 2026-09-30：③ 给衣服补齐一整套**变换调节**：
//       · 平移 x/y/z：绝对位置输入框 + 步长输入框 + 步进按钮（"<" ">"）；
//       · 旋转 x/y/z：步长输入框 + 步进按钮（度，绕 X/Y/Z 逆时针），默认 45°；
//       · 整体缩放：步进式（"+" 乘系数 / "−" 除系数），系数用输入框调，默认 1.1；
//       · "保存衣服 glb" 按钮：把**当前已变换好的几何**写成 glb。
//     ⭐ 关键：**一律直接修改衣服 mesh 的顶点数据来实现调节**（见 clothing_transform.h），
//     不靠 DrawGltfObject 的 center/up/front/scale 放置参数——旋转尤其如此。这样"编辑后
//     保存 / 后续仿真统一化"拿到的就是已经变换好的几何，不必再套一层放置变换。
//
// 仍不做（后续管线的事）：穿衣物理、衣服贴合。本文件只负责"加载 + 显示 + 调节 + 保存"。
//
// 与 soft_mesh_viewer 的关键差异：本工具的两个模型是**静态资产**，故直接用
// LoadGltf → GltfObject 画（cmds->DrawGltfObject）；衣服侧额外保留一份 CPU 几何
// （clothing_init 后台加载时顺带保留）用于烘焙变换与保存。
//
// 命名空间 jpov::clothing、文件夹 tools/jpov/clothing/ 均为**独立**的一整套，
// 与 soft_mesh_simulator 互不牵连（Danis 要求「单独文件夹和命名空间」）。

#ifndef JPOV_CLOTHING_CLOTHING_TOOL_APP_H_
#define JPOV_CLOTHING_CLOTHING_TOOL_APP_H_

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

#include <glog/logging.h>

#include "tools/jpov/clothing/clothing_axis_input.h"
#include "tools/jpov/clothing/clothing_init.h"
#include "tools/jpov/clothing/clothing_save.h"
#include "tools/jpov/clothing/clothing_transform.h"
#include "tools/jpov/demo/skylight_scene.h"
#include "tools/jpov/demo/view_config.h"
#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/interface/ui.h"
#include "tools/jpov/src/gltf_loader.h"

namespace jpov {
namespace clothing {

// 视角 / 光照 / 地面工具：视角与地面复用姊妹查看器的纯函数（ViewConfig、ApplyInput、
// MakeGroundQuad、GroundMaterial 在 jpov_viewer 命名空间）。
// 光照改用 **skylight viewer 的天光配置**（太阳仰角 45° + 三色环境光），而非旧版的
// MakeNoonLighting——Danis 2026-09-29 反馈原来 ambient 偏暗，要求参照 skylight viewer。
using jpov_viewer::ApplyInput;
using jpov_viewer::DefaultView;
using jpov_viewer::GroundMaterial;
using jpov_viewer::MakeGroundQuad;
using jpov_viewer::ViewConfig;

// 渲染/窗口分辨率（与姊妹查看器一致：1280×720，不可 resize）。单点定义。
inline constexpr int kViewerWidth  = 1280;
inline constexpr int kViewerHeight = 720;

// 交互帧率（查看器刷新率，Hz）。
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

// 一个数值输入框的跨帧状态：文本缓冲 + 上一帧聚焦态。
// 聚焦态用于检测"回车 / 焦点丧失"这一提交边界（InputText 返回的是"帧末是否聚焦"）。
struct NumberField {
    char text[kAxisInputCapacity];
    bool focused_prev = false;

    NumberField() { text[0] = '\0'; }
    explicit NumberField(const char* init) {
        snprintf(text, kAxisInputCapacity, "%s", init);
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

    // 地面高度（米）滑条值，[-3, +3]。
    float ground_y_ = -3.0f;

    // ── 衣服变换状态（平移 / 旋转 / 缩放）──
    // 所有调节都**烘进衣服 CPU 顶点**（见 clothing_transform.h / RebakeClothMesh），
    // 不靠 DrawGltfObject 的放置参数。默认全为恒等。
    ClothTransform transform_;

    // 各步长 / 系数（供步进按钮使用；用户可在面板里用输入框改，提交时 clamp）。
    float trans_step_[3] = {kDefaultTransStep, kDefaultTransStep, kDefaultTransStep};
    float rot_step_[3]   = {kDefaultRotStep, kDefaultRotStep, kDefaultRotStep};
    float scale_step_    = kDefaultScaleStep;

    // 面板数值输入框的跨帧文本 + 聚焦态。
    // 绝对位置默认 "0"；平移步长 "0.1"；旋转步长 "45"；缩放系数 "1.1"。
    NumberField pos_field_[3] = {NumberField("0"), NumberField("0"),
                                 NumberField("0")};
    NumberField trans_step_field_[3] = {NumberField("0.1"), NumberField("0.1"),
                                        NumberField("0.1")};
    NumberField rot_step_field_[3] = {NumberField("45"), NumberField("45"),
                                      NumberField("45")};
    NumberField scale_step_field_ = NumberField("1.1");

    // 衣服保存控制器（后台线程写 glb）。
    ClothingSaveController save_ctrl_;

    // 装配真实字体文本测量回调（UI 内部用），Init() 后调用一次。
    void InstallTextMeasure() {
        ui_.SetTextMeasure(&ClothingToolApp::ViewerTextWidth, this);
    }

    // 渲染/出图时是否绘制面板（交互窗口 = true；headless 纯 3D 截图 = false）。
    void SetShowPanel(bool show) { show_panel_ = show; }
    bool show_panel() const { return show_panel_; }

    // 后台初始化泵（每帧，在主/GL 线程调用；供 OneIteration 与 headless main 循环使用）。
    //
    // 三阶段：
    //   1) kBuilding：后台线程建图，屏幕上显示进度文案（不阻帧）。
    //   2) kDone 第一帧：先把进度页切成"正在上传 GPU 资源..."（本帧只画不做事，
    //      保证用户能看到文案，而不是被 LoadGltf 阻帧时停留在上一帧的旧文案）。
    //   3) 下一帧：真正做 GPU 上传（LoadGltf 必须走 GL，仅主线程；会阻帧几秒，
    //      但屏幕上已显示正确文案）+ 相机自适应。
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

    // 上传两份 GPU 资产（仅主线程/GL 上下文）+ 相机自适应 + 取回衣服 CPU 几何。
    // Pre-condition: init_.state() == kDone。
    void UploadGpuAssets() {
        CHECK_EQ(static_cast<int>(init_.state()), static_cast<int>(InitState::kDone));
        // GPU 上传（主线程）。失败会 LOG(FATAL)（与旧版一致，资产缺失是硬错误）。
        body_ = LoadGltf(body_path_);
        CHECK(!body_.empty()) << "人体 reference 加载失败或为空: " << body_path_;
        cloth_ = LoadGltf(cloth_path_);
        CHECK(!cloth_.empty()) << "衣服模型加载失败或为空: " << cloth_path_;

        // 取回后台顺带保留的衣服 CPU 几何（base 几何 + 材质），供烘焙 / 保存用。
        // 顺序与 LoadGltf 内部的 LoadGltfScene 一致（同一次遍历的同一顺序），故可下标对齐。
        cloth_geometry_ = init_.TakeClothGeometry();
        CHECK_EQ(cloth_geometry_.size(), cloth_.size())
            << "衣服 CPU 几何与 GPU primitive 数量不一致："
            << cloth_geometry_.size() << " vs " << cloth_.size();

        // 当前几何快照（保存用）：初值 = 未变换的 base（默认变换为恒等）。
        cloth_current_.resize(cloth_geometry_.size());
        for (size_t i = 0; i < cloth_geometry_.size(); ++i) {
            cloth_current_[i].mesh = cloth_geometry_[i].mesh;
            cloth_current_[i].material = cloth_geometry_[i].material;
        }

        gpu_uploaded_ = true;
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
        cmds->camera.fbo_3d_width_  = kViewerWidth;
        cmds->camera.fbo_3d_height_ = kViewerHeight;

        // 后台初始化泵：完成分两帧（先显"上传中"再真传，见 TickInit）。
        const bool scene_ready = TickInit();

        if (init_.state() == InitState::kFailed) {
            DrawInitFailedScreen(cmds);
            return;
        }
        if (!scene_ready) {
            DrawInitProgressScreen(cmds);
            return;
        }

        // 保存状态机泵（每帧）。
        save_ctrl_.Tick();

        // 衣服几何重建：上一帧面板若有调节（cloth_dirty_），在此烘进顶点并更新 GPU
        // （UpdateMesh 需 GL 上下文，故只能在主线程这段做）。1 帧延迟，符合即时模式惯例。
        if (cloth_dirty_) {
            RebakeClothMesh();
            cloth_dirty_ = false;
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
        // 衣服：变换已烘进顶点，故此处用**恒等放置**（center=0/up=+Y/front=+Z）。
        if (show_cloth_) {
            cmds->DrawGltfObject(cloth_,
                                 /*center*/ {0.0f, 0.0f, 0.0f},
                                 /*up*/     {0.0f, 1.0f, 0.0f},
                                 /*front*/  {0.0f, 0.0f, 1.0f});
        }

        // ── 面板（仅交互窗口；headless 是纯 3D 截图）──
        if (show_panel_) {
            DrawPanel(input, cmds);
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

    // ==================== 衣服变换烘焙 ====================

    // 把当前变换（平移/旋转/缩放）烘进衣服每个 primitive 的顶点，更新 GPU mesh，
    // 并刷新"当前几何快照"（保存用）。
    // Pre-condition: gpu_uploaded_ == true（cloth_geometry_/cloth_current_ 已就绪）。
    void RebakeClothMesh() {
        CHECK_EQ(cloth_geometry_.size(), cloth_.primitives.size());
        CHECK_EQ(cloth_current_.size(), cloth_.primitives.size());
        for (size_t i = 0; i < cloth_geometry_.size(); ++i) {
            jpov::MeshData baked =
                BakeClothMesh(cloth_geometry_[i].mesh, transform_);
            UpdateMesh(cloth_.primitives[i].mesh_id, baked);
            cloth_current_[i].mesh = std::move(baked);
        }
    }

    // 平移步进：offset[axis] += direction * trans_step_[axis]（direction = ±1），
    // 同步刷新"绝对位置"输入框文本。
    // Pre-condition: 0 <= axis < 3。
    void StepTranslation(int axis, float direction) {
        CHECK_GE(axis, 0);
        CHECK_LT(axis, 3);
        transform_.offset[axis] += direction * trans_step_[axis];
        snprintf(pos_field_[axis].text, kAxisInputCapacity, "%g",
                 static_cast<double>(transform_.offset[axis]));
        cloth_dirty_ = true;
    }

    // 旋转步进：rotation_deg[axis] += direction * rot_step_[axis]（度）。
    // Pre-condition: 0 <= axis < 3。
    void StepRotation(int axis, float direction) {
        CHECK_GE(axis, 0);
        CHECK_LT(axis, 3);
        transform_.rotation_deg[axis] += direction * rot_step_[axis];
        cloth_dirty_ = true;
    }

    // 缩放步进：factor > 1 放大、< 1 缩小。整体缩放夹到 [kClothScaleMin, kClothScaleMax]。
    // Pre-condition: factor > 0。
    void StepScale(float factor) {
        CHECK_GT(factor, 0.0f);
        transform_.scale = ClampClothScale(transform_.scale * factor);
        cloth_dirty_ = true;
    }

    // 提交一个数值输入框：解析文本 →（可选）clamp → 写回目标；非法输入不改目标。
    // 无论成功与否，都把文本框规范化为**最终生效值**（用户可见 clamp 后的结果）。
    // 返回 true = 目标值发生了变化。
    //
    // clamp_fn：把解析出的原始数值映射到合法范围（不需要范围时传恒等函数）。
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

    // ==================== 面板 ====================

    // 面板（左上角半透明黑底 + 自上而下的控件）：
    //   平移列表头 / X,Y,Z 行（绝对位置 + 步长 + "<" ">"）
    //   旋转列表头 / RX,RY,RZ 行（步长 + "<" ">"）
    //   缩放行（系数 + "−" "+" + 当前缩放只读）
    //   保存行（按钮 + 状态）
    //   地面高度滑条 / 显示开关 / 两行只读信息
    //
    // 布局用一个"行游标"自上而下堆叠（每行 row_h，行间 spacing）。
    void DrawPanel(const jpov::InputSnapshot& input,
                   jpov::RenderCommandList* cmds) {
        const float w = static_cast<float>(kViewerWidth);
        const float h = static_cast<float>(kViewerHeight);

        // 半透明黑底版（Danis 指定 0.5 半透明黑）。面板几何：左上角 + margin。
        const float kMargin  = 12.0f;
        const float kPad     = 10.0f;
        const float kRowH    = kPanelRowH;  // 单点定义（DrawLabel 也用同一常量）
        const float kSpacing = 5.0f;
        const float panel_w  = 0.36f * w;
        const float panel_x  = kMargin;
        const float panel_y  = kMargin;
        // 行数：平移表头1 + 平移3 + 旋转表头1 + 旋转3 + 缩放1 + 保存按钮1 + 保存状态1
        //        + 滑条1 + 开关1 + 只读2 = 15。
        constexpr int kRows = 15;
        const float panel_h = kPad * 2.0f + kRows * kRowH +
                              (kRows - 1) * kSpacing;
        const jpov::Color kPanelBg{0.0f, 0.0f, 0.0f, 0.5f};
        cmds->DrawRect(/*pos*/ {panel_x, panel_y},
                       /*size*/ {panel_w, panel_h}, kPanelBg);

        // Ui 不做面板平移：控件 UiRect 直接按屏幕像素定位（本工具只有一个面板，
        // 故把面板屏幕位置 + 内边距并进 left/top）。
        const float left = panel_x + kPad;
        const float top  = panel_y + kPad;
        const float row_w = panel_w - kPad * 2.0f;
        const float step_y = kRowH + kSpacing;

        jpov::UiTheme theme = jpov::UiTheme::Default(kFontSize);
        theme.font_alias = kViewerFontAlias;
        ui_.Begin(input, theme, w, h, 1000.0f / kViewerFps);

        // ---- 平移行的列布局 ----
        const float axis_w = 24.0f;      // "轴"列（X/Y/Z）
        const float hdr_step_w = 40.0f;  // 表头"步长"列宽 / 行内"步长"标签宽
        const float stepbox_w = 66.0f;   // 步长输入框宽
        const float btn_w = 26.0f;       // "<" / ">" / "−" / "+" 按钮宽
        const float gap = kSpacing;
        // 绝对位置框吃掉剩余宽度。
        const float pos_w = row_w - (axis_w + hdr_step_w + stepbox_w +
                                     2.0f * btn_w) - 5.0f * gap;
        const float col_axis   = left;
        const float col_pos    = col_axis + axis_w + gap;
        const float col_steplb = col_pos + pos_w + gap;
        const float col_stepbx = col_steplb + hdr_step_w + gap;
        const float col_btn_in = col_stepbx + stepbox_w + gap;   // "<"
        const float col_btn_in2 = col_btn_in + btn_w + gap;      // ">"

        float row_y = top;

        // ---- 平移列表头 ----
        DrawLabel("轴", col_axis, axis_w, row_y);
        DrawLabel("绝对位置", col_pos, pos_w, row_y);
        DrawLabel("步长", col_steplb, hdr_step_w, row_y);
        DrawLabel("步进", col_btn_in, col_btn_in2 + btn_w - col_btn_in, row_y);
        row_y += step_y;

        // ---- 平移 X / Y / Z 行 ----
        static const char* const kAxisTags[3] = {"X", "Y", "Z"};
        for (int axis = 0; axis < 3; ++axis) {
            DrawLabel(kAxisTags[axis], col_axis, axis_w, row_y);

            // 绝对位置（回车 / 焦点丧失提交；无 clamp）。
            const bool pos_focus = ui_.InputText(
                "", pos_field_[axis].text, kAxisInputCapacity,
                jpov::UiRect{{col_pos, row_y}, {pos_w, kRowH}});
            if (pos_field_[axis].focused_prev && !pos_focus) {
                if (CommitNumberField(&pos_field_[axis], NoClamp,
                                      &transform_.offset[axis])) {
                    cloth_dirty_ = true;
                }
            }
            pos_field_[axis].focused_prev = pos_focus;

            DrawLabel("步长", col_steplb, hdr_step_w, row_y);

            // 步长（回车 / 焦点丧失提交并 clamp；改的是"下一步的步长"，不影响当前几何）。
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

        // ---- 旋转列表头 ----
        DrawLabel("轴", col_axis, axis_w, row_y);
        DrawLabel("步长(°)", col_pos, stepbox_w, row_y);
        DrawLabel("步进", col_btn_in, col_btn_in2 + btn_w - col_btn_in, row_y);
        row_y += step_y;

        // ---- 旋转 RX / RY / RZ 行（只有步长 + 步进按钮，无绝对输入）----
        static const char* const kRotTags[3] = {"RX", "RY", "RZ"};
        for (int axis = 0; axis < 3; ++axis) {
            DrawLabel(kRotTags[axis], col_axis, axis_w, row_y);

            // 旋转步长（度）。
            const bool rot_focus = ui_.InputText(
                "", rot_step_field_[axis].text, kAxisInputCapacity,
                jpov::UiRect{{col_pos, row_y}, {stepbox_w, kRowH}});
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

        // ---- 缩放行：系数输入框 + "−" "+" + 当前缩放只读 ----
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
        DrawLabel(Format("x%.3f", static_cast<double>(transform_.scale)).c_str(),
                  scale_info_x, left + row_w - scale_info_x, row_y);
        row_y += step_y;

        // ---- 保存行：仅按钮 ----
        const float save_btn_w = 140.0f;
        const char* save_label =
            (save_ctrl_.state() == ClothSaveState::kSaving) ? "保存中..."
                                                            : "保存衣服 glb";
        if (ui_.Button(save_label,
                       jpov::UiRect{{left, row_y}, {save_btn_w, kRowH}})) {
            StartSaveCloth();
        }
        row_y += step_y;

        // ---- 保存状态行：单独一行 + 真左对齐 ----
        // 不用 Ui::Text（它总是把文字在 box 内居中；长文案会横向压到上一行的按钮上）。
        // 对齐交给渲染层（kTopLeft 以 pos 为左上角），与 editor_app.h 的做法一致。
        // 未保存过时 message() 为空 → 不画。
        const std::string& save_msg = save_ctrl_.message();
        if (!save_msg.empty()) {
            const jpov::Color kForeground{0.92f, 0.93f, 0.95f, 1.0f};  // 同 UiTheme 前景色
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

    // 画一个左对齐、垂直居中的标签（不拉伸：内容居中于给定宽度）。
    void DrawLabel(const char* text, float x, float width, float y) {
        ui_.Text(text, jpov::UiRect{{x, y}, {width, kPanelRowH}}, false, false);
    }

    // 发起保存：把当前衣服几何快照交给保存控制器（按值快照，之后改动不影响本次）。
    void StartSaveCloth() {
        if (cloth_current_.empty()) {
            LOG(WARNING) << "保存被忽略：衣服几何尚未就绪";
            return;
        }
        // 若同一帧内刚改过变换（尚未到期下一帧的重烘），先烘一次，保证存的是最新几何。
        if (cloth_dirty_) {
            RebakeClothMesh();
            cloth_dirty_ = false;
        }
        save_ctrl_.Start(cloth_current_, cloth_path_, "cloth");
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
        view_.R = ViewConfig::FitRadius(bounds.min, bounds.max, /*fov_deg*/ 60.0);
        LOG(INFO) << "场景包围盒 [" << bounds.min[0] << "," << bounds.min[1] << ","
                  << bounds.min[2] << "] ~ [" << bounds.max[0] << "," << bounds.max[1]
                  << "," << bounds.max[2] << "]，初始 R=" << view_.R;
    }

    // 后台初始化进度页：整屏黑底（不透明）+ 居中白字。
    void DrawInitProgressScreen(jpov::RenderCommandList* cmds) {
        const jpov::Color kBlack{0.0f, 0.0f, 0.0f, 1.0f};
        const jpov::Color kWhite{1.0f, 1.0f, 1.0f, 1.0f};
        cmds->DrawRect(/*pos*/ {0.0f, 0.0f},
                       /*size*/ {static_cast<float>(kViewerWidth),
                                 static_cast<float>(kViewerHeight)},
                       kBlack);
        std::string msg = init_.progress_message();
        if (init_.state() == InitState::kDone && !gpu_uploaded_) {
            msg = "正在上传 GPU 资源...";
        }
        cmds->DrawText(msg.empty() ? "正在初始化..." : msg,
                       /*pos*/ {kViewerWidth * 0.5f, kViewerHeight * 0.5f},
                       /*font_size*/ 24.0f, kWhite,
                       jpov::TextAlignment::kCenter, kViewerFontAlias);
    }

    // 后台初始化失败页：整屏黑底 + 居中白字（出错原因）。
    void DrawInitFailedScreen(jpov::RenderCommandList* cmds) {
        const jpov::Color kBlack{0.0f, 0.0f, 0.0f, 1.0f};
        const jpov::Color kWhite{1.0f, 1.0f, 1.0f, 1.0f};
        cmds->DrawRect(/*pos*/ {0.0f, 0.0f},
                       /*size*/ {static_cast<float>(kViewerWidth),
                                 static_cast<float>(kViewerHeight)},
                       kBlack);
        const std::string msg = init_.error_message();
        cmds->DrawText(msg.empty() ? "初始化失败" : ("初始化失败：" + msg),
                       /*pos*/ {kViewerWidth * 0.5f, kViewerHeight * 0.5f},
                       /*font_size*/ 20.0f, kWhite,
                       jpov::TextAlignment::kCenter, kViewerFontAlias);
    }

    // 取路径的文件名部分（去目录），用于面板只读行避免长路径溢出。
    static std::string BaseNameOf(const std::string& path) {
        const size_t slash = path.find_last_of("/\\");
        return (slash == std::string::npos) ? path : path.substr(slash + 1);
    }

    // 恒等 clamp（绝对位置不做范围限制）。
    static float NoClamp(float v) { return v; }

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
    bool cloth_dirty_ = false;           // 上一帧面板是否改过衣服变换（待重烘焙）

    // 衣服的 base CPU 几何（每个 primitive 的 MeshData + 材质）——烘焙的输入。
    std::vector<jpov::GltfMeshEntry> cloth_geometry_;
    // 衣服的当前几何快照（= 最新烘焙结果 + 材质）——保存的输入。
    std::vector<jpov::GltfSaveMesh> cloth_current_;

    jpov::Ui ui_;                        // 跨帧持有（滑条拖动态 / 聚焦态等内部记忆）

    static constexpr float kFontSize = 16.0f;
    static constexpr float kPanelRowH = 26.0f;  // 面板每行高度（DrawPanel / DrawLabel 共用）
};

}  // namespace clothing
}  // namespace jpov

#endif  // JPOV_CLOTHING_CLOTHING_TOOL_APP_H_
