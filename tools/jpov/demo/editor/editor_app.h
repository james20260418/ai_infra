// JPOV 模型编辑器 — 渲染核心 App（EditorApp）
//
// 与 viewer_app.h 的 ViewerApp 是**姊妹**关系：同一套场景（sunny day 光照 +
// 可调地面 + 相机右键环绕），但底部面板换成“模型放置”控件
// （缩放 / 平移 XYZ / 地面高度），并额外消费【左键横向 drag】做旋转。
//
// 为什么另起一个 App 而不是给 ViewerApp 加开关（SOUL：不要 demo 分叉温床）：
//   - 两个工具的手感与面板语义本就不同（viewer 调"光照标定"，editor 调"模型
//     摆位"），塞进一个类的代价是面板布局 + 输入消费全绕 show_editor_ 分支；
//   - 渲染体 OneIteration 的关键部分（相机推导 / 光照推导 / 地面重建）本就可
//     按段落复制——真正必须共享的是【放置数学】（model_placement.h，两边共用）
//     和【场景构造】（view_config.h，两边共用），而非 App 外壳。
// 判据（Danis 定调）："一片代码不能单测，就不要为共享而共享"——放置数学已抽到
// model_placement.h 并被单测覆盖，本类只留"接线 + 布局"。
//
// 与查看器一致的既有约定（勿改）：
//   - 右键 drag 转视角、滚轮 zoom（ApplyInput，需求要求"依然服从相机右键拖动"）；
//   - 光照固定 sunny day 同款配置（MakeLighting，滑条不暴露光照）；
//   - 地面 0 ~ -3 米可调（MakeGroundQuad，需求"地面依然是0~-3米可调"）；
//   - 交互/出图共用同一条 OneIteration（zero 分叉）。

// ⚠️ 关于布局的两个硬约束（改布局时必读）：
//   1. 滑条 box 与由 PanelRow() 算出，且 PointInPanel 用同一份几何判定左键归属；
//      两处必须同源，否则“能画的滑条拖不动”或“能拖的区域没有控件”。
//   2. 左上角说明文字区也计入面板（见 PointInPanel），避免它成为旋转的
//      隐形触发区。文字区不得与底部滑条区重叠（有测试守卫）。

#ifndef JPOV_DEMO_EDITOR_APP_H_
#define JPOV_DEMO_EDITOR_APP_H_

#include <string>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/demo/editor/model_placement.h"
#include "tools/jpov/demo/view_config.h"
#include "tools/jpov/interface/ui.h"

namespace jpov_viewer {

// 前向声明：本测试类需要访问 EditorApp 的私有归属判定/旋转消费逻辑（白盒回归）。
// 仅用于单测，不属公共 API（与 interface/ui.h 的 UiS0Test friend 同模式）。
class EditorDragOwnershipTest;

// 编辑器**默认**窗口/初始渲染尺寸（与查看器同规格）。
// ⚠️ 仅供参考"开窗多大"：**不是**布局常量。2D 面板/说明的坐标空间是每帧的
//   渲染分辨率（= 当帧窗口尺寸，见 OneIteration 的 winfo），窗口一变布局就跟
//    着贴到新的左下/左上角。切勿用这两个常量去算布局（那会让窗口放大后面板飘走）。
inline constexpr int kEditorDefaultWidth  = 1280;
inline constexpr int kEditorDefaultHeight = 720;

// 交互帧率（同查看器）。
inline constexpr float kEditorFps = 60.0f;

// UI 文本字体别名 = CJK（标签显中文；拉丁字母由渲染层自动回退）。
// 与 viewer_app.h 的 kViewerFontAlias 同值，但**刻意不引用它**——editor 与
// viewer 是两个独立工具，不应因对方改名而连带编译失败（同值不同名，各自单点定义）。
inline constexpr const char* kEditorFontAlias = jpov::kFontBuiltinCJK;

// 编辑器光照固定为 sunny day 同款：太阳仰角 90°（天顶正午）、浊度 2、中性色温。
// 需求原文"光照固定为sunny day 同款配置"——不暴露滑条，常量集中于此，改动只有一处。
inline constexpr float kEditorSunElevDeg = 90.0f;
inline constexpr float kEditorTurbidity  = 2.0f;
inline constexpr float kEditorSeasonR    = 1.0f;

// 地面高度滑条范围（需求："地面依然是0~-3米可调"）。
inline constexpr float kEditorGroundMin = -3.0f;
inline constexpr float kEditorGroundMax = 0.0f;
inline constexpr float kEditorGroundDefault = -3.0f;

class EditorApp : public JPOV {
public:
    using JPOV::JPOV;

    // ---- 面板布局几何（唯一真相：绘制与 PointInPanel 共用）----
    //
    // 🔑 全部从【本帧窗口尺寸 (fw, fh)】推算，不用编译期常量：
    //   2D 指令的坐标空间就是本帧渲染分辨率（= 窗口尺寸），所以窗口一变，
    //   滑条必须跟着贴到新尺寸的左下角。早期版本把宽高写死为 1280×720，
    //   导致窗口放大后滑条飘在中间偏上。
    //
    // 尺寸（滑条宽/行高）固定，仅位置随窗口平移 —— 满足"尺寸可固定，位置
    // 整体贴左下角"。
    struct PanelLayout {
        float w;          // 本帧渲染分辨率宽（= fbo/窗口宽）
        float h;          // 同上高
        float slider_w;   // 滑条宽（半屏，固定比例，不随窗口绝对值变）
        float help_left;  // 说明文字左缘
        float help_top;   // 说明文字上缘

        // 第 i 行（0 起）的 box：左缘贴左边距，底部向上堆叠。
        jpov::UiRect Row(int i) const {
            const float block = kPanelRows * kRowH +
                                (kPanelRows - 1) * kSpacing;
            const float top = h - kBottom - block;   // 首行上缘（贴底）
            return jpov::UiRect{
                {kMarginLeft, top + i * (kRowH + kSpacing)},
                {slider_w, kRowH}};
        }
        // 说明文字区（左上角）。
        jpov::UiRect Help() const {
            return jpov::UiRect{{help_left, help_top},
                                {kHelpLines * kHelpW, kHelpLines * kHelpLineH}};
        }

        // ---- 布局常量（尺寸固定部分）----
        static constexpr float kRowH       = 30.0f;   // 滑条行高
        static constexpr float kSpacing    = 10.0f;   // 行间距
        static constexpr float kBottom     = 18.0f;   // 到屏底留白
        static constexpr float kMarginLeft = 24.0f;   // 到屏左留白
        static constexpr int   kPanelRows  = 5;       // 缩放1+平移3+地面1
        static constexpr float kHelpLines  = 4.0f;    // 说明行数（与文案数组同长）
        static constexpr float kHelpLineH  = 24.0f;   // 单行行高（行距放宽，防拥挤）
        static constexpr float kHelpW      = 640.0f;  // 说明区宽（单行内容宽上限）
    };

    // 单测 friend（白盒回归，见 editor_drag_ownership_test.cc）。
    // 本类不暴露几何/输入的公共 API，测试通过 friend 直接验私有逻辑。
    friend class EditorDragOwnershipTest;

    // 单测访问器（仅供 friend 测试调用；ForTest 后缀明确用途，不属公共 API）。
    static PanelLayout MakeLayoutForTest(float fw, float fh) {
        return MakeLayout(fw, fh);
    }
    bool PointInPanelForTest(float x, float y, float fw, float fh) const {
        return PointInPanel(x, y, fw, fh);
    }
    void UpdateRotateFromDragForTest(const jpov::InputSnapshot& in, float fw,
                                     float fh) {
        UpdateRotateFromDrag(in, fw, fh);
    }

    // ── 场景状态（main 在 Init() 后一次性装配）──
    jpov::GltfObject gltf_;          // 被编辑的模型
    uint32_t ground_mesh_ = 0;       // 300×300 地面 quad 的 GPU handle
    jpov::PBRMaterial ground_mat_;   // 高粗糙灰色地面材质

    // ── 相机（右键环绕 + 滚轮 zoom；与查看器同一套 ViewConfig）──
    ViewConfig view_;

    // ── 模型放置状态（缩放/平移/旋转；唯一真相，见 model_placement.h）──
    // 朝向状态在 placement_.up / placement_.front 两个矢量里（不是角度）。
    ModelPlacement placement_;

    // ── 地面高度（[-3,0]，需求定档）──
    float ground_y_ = kEditorGroundDefault;

    // 左键旋转 drag 的按键边沿检测（跨帧）。
    //   rotate_mode_latched_ — 冻结的轴：true=绕 Y，false=绕 X
    //   rotate_drag_prev_    — 上一帧左键是否已处于（被本组件接管的）drag 态
    //   rotate_active_       — 本次左键按住期间，drag 是否由【3D 视口】发起
    //                          （false = 发起在面板上 → 交给 Ui 滑条，本组件不插手）
    //   left_down_prev_      — 上一帧左键是否已按下（Drag|Hold），用于取"按下上升沿"
    bool rotate_mode_latched_ = false;
    bool rotate_drag_prev_ = false;
    bool rotate_active_ = false;
    bool left_down_prev_ = false;

    void InstallTextMeasure() {
        ui_.SetTextMeasure(&EditorApp::EditorTextWidth, this);
    }
    void SetShowPanel(bool show) { show_panel_ = show; }
    bool show_panel() const { return show_panel_; }

    // ⭐ 唯一渲染体（交互与 headless 出图共用）。拍摄模式不改本函数：
    // 只要在帧间改 view_ / placement_，然后 RunOnce 出图即可。
    void OneIteration(int64_t frame_count, const jpov::InputSnapshot& input,
                      const jpov::WindowInfo& winfo,
                      jpov::RenderCommandList* cmds) override {
        (void)frame_count;

        // 渲染分辨率 = 本帧窗口尺寸（winfo 每帧由窗口层传入，随窗口大小变）。
        // 2D 指令（面板/文字）的坐标空间就是这个分辨率（见 render_command.h
        // 对 Rect2D/Polyline2D 的说明：坐标空间同 fbo_3d_width_/height_）。
        // 因此布局必须从【本帧 w/h】推算，不能用常量 —— 否则窗口一变面板就飘。
        CHECK_GT(winfo.width, 0.0f);
        CHECK_GT(winfo.height, 0.0f);
        const float fw = winfo.width;
        const float fh = winfo.height;
        cmds->camera.fbo_3d_width_  = static_cast<int>(fw);
        cmds->camera.fbo_3d_height_ = static_cast<int>(fh);

        if (show_panel_) {  // 仅可见窗口消费输入；headless 相机/放置由外部指定
            // 1) 右键 drag + 滚轮 → 相机（与查看器同款）。
            float dx = 0.0f, dy = 0.0f, scroll = 0.0f;
            if (input.right.IsDrag()) {
                dx = input.mouse_dx;
                dy = input.mouse_dy;
            }
            if (input.scroll_delta != 0.0f) scroll = input.scroll_delta;
            ApplyInput(&view_, dx, dy, scroll,
                       static_cast<int>(winfo.width),
                       static_cast<int>(winfo.height));

            // 2) 左键横向 drag → 模型旋转（纵向分量丢弃；Ctrl 选轴，按下时冻结）。
            UpdateRotateFromDrag(input, fw, fh);
        }

        // ── 相机：由 view_ 推导（目标点恒为原点，与查看器一致）──
        cmds->camera.position = view_.Position();
        cmds->camera.target   = ViewConfig::Target();
        cmds->camera.up       = {0.0f, 1.0f, 0.0f};
        cmds->camera.fov      = 60.0f;
        cmds->camera.near     = 0.05f;
        cmds->camera.far      = 1000.0f;

        // ── 光照：固定 sunny day（不暴露滑条）──
        const NoonLighting light =
            MakeLighting(kEditorSunElevDeg, kEditorTurbidity, kEditorSeasonR);
        cmds->sky = light.sky;
        cmds->sun = light.sun;
        cmds->ambient = light.ambient;
        cmds->tone_mapping = true;

        // ── 场景：地面 + 被编辑的 glTF ──
        if (ground_y_ != ground_y_prev_) {
            UpdateMesh(ground_mesh_, MakeGroundQuad(ground_y_));
            ground_y_prev_ = ground_y_;
        }
        cmds->DrawObject3D(ground_mesh_, ground_mat_,
                           /*center*/ {0.0f, 0.0f, 0.0f},
                           /*up*/     {0.0f, 1.0f, 0.0f},
                           /*front*/  {0.0f, 0.0f, 1.0f},
                           /*scale*/  1.0f);

        // 模型：放置状态 → (center,up,front,scale) → 原样喂给既有绘制指令。
        // 本 PR 的**全部**编辑能力都落在这四行——后端渲染器零改动。
        const DrawPlacement dp = ToDrawParams(placement_);
        cmds->DrawGltfObject(gltf_, dp.center, dp.up, dp.front, dp.scale,
                             /*highlight*/ false, /*picking_id*/ 0);

        if (show_panel_) {
            DrawPlacementPanel(input, fw, fh, cmds);
            ui_.End();
            ui_.Emit(cmds);
        }
    }

private:
    // 文本测量接线（与查看器同款；UI 内部布局/居中不依赖，但保持一致性）。
    static float EditorTextWidth(const char* text, float font_size,
                                 const char* /*font_alias*/, void* userdata) {
        EditorApp* app = static_cast<EditorApp*>(userdata);
        return app->MeasureTextWidth(/*alias=*/std::string(),
                                     /*text=*/text ? text : "", font_size);
    }

    // 左键横向 drag → 模型旋转。细节：
    //   - 只在左键处于 Drag 态时消费；不用 click/hold（"横向拖动"语义）；
    //   - 🔴 **只在 3D 视口发起的 drag 才旋转**：若左键是在底部面板（滑条等）上
    //     按下的，本次按住期间一律不旋转 —— 否则拖"平移 X"滑条时同一个
    //     mouse_dx 会被旋转逻辑再吃一次，表现为"拖 X 平移，RX 跟着动"。
    //     判定用【按下位置】：按下那一刻鼠标落在面板矩形内 → 本次 drag 归面板；
    //     否则归 3D 视口。drag 全程沿用该判定（中途拖出/拖入面板不改归属，
    //     与 Ui 滑条"drag 一旦开始判定区不作数"的语义对称）。
    //   - ⚠️ 归属必须在【左键按下的那一帧】就记下，不能等 IsDrag() 变真才判：
    //     按下后先 Hold（原地不动）再移动时，IsDrag 转真的那一帧鼠标已经在别处，
    //     用那时坐标定归属会把"起于面板"误判成"起于视口"。故下面用 left_down
    //     （Drag|Hold）的上升沿做起点，那时坐标还是按下点。
    //   - Ctrl 按下时绕世界 Y，否则绕世界 X；轴与归属都在 drag 起点冻结
    //     （中途切换 Ctrl 不换轴，避免手心打滑）；
    //   - 纵向分量丢弃（不参与任何计算）。
    void UpdateRotateFromDrag(const jpov::InputSnapshot& input, float fw,
                              float fh) {
        // 起点 = 左键按下的上升沿（Drag 或 Hold 都算，那时坐标=按下点）。
        const bool left_down = input.left.IsDrag() || input.left.IsHold();
        if (left_down && !left_down_prev_) {
            // 按下起点：先记归属（在面板上？），再按此刻 Ctrl 状态冻结旋转轴。
            rotate_active_ = !PointInPanel(input.mouse_x, input.mouse_y, fw, fh);
            rotate_mode_latched_ =
                input.GetKey(jpov::KeyCode::LeftCtrl).IsHold() ||
                input.GetKey(jpov::KeyCode::RightCtrl).IsHold();
        }
        if (!left_down) {
            left_down_prev_ = false;
            rotate_drag_prev_ = false;
            rotate_active_ = false;
            return;
        }
        left_down_prev_ = true;
        // 只有真正处于 Drag 态才写值（Hold = 按着没动 → 不旋转，符合"拖动"语义）。
        const bool dragging = input.left.IsDrag();
        if (!dragging) {
            rotate_drag_prev_ = false;
            return;
        }
        rotate_drag_prev_ = true;
        if (!rotate_active_) return;   // 本次按住始于面板 → 不旋转（交给 Ui 滑条）
        ApplyRotateDrag(&placement_, input.mouse_dx, rotate_mode_latched_,
                        static_cast<int>(fw));
    }

    // 由本帧窗口尺寸构造布局。
    static PanelLayout MakeLayout(float fw, float fh) {
        PanelLayout L;
        L.w = fw;
        L.h = fh;
        L.slider_w = 0.5f * fw;      // 半屏宽（需求：宽度不变 → 仍是半屏比例）
        L.help_left = 20.0f;         // 左上角说明左缘
        L.help_top = 16.0f;          // 左上角说明上缘
        return L;
    }

    // 点 (x,y)（窗口像素坐标）是否落在面板/说明文字上。
    // 布局由 MakeLayout 唯一决定，避免"判定用的矩形"与"实际画的矩形"分叉。
    //
    // 除底部滑条区外，**左上角的操作说明文字也计入面板**：它是纯文本无交互，
    // 在它上面按下左键也不应触发模型旋转（用户可能想拖它当“空白区”，但更
    // 重要的是不要让文字区域成为旋转的隐形触发区——保持"无控件处才算视口"
    // 的一致心智）。
    bool PointInPanel(float x, float y, float fw, float fh) const {
        const PanelLayout L = MakeLayout(fw, fh);
        // ① 底部滑条区：横向放宽一个行高（句柄/文本可能溢出 box 少许），
        //    纵向从首行上缘（上留一个行高余量）到屏幕底。
        const jpov::UiRect r0 = L.Row(0);
        const jpov::UiRect rN = L.Row(PanelLayout::kPanelRows - 1);
        const bool in_sliders =
            x >= r0.pos.x() - PanelLayout::kRowH &&
            x <= r0.pos.x() + r0.size.x() + PanelLayout::kRowH &&
            y >= r0.pos.y() - PanelLayout::kRowH && y <= fh;
        (void)rN;
        // ② 左上角说明文字区（见 PanelLayout::Help）。
        const jpov::UiRect help = L.Help();
        const bool in_help = x >= help.pos.x() &&
                             x <= help.pos.x() + help.size.x() &&
                             y >= help.pos.y() &&
                             y <= help.pos.y() + help.size.y();
        return in_sliders || in_help;
    }

    // 模型放置面板：缩放 / 平移 XYZ / 地面高度。
    // 全部滑条数值由 placement_ / ground_y_ 外置持有。
    //
    // 🔑 布局由 MakeLayout(fw,fh) 唯一决定，使"画控件用的 box"与
    // "判定左键归属用的面板矩形"（PointInPanel）**共用同一份几何**——
    // 两处各写一份是这类"拖滑条误触旋转"bug 的经典温床。
    void DrawPlacementPanel(const jpov::InputSnapshot& input, float fw, float fh,
                            jpov::RenderCommandList* cmds) {
        const PanelLayout L = MakeLayout(fw, fh);
        jpov::UiTheme theme = jpov::UiTheme::Default(kEditorFontSize);
        theme.font_alias = kEditorFontAlias;
        ui_.Begin(input, theme, fw, fh, 1000.0f / kEditorFps);

        // 行盒辅助（布局几何见 PanelLayout::Row）。
        auto row = [&](int i) { return L.Row(i); };

        // ── 缩放 [0.1,10]，默认 1.0 ──
        ui_.SliderFloat("缩放", &placement_.scale, row(0),
                        ModelPlacement::kScaleMin, ModelPlacement::kScaleMax,
                        /*decimal_places*/2);

        // ── 平移 XYZ 各一条，范围 ±3 ──
        ui_.SliderFloat("平移 X", &placement_.tx, row(1),
                        ModelPlacement::kTransMin, ModelPlacement::kTransMax, 2);
        ui_.SliderFloat("平移 Y", &placement_.ty, row(2),
                        ModelPlacement::kTransMin, ModelPlacement::kTransMax, 2);
        ui_.SliderFloat("平移 Z", &placement_.tz, row(3),
                        ModelPlacement::kTransMin, ModelPlacement::kTransMax, 2);

        // 注：**旋转没有滑条**。朝向的状态量是 (up, front) 两个矢量，不是角度
        // （见 model_placement.h 顶部：两个 float 角度自由度不够，无法表达
        //  "沿世界 Y 转完再沿世界 X 转" 的任意累积）。左键横向拖动即旋转。

        // ── 地面高度 [-3, 0] ──
        ui_.SliderFloat("地面高度 y", &ground_y_, row(4),
                        kEditorGroundMin, kEditorGroundMax, 2);

        // ── 左上角说明文字（需求：黑色文字 + 真左对齐；不走 Ui 控件）──
        // 注意：该函数直接向 cmds 写 DrawText，而非经 ui_.Text —— 因为
        // Ui::Text 写死居中 + 写死 theme.foreground，无法满足"左对齐 + 黑字"。
        DrawHelpText(cmds, L);
    }

    // 左上角操作说明（纯展示，不参与交互）。
    //
    // 直接用 RenderCommandList::DrawText + kTopLeft 对齐输出：
    //   - **真左对齐**：对齐方式交给渲染层（kTopLeft 以 pos 为左上角），
    //     不再靠"估算文本宽 + Ui::Text 居中"凑左对齐（估宽不准就会参差不齐，
    //     正是上一版排版看着怪的原因）。
    //   - **黑色文字**：不用 UiThreshold 的 theme_.foreground（Ui::Text 写死用
    //     它，无法控色），这里显式给黑。（Danis 定：黑字不加底版。）
    void DrawHelpText(jpov::RenderCommandList* cmds, const PanelLayout& L) {
        // 文案（需求：工具左上角打印左键使用说明）。改行数须同步 kHelpLines。
        static const char* const kText[] = {
            "左键横向拖动 = 模型绕世界 X 轴旋转（右滑逆时针）",
            "Ctrl + 左键横向拖动 = 模型绕世界 Y 轴旋转（右滑逆时针）",
            "右键拖动 = 旋转视角 · 滚轮 = 缩放视角",
            "底下滑条 = 缩放 / 平移 / 地面高度",
        };
        static_assert(sizeof(kText) / sizeof(kText[0]) ==
                          static_cast<int>(PanelLayout::kHelpLines),
                      "说明文字行数与 kHelpLines 不一致（改文案须同步）");
        const jpov::Color kBlack{0.0f, 0.0f, 0.0f, 1.0f};
        for (int i = 0; i < static_cast<int>(PanelLayout::kHelpLines); ++i) {
            cmds->DrawText(kText[i],
                           {L.help_left,
                            L.help_top + i * PanelLayout::kHelpLineH},
                           kEditorFontSize, kBlack,
                           jpov::TextAlignment::kTopLeft,
                           kEditorFontAlias);
        }
    }

    bool show_panel_ = true;
    float ground_y_prev_ = kEditorGroundDefault;
    jpov::Ui ui_;

    static constexpr float kEditorFontSize = 16.0f;
};

}  // namespace jpov_viewer

#endif  // JPOV_DEMO_EDITOR_APP_H_
