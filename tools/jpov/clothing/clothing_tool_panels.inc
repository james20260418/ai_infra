// JPOV 穿衣工具 — 面板 / UI 绘制方法。
//
// ⚠️ 本文件**不是**独立头文件：它由 clothing_tool_app.h 在 **类体内部** `#include`
//    （「部分类 / partial class」写法）——方法仍属于 ClothingToolApp 这一个类，零接口改动。
//    目的：ClothingToolApp 日渐庞大，把「面板绘制 + 画笔 UI + 初始化页」这一大块（约占
//    原文件 1/3）拆出来，避免单头文件无限膨胀。不要单独 include；不要在别处复制这些方法。
//
// 覆盖：CommitNumberField / DrawPanels / DrawBrushPanel / ProcessBrush / DrawBrushMarkers /
//       MouseOverUi / DrawRelaxPanel / DrawHoleFillPanel / DrawLeftPanel / DrawRightPanel /
//       DrawSkinPanel / DrawMotionPanel。
//
// 折叠：除左上「变换 / 保存 / 场景」面板（DrawLeftPanel，常驻）外，其余面板均用
//   BeginAccordionPanel 绘制 —— 点标题栏展开/收起，收起时仅留标题栏（见该方法注释）。

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

    // 画各面板。除「平移/旋转/保存 glb」的左上角面板（DrawLeftPanel）保持原样外，其余
    // 面板均为可折叠（Accordion）面板：折叠时只显示标题栏，展开时显示标题栏 + 内容。
    // 所有布局都从**本帧窗口尺寸** winfo 推算（见文件头坐标空间说明）。
    void DrawPanels(const jpov::InputSnapshot& input,
                    const jpov::WindowInfo& winfo,
                    jpov::RenderCommandList* cmds) {
        const float w = winfo.width;
        const float h = winfo.height;

        jpov::UiTheme theme = jpov::UiTheme::Default(kFontSize);
        theme.font_alias = kViewerFontAlias;
        ui_.Begin(input, theme, w, h, 1000.0f / kViewerFps);

        panel_rects_.clear();   // 重新收集本帧面板矩形（画笔「鼠标在 UI 上」判定用）
        DrawLeftPanel(cmds);
        DrawRightPanel(cmds, w, h);
        // 顶部中间两面板同列堆叠：画笔面板先画，把面板高回传给松弛度面板定位
        //（任一折叠/展开都会带动下方那个随之上/下移）。
        const float brush_h = DrawBrushPanel(cmds, w);
        DrawRelaxPanel(cmds, w, brush_h);
        DrawSkinPanel(cmds, w, h);
        DrawHoleFillPanel(cmds, w, h);
        DrawMotionPanel(cmds, h);
    }

    // ---- 顶部中间：3D 画笔（可折叠）----
    //
    // 「画笔」按钮切换激活态（激活 = 存在选区概念；再点 / ESC = 结束并清空选区）；
    // 「画笔半径 (px)」滑条 5~200（默认 100）。激活态下鼠标周围画淡黄半透明圆表示尺
    // 寸，左键在非 UI 区涂抹把邻近顶点选入选区（见 ProcessBrush）。
    // 标题栏（折叠时仅此）：3D 画笔。展开内容行：按钮(1) + 半径(1) + 状态(1) = 3。
    // 返回本帧面板总高（供下方松弛度面板定位）。
    float DrawBrushPanel(jpov::RenderCommandList* cmds, float win_w) {
        const float panel_w  = kPanelWidth;  // 固定宽（像素）
        const float panel_x = std::max((win_w - panel_w) * 0.5f, kPanelMargin);  // 顶部居中
        const float panel_y = kPanelMargin;
        constexpr int kContentRows = 3;
        const AccordionFrame af = BeginAccordionPanel(
            cmds, panel_x, panel_y, panel_w, "3D 画笔", &brush_panel_open_,
            kContentRows);
        if (!af.expanded) {
            return af.height;
        }
        const PanelFrame& frame = af.frame;
        const float left = frame.left;
        const float row_w = frame.row_w;
        const float step_y = frame.step_y;
        const float kRowH = kPanelRowH;
        float row_y = frame.row_y;

        // 激活态按钮：点一次激活，再点一次结束（生命周期）。
        const char* btn =
            brush_.active() ? "画笔：激活中（点击结束）" : "画笔：点击激活";
        if (ui_.Button(btn, jpov::UiRect{{left, row_y}, {row_w, kRowH}})) {
            brush_.Toggle();
        }
        row_y += step_y;

        // 画笔半径（屏幕像素）：5~200，默认 100。
        ui_.SliderFloat("画笔半径 (px)", brush_.radius_px_mutable(),
                        jpov::UiRect{{left, row_y}, {row_w, kRowH}},
                        BrushTool::kMinRadiusPx, BrushTool::kMaxRadiusPx,
                        /*decimal_places*/ 0);
        row_y += step_y;

        const std::string status =
            brush_.active()
                ? Format("激活 · 已选 %zu 点（左键涂抹；ESC 结束）",
                         brush_.selected_count())
                : std::string("未激活（点按钮开始；ESC 结束）");
        const jpov::Color kForeground{0.92f, 0.93f, 0.95f, 1.0f};
        cmds->DrawText(status, /*pos*/ {left, row_y + (kRowH - kFontSize) * 0.5f},
                       kFontSize, kForeground, jpov::TextAlignment::kTopLeft,
                       kViewerFontAlias);
        return af.height;
    }

    // ---- 顶部中间（画笔面板下方）：布料 LAG 松弛度（可折叠）----
    //
    // 由「固定点」（3D 画笔选区）自动推导逐顶点松弛度（relax_derive.h）。固定点 = 画笔选区
    // ∪ 可选高度带；离固定点越远松弛度越大（测地距离），Laplacian 平滑去拐角。
    // 推导结果存本工具内；**保存 glb 时**写入 _JPOV_RELAX。
    // above_h = 上方画笔面板的**本帧总高**（定位在其下方；画笔折叠/展开会带动本面板上/下移）。
    // 标题栏：布料 LAG 松弛度。展开内容行：按钮(1) + 半径(1) + max_lag(1) + 显示(1) + 状态(1) = 5。
    void DrawRelaxPanel(jpov::RenderCommandList* cmds, float win_w, float above_h) {
        const float panel_w  = kPanelWidth;  // 固定宽（像素）
        const float panel_x = std::max((win_w - panel_w) * 0.5f, kPanelMargin);  // 顶部居中，与画笔面板同列
        const float panel_y = kPanelMargin + above_h + kPanelSpacing;  // 紧接画笔面板下方
        constexpr int kContentRows = 5;
        const AccordionFrame af = BeginAccordionPanel(
            cmds, panel_x, panel_y, panel_w, "布料 LAG 松弛度",
            &relax_panel_open_, kContentRows);
        if (!af.expanded) {
            return;
        }
        const PanelFrame& frame = af.frame;
        const float left = frame.left;
        const float row_w = frame.row_w;
        const float step_y = frame.step_y;
        const float kRowH = kPanelRowH;
        float row_y = frame.row_y;

        if (ui_.Button("由固定点推导松弛度",
                       jpov::UiRect{{left, row_y}, {row_w, kRowH}})) {
            RunDeriveRelax();
        }
        row_y += step_y;

        // 松弛度饱和（升到 1）的测地距离（米）：0 = 自动（用到最远顶点的测地距离定标）。
        ui_.SliderFloat("松弛度半径 (m, 0=自动)", &relax_radius_ui_,
                        jpov::UiRect{{left, row_y}, {row_w, kRowH}},
                        kRelaxRadiusMin, kRelaxRadiusMax, /*decimal_places*/ 3);
        row_y += step_y;

        // 逐顶点最大滞后位移 max_lag（米；相对当前 pose 的位移上限，防时间性拉伸）。
        ui_.SliderFloat("最大滞后位移 max_lag (m)", &relax_max_lag_ui_,
                        jpov::UiRect{{left, row_y}, {row_w, kRowH}},
                        kRelaxMaxLagMin, kRelaxMaxLagMax, /*decimal_places*/ 3);
        row_y += step_y;

        // 松弛度场可视化（2D 像素：蓝 = 固定(0)，红 = 自由(1)）。
        ui_.Checkbox("显示松弛度场（蓝=固定 红=自由）", &show_relax_field_,
                     jpov::UiRect{{left, row_y}, {row_w, kRowH}});
        row_y += step_y;

        if (!relax_msg_.empty()) {
            const jpov::Color kForeground{0.92f, 0.93f, 0.95f, 1.0f};
            cmds->DrawText(relax_msg_,
                           /*pos*/ {left, row_y + (kRowH - kFontSize) * 0.5f},
                           kFontSize, kForeground,
                           jpov::TextAlignment::kTopLeft, kViewerFontAlias);
        }
    }

    // ---- 3D 画笔：每帧处理（在面板之后调用；可用本帧面板矩形）----
    //
    //  ① 生命周期：ESC 结束（→ 清空选区）；激活/结束切换在 DrawBrushPanel 的按钮。
    //  ② 涂抹：仅「激活 + 鼠标不在任何面板上 + 在视口内 + 左键 drag/click」时，
    //     由鼠标像素构造世界射线，把衣物各 primitive 中靠近射线的顶点选入选区。
    //  ③ 绘制：激活态下画顶点标记（2px 淡黄方块）；鼠标非 UI 时再画画笔圆（淡黄半透明）。
    //  ④ 激活态结束 → 选区清空（BrushTool::SetActive(false)）。
    void ProcessBrush(const jpov::InputSnapshot& input,
                      const jpov::WindowInfo& winfo,
                      jpov::RenderCommandList* cmds) {
        // 生命周期：ESC 结束。
        if (brush_.active() && input.GetKey(jpov::KeyCode::Escape).IsClick()) {
            brush_.SetActive(false);
        }
        if (!brush_.active()) {
            return;
        }

        const float w = winfo.width;
        const float h = winfo.height;
        const float mx = input.mouse_x;
        const float my = input.mouse_y;
        const bool over_ui = MouseOverUi(mx, my);
        const bool in_view = (mx >= 0.0f && mx < w && my >= 0.0f && my < h);

        // 相机基（与渲染相机一致：position = view_.Position() + 注视点偏移）。
        const jpov::Vec3f target = CameraTarget();
        const CameraBasis basis =
            MakeCameraBasis(view_.Position() + target, target,
                            /*world_up*/ {0.0f, 1.0f, 0.0f}, kCameraFovDeg, w, h);

        // 涂抹：仅非 UI、视口内、左键 drag/click（drag 用帧末鼠标位置，靠 60Hz 连续）。
        if (!over_ui && in_view && (input.left.IsDrag() || input.left.IsClick())) {
            const CameraRay ray = RayFromPixel(basis, mx, my);
            const size_t prims =
                std::min(cloth_current_.size(), brush_.primitive_count());
            for (size_t i = 0; i < prims; ++i) {
                brush_.PaintAlongRay(i, cloth_current_[i].mesh.positions, ray.origin,
                                     ray.dir, brush_.radius_px(), h,
                                     basis.tan_half_fov);
            }
        }

        // 画笔圆（淡黄半透明）：鼠标非 UI 且在视口内时显示。
        if (!over_ui && in_view) {
            const jpov::Color kBrushCircle{1.0f, 0.95f, 0.55f, 0.30f};
            cmds->DrawCircle(/*center*/ {mx, my}, brush_.radius_px(), kBrushCircle);
        }

        // 顶点标记（2px 淡黄方块）：激活态下始终画选中点。
        DrawBrushMarkers(basis, cmds);
    }

    // 把选中顶点投影到屏幕，用 2px 淡黄方块标记（相机后方 / 越界跳过）。
    void DrawBrushMarkers(const CameraBasis& basis, jpov::RenderCommandList* cmds) {
        const jpov::Color kMark{1.0f, 0.93f, 0.45f, 1.0f};
        const float kMarkSize = 2.0f;
        const size_t prims =
            std::min(cloth_current_.size(), brush_.primitive_count());
        for (size_t i = 0; i < prims; ++i) {
            const std::vector<jpov::Vec3f>& pos = cloth_current_[i].mesh.positions;
            for (uint32_t idx : brush_.selected(i)) {
                if (idx >= pos.size()) {
                    continue;
                }
                float px = 0.0f;
                float py = 0.0f;
                if (!ProjectToScreen(basis, pos[idx], &px, &py, nullptr)) {
                    continue;
                }
                cmds->DrawRect(/*pos*/ {px - kMarkSize * 0.5f, py - kMarkSize * 0.5f},
                               /*size*/ {kMarkSize, kMarkSize}, kMark);
            }
        }
    }

    // 鼠标是否落在任一面板（UI）矩形内（画笔「UI hover 不算画笔」用）。
    bool MouseOverUi(float mx, float my) const {
        for (const jpov::UiRect& r : panel_rects_) {
            if (mx >= r.pos.x() && mx <= r.pos.x() + r.size.x() && my >= r.pos.y() &&
                my <= r.pos.y() + r.size.y()) {
                return true;
            }
        }
        return false;
    }

    // ---- 底部中间：补洞（推进法）（可折叠）----
    //
    // 紧邻右下「软布自动蒙皮」面板的左侧（贴底）。按钮「执行补洞」把网格破洞补上（
    // 在仿真关联之前）；「最大洞周长」滑条滤掉腰口/下摆这类本来该开的口。
    // 标题栏：补洞（推进法）。展开内容行：按钮(1) + 细分(1) + 平滑(1) + 最大周长(1) + 状态(1) = 5。
    void DrawHoleFillPanel(jpov::RenderCommandList* cmds, float win_w, float win_h) {
        const float panel_w  = kPanelWidth;  // 固定宽（像素）
        constexpr int kContentRows = 5;
        // 贴底、位于蒙皮面板（贴右）左侧；与左下摆动面板留出间隔。
        const float panel_x = std::max(
            win_w - 2.0f * panel_w - 2.0f * kPanelMargin, kPanelMargin);
        // 贴底锚定：折叠时面板变矮 逐向上收（y 变大）。
        const float panel_y = std::max(
            win_h - AccordionHeight(holefill_panel_open_, kContentRows) -
                kPanelMargin,
            kPanelMargin);
        const AccordionFrame af = BeginAccordionPanel(
            cmds, panel_x, panel_y, panel_w, "补洞（推进法）",
            &holefill_panel_open_, kContentRows);
        if (!af.expanded) {
            return;
        }
        const PanelFrame& frame = af.frame;
        const float left = frame.left;
        const float row_w = frame.row_w;
        const float step_y = frame.step_y;
        const float kRowH = kPanelRowH;
        float row_y = frame.row_y;

        if (ui_.Button("执行补洞", jpov::UiRect{{left, row_y}, {row_w, kRowH}})) {
            RunHoleFill();
        }
        row_y += step_y;

        ui_.Checkbox("细分（匹配周围边长）", &hole_refine_ui_,
                     jpov::UiRect{{left, row_y}, {row_w, kRowH}});
        row_y += step_y;

        ui_.Checkbox("平滑（fairing）", &hole_fair_ui_,
                     jpov::UiRect{{left, row_y}, {row_w, kRowH}});
        row_y += step_y;

        // 最大洞周长（米）：只补周长 ≤ 该值的环；0 = 不限制（⚠️ 会把腰口/下摆也封上）。
        ui_.SliderFloat("最大洞周长 (m)", &hole_max_perimeter_,
                        jpov::UiRect{{left, row_y}, {row_w, kRowH}},
                        0.0f, 1.0f, /*decimal_places*/ 3);
        row_y += step_y;

        if (!hole_fill_msg_.empty()) {
            const jpov::Color kForeground{0.92f, 0.93f, 0.95f, 1.0f};
            cmds->DrawText(hole_fill_msg_,
                           /*pos*/ {left, row_y + (kRowH - kFontSize) * 0.5f},
                           kFontSize, kForeground,
                           jpov::TextAlignment::kTopLeft, kViewerFontAlias);
        }
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
    void DrawLeftPanel(jpov::RenderCommandList* cmds) {
        const float panel_w  = kPanelWidth;  // 固定宽（像素）
        const float panel_x  = kPanelMargin;
        const float panel_y  = kPanelMargin;
        // 行数：平移表头1 + 平移3 + 旋转表头1 + 旋转3 + 缩放1 + 保存1 + 保存状态1
        //        + 地面1 + 勾选1 + 只读2 = 15。
        constexpr int kRows = 15;
        PanelFrame frame = BeginPanel(cmds, panel_x, panel_y, panel_w, kRows);
        const float left = frame.left;
        const float row_w = frame.row_w;
        const float step_y = frame.step_y;
        const float kRowH = kPanelRowH;
        const float kSpacing = kPanelSpacing;

        const float axis_w = 30.0f;     // "轴"列（X/Y/Z）
        const float stepbox_w = 72.0f;  // 步长输入框宽
        const float btn_w = 26.0f;      // "<" / ">" / "−" / "+" 按钮宽
        const float gap = kSpacing;
        const float col_axis   = left;
        const float col_stepbx = col_axis + axis_w + gap;
        const float col_btn_in = col_stepbx + stepbox_w + gap;   // "<"
        const float col_btn_in2 = col_btn_in + btn_w + gap;      // ">"

        float row_y = frame.row_y;

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

    // ---- 右上角：仿真动力学（可折叠）----
    //
    // 注意：面板**贴右边缘**，x 从**本帧窗口宽度** win_w 反推（不是固定常量），
    // 这样窗口 resize 时右上角面板始终贴住右上角。
    //
    // 标题栏：仿真动力学。展开内容行：关联距离(1) + 重力/质量/力系数/衰减/速度上限 滑条(5)
    //     + 人体排斥开关(1) + buffer(1) + 切向保留系数(1) + 显示力学参照网格(1) + 按钮行(1)
    //     + 状态行(2) = 13。
    void DrawRightPanel(jpov::RenderCommandList* cmds, float win_w, float win_h) {
        (void)win_h;
        const float panel_w  = kPanelWidth;  // 固定宽（像素）
        const float panel_x  = std::max(win_w - panel_w - kPanelMargin, kPanelMargin);  // 贴右边缘
        const float panel_y  = kPanelMargin;
        constexpr int kContentRows = 13;
        const AccordionFrame af = BeginAccordionPanel(
            cmds, panel_x, panel_y, panel_w, "仿真动力学", &right_panel_open_,
            kContentRows);
        if (!af.expanded) {
            return;
        }
        const PanelFrame& frame = af.frame;
        const float left = frame.left;
        const float row_w = frame.row_w;
        const float step_y = frame.step_y;
        const float kRowH = kPanelRowH;
        const float kSpacing = kPanelSpacing;
        float row_y = frame.row_y;

        // ---- 关联距离 d（米）：仿真点集合 / 关联邻居表（= 仿真点"缝合"半径）。
        //      拖它 → 重建仿真点集合（与 soft_mesh_viewer 同语义）。----
        ui_.SliderFloat("关联距离 d (m)", &bind_distance_ui_,
                        jpov::UiRect{{left, row_y}, {row_w, kRowH}},
                        Simulator::kMinBindDistance, Simulator::kMaxBindDistance,
                        /*decimal_places*/3);
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

        // ---- 力学参照网格：开启后用蓝色 2D 像素画出「力参照的网格」
        //      （= 原始 mesh + 全体 scale/rotation；不含选区变换 / 仿真形变，见 README）。----
        ui_.Checkbox("显示力学参照网格", &show_ref_mesh_,
                     jpov::UiRect{{left, row_y}, {row_w, kRowH}});
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
            ResetCloth();
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

    // ---- 右下角：软布自动蒙皮（一键）（可折叠）----
    //
    // 贴右下角（x 由窗口宽反推、y 由窗口高反推），与左上（变换/保存）、右上（动力学）
    // 互不遮挡。
    // 标题栏：软布自动蒙皮。展开内容行：一键蒙皮按钮(1) + 权重生长勾选(1) + 种子半径输入(1)
    //             + 焊接模式勾选(1) + 焊接参数输入(1) + base color alpha 归一按钮(1)
    //             + 蒙皮状态(1) + alpha 状态(1) = 8。
    void DrawSkinPanel(jpov::RenderCommandList* cmds, float win_w, float win_h) {
        const float panel_w  = kPanelWidth;  // 固定宽（像素）
        constexpr int kContentRows = 8;
        const float panel_x = std::max(win_w - panel_w - kPanelMargin, kPanelMargin);  // 贴右边缘
        // 贴底锚定：折叠时面板变矮 逐向上收。
        const float panel_y = std::max(
            win_h - AccordionHeight(skin_panel_open_, kContentRows) -
                kPanelMargin,
            kPanelMargin);
        const AccordionFrame af = BeginAccordionPanel(
            cmds, panel_x, panel_y, panel_w, "软布自动蒙皮", &skin_panel_open_,
            kContentRows);
        if (!af.expanded) {
            return;
        }
        const PanelFrame& frame = af.frame;
        const float left = frame.left;
        const float row_w = frame.row_w;
        const float step_y = frame.step_y;
        const float kRowH = kPanelRowH;
        float row_y = frame.row_y;

        // 一键蒙皮按钮：衣服带蒙皮后 RunAutoSkin 自身 no-op（按钮仍可点，文案变明示状态）。
        const char* btn = ClothMeshHasSkin() ? "已蒙皮" : "一键蒙皮";
        if (ui_.Button(btn, jpov::UiRect{{left, row_y}, {row_w, kRowH}})) {
            RunAutoSkin();
        }
        row_y += step_y;

        ui_.Checkbox("权重生长（种子扩散）", &skin_grow_ui_,
                     jpov::UiRect{{left, row_y}, {row_w, kRowH}});
        row_y += step_y;

        // 种子半径（mm）：到身体距离 <= 该值的顶点 = 种子（生长锚点，权重冻结）。
        const float seed_label_w = 96.0f;
        DrawLabel("种子半径(mm)", left, seed_label_w, row_y);
        const float seed_box_x = left + seed_label_w + 8.0f;
        const float seed_box_w = row_w - seed_label_w - 8.0f;
        const bool seed_focus = ui_.InputText(
            "", seed_eps_field_.text, kAxisInputCapacity,
            jpov::UiRect{{seed_box_x, row_y}, {seed_box_w, kRowH}});
        if (seed_eps_field_.focused_prev && !seed_focus) {
            CommitNumberField(&seed_eps_field_, ClampSeedEpsMm, &seed_eps_mm_);
        }
        seed_eps_field_.focused_prev = seed_focus;
        row_y += step_y;

        // 焊接判据模式：勾选 = 相对局部边长（比例，与缩放/密度无关）；否则 = 绝对距离（mm）。
        ui_.Checkbox("焊接用相对局部边长", &weld_relative_ui_,
                     jpov::UiRect{{left, row_y}, {row_w, kRowH}});
        row_y += step_y;

        // 焊接参数：相对模式填「比例(×局部边长)」，绝对模式填「容差(mm)」。
        const float weld_label_w = 96.0f;
        const char* weld_label =
            weld_relative_ui_ ? "焊接比例(×边长)" : "焊接容差(mm)";
        DrawLabel(weld_label, left, weld_label_w, row_y);
        const float weld_box_x = left + weld_label_w + 8.0f;
        const float weld_box_w = row_w - weld_label_w - 8.0f;
        if (weld_relative_ui_) {
            const bool f = ui_.InputText(
                "", weld_ratio_field_.text, kAxisInputCapacity,
                jpov::UiRect{{weld_box_x, row_y}, {weld_box_w, kRowH}});
            if (weld_ratio_field_.focused_prev && !f) {
                CommitNumberField(&weld_ratio_field_, ClampWeldRatio, &weld_ratio_);
            }
            weld_ratio_field_.focused_prev = f;
        } else {
            const bool f = ui_.InputText(
                "", weld_tolerance_field_.text, kAxisInputCapacity,
                jpov::UiRect{{weld_box_x, row_y}, {weld_box_w, kRowH}});
            if (weld_tolerance_field_.focused_prev && !f) {
                CommitNumberField(&weld_tolerance_field_, ClampWeldToleranceMm,
                                  &weld_tolerance_mm_);
            }
            weld_tolerance_field_.focused_prev = f;
        }
        row_y += step_y;

        // base color alpha 归一：把贴图 alpha 置 1（不透明）。画笔有选区 → 只处理「被选中
        // 顶点围成的三角形」覆盖的 texel；无选区 → 整图。（见 base_color_alpha.h / README）
        if (ui_.Button("base color alpha 归一",
                       jpov::UiRect{{left, row_y}, {row_w, kRowH}})) {
            RunAlphaNormalize();
        }
        row_y += step_y;

        // 蒙皮状态 / 提示（可能为空）。
        if (!skin_msg_.empty()) {
            const jpov::Color kForeground{0.92f, 0.93f, 0.95f, 1.0f};
            cmds->DrawText(skin_msg_,
                           /*pos*/ {left, row_y + (kRowH - kFontSize) * 0.5f},
                           kFontSize, kForeground,
                           jpov::TextAlignment::kTopLeft, kViewerFontAlias);
        }
        row_y += step_y;

        // alpha 归一状态 / 提示（可能为空）。
        if (!alpha_msg_.empty()) {
            const jpov::Color kForeground{0.92f, 0.93f, 0.95f, 1.0f};
            cmds->DrawText(alpha_msg_,
                           /*pos*/ {left, row_y + (kRowH - kFontSize) * 0.5f},
                           kFontSize, kForeground,
                           jpov::TextAlignment::kTopLeft, kViewerFontAlias);
        }
    }

    // ---- 左下角：随机摆动测试 + 整体平动 + 平动 LAG（真骨架蒙皮驱动人体 + 已蒙皮衣服）----
    //
    // **贴左下角 sticky**（x 贴左、y 由窗口高反推并 clamp 在视口内）。可折叠。
    // 标题栏：随机摆动测试。展开内容行：启用(1) + 播放(1) + 主轴(1) + 幅度(1) + 种子(1)
    //     + 平动 XY(2) + LAG(1) + 二阶(1) + 上升时间(1) + 超调量(1) + 状态(1) = 12。
    void DrawMotionPanel(jpov::RenderCommandList* cmds, float win_h) {
        // 状态提示（每帧刷新，供面板与出图自检）。
        if (!has_body_skeleton_) {
            motion_msg_ = "人体无骨架，无法摆动";
        } else if (body_skel_id_ == 0) {
            motion_msg_ = "骨架未注册";
        } else if (!motion_test_enabled_) {
            motion_msg_ = "勾选后启用（先蒙皮再播放，衣服才跟随）";
        } else if (!ClothMeshHasSkin()) {
            motion_msg_ = "衣服未蒙皮：动作不带动衣服";
        } else if (lag_enabled_ui_) {
            motion_msg_ = Format("人体 + 衣服同步摆动（%s LAG 平动滞后）",
                                 lag_second_order_ui_ ? "二阶" : "一阶");
        } else {
            motion_msg_ = "人体 + 已蒙皮衣服同步摆动";
        }

        const float panel_w  = kPanelWidth;  // 固定宽（像素）
        constexpr int kContentRows = 12;
        const float panel_x = kPanelMargin;  // 贴左边缘
        // 贴底边缘；窗口过矮时夹到顶边以内（不让面板跑出视口）。折叠时面板变矮 逐向上收。
        const float panel_y = std::max(
            win_h - AccordionHeight(motion_panel_open_, kContentRows) -
                kPanelMargin,
            kPanelMargin);
        const AccordionFrame af = BeginAccordionPanel(
            cmds, panel_x, panel_y, panel_w, "随机摆动测试", &motion_panel_open_,
            kContentRows);
        if (!af.expanded) {
            return;
        }
        const PanelFrame& frame = af.frame;
        const float left = frame.left;
        const float row_w = frame.row_w;
        const float step_y = frame.step_y;
        const float kRowH = kPanelRowH;
        float row_y = frame.row_y;

        // 启用勾选（面板标题已由标题栏承担，这里只做开关）。
        ui_.Checkbox("启用", &motion_test_enabled_,
                     jpov::UiRect{{left, row_y}, {row_w, kRowH}});
        row_y += step_y;

        const char* play_label = motion_playing_ ? "暂停" : "播放";
        if (ui_.Button(play_label, jpov::UiRect{{left, row_y}, {row_w, kRowH}})) {
            motion_playing_ = !motion_playing_;
        }
        row_y += step_y;

        // 主轴（相位）：播放时自动推进（速度固定）；暂停时可拖动查看任意时刻的动作。
        ui_.SliderFloat("主轴（相位）", &motion_phase_,
                        jpov::UiRect{{left, row_y}, {row_w, kRowH}},
                        0.0f, 1.0f, /*decimal_places*/ 3);
        row_y += step_y;

        // 幅度：按档位量化（幅度是烘焙维度，19 档一次烘完，切档无需重烘）。
        float amp = motion_amplitude_deg_;
        ui_.SliderFloat("关节幅度 (°)", &amp,
                        jpov::UiRect{{left, row_y}, {row_w, kRowH}},
                        0.0f, kMotionAmplitudeMaxDeg, /*decimal_places*/ 0);
        motion_amplitude_deg_ = SnapAmplitudeDeg(amp);
        row_y += step_y;

        // 随机种子：输入框（0~65535，失焦/回车提交 → clamp → 变了则重烘焙姿态集）。
        const float seed_label_w = 96.0f;
        DrawLabel("随机种子", left, seed_label_w, row_y);
        const float seed_box_x = left + seed_label_w + 8.0f;
        const float seed_box_w = row_w - seed_label_w - 8.0f;
        const bool seed_focus = ui_.InputText(
            "", motion_seed_field_.text, kAxisInputCapacity,
            jpov::UiRect{{seed_box_x, row_y}, {seed_box_w, kRowH}});
        if (motion_seed_field_.focused_prev && !seed_focus) {
            CommitMotionSeedField();
        }
        motion_seed_field_.focused_prev = seed_focus;
        row_y += step_y;

        // 整体平动（世界 m，±2）：人体 + 衣服的世界平移；LAG 时衣服按 tracker 滞后。
        static const char* const kTransLabels[2] = {"整体平动 X (m)", "整体平动 Y (m)"};
        for (int a = 0; a < 2; ++a) {
            ui_.SliderFloat(kTransLabels[a], &body_trans_ui_[a],
                            jpov::UiRect{{left, row_y}, {row_w, kRowH}},
                            -kBodyTransMaxM, kBodyTransMaxM, /*decimal_places*/ 2);
            row_y += step_y;
        }
        // LAG：布料按「整体平动」的一阶/二阶延迟滞后（→ pos_lag；需网格自带 _JPOV_RELAX）。
        ui_.Checkbox("开启 LAG（平动滞后）", &lag_enabled_ui_,
                     jpov::UiRect{{left, row_y}, {row_w, kRowH}});
        row_y += step_y;
        ui_.Checkbox("二阶延迟（勾=二阶，否则一阶）", &lag_second_order_ui_,
                     jpov::UiRect{{left, row_y}, {row_w, kRowH}});
        row_y += step_y;
        ui_.SliderFloat("上升时间 (ms)", &lag_rise_ms_ui_,
                        jpov::UiRect{{left, row_y}, {row_w, kRowH}},
                        kLagRiseMsMin, kLagRiseMsMax, /*decimal_places*/ 0);
        row_y += step_y;
        ui_.SliderFloat("超调量（仅二阶）", &lag_overshoot_ui_,
                        jpov::UiRect{{left, row_y}, {row_w, kRowH}},
                        kLagOvershootMin, kLagOvershootMax, /*decimal_places*/ 2);
        row_y += step_y;

        const jpov::Color kForeground{0.92f, 0.93f, 0.95f, 1.0f};
        cmds->DrawText(motion_msg_,
                       /*pos*/ {left, row_y + (kRowH - kFontSize) * 0.5f},
                       kFontSize, kForeground,
                       jpov::TextAlignment::kTopLeft, kViewerFontAlias);
    }
