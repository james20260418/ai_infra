// JPOV 模型编辑器 — 左键 drag 归属 + 布局随窗口自适应 回归测试
//
// 覆盖两类"已发生过/极易破"的问题（白盒：本类是 EditorApp 的 friend）：
//
//  A. 左键 drag 归属（实测发生过的 bug）：「拖动 平移X 滑条时，RX 跟着一起动」。
//     根因：EditorApp::UpdateRotateFromDrag 无条件消费**任何**左键 drag，
//     没区分 drag 起于底部面板还是 3D 视口 → 同一个 mouse_dx 被滑条与旋转逻辑
//     各吃一次。修法：归属在 drag 起点冻结（起点在面板内则整段按住都不旋转）。
//
//  B. 布局随窗口尺寸自适应（改布局极易破）：面板/说明的坐标空间是**每帧渲染
//     分辨率**（= 当帧窗口尺寸），故窗口变大后滑条必须仍贴左下、说明仍贴左上。
//     早期版本把宽高写死 1280×720，窗口放大后滑条飘在中间偏上。
//
// 断言写法纪律（skills/zero-run-code-reading-check）：每条都必须存在能令其
// 失败的合法改动，禁止恒真检查。

#include <cmath>

#include <glog/logging.h>

#include "tools/jpov/demo/editor/editor_app.h"

namespace jpov_viewer {

class EditorDragOwnershipTest {
public:
    static void Run() {
        TestLayoutHugsWindowCorners();
        TestLayoutScalesWithWindow();
        TestGeometryCoversAllRows();
        TestHelpRegionIsPanel();
        TestDragOriginDecidesRotate();
    }

private:
    static void ExpectTrue(bool cond, const char* msg) {
        if (!cond) LOG(FATAL) << msg;
    }
    static bool OriChanged(const ModelPlacement& p, const jpov::Vec3f& up0,
                           const jpov::Vec3f& fr0) {
        auto d = [](const jpov::Vec3f& a, const jpov::Vec3f& b) {
            return std::abs(a.x()-b.x()) + std::abs(a.y()-b.y()) +
                   std::abs(a.z()-b.z());
        };
        return d(p.up, up0) > 1e-6f || d(p.front, fr0) > 1e-6f;
    }

    // 参照窗口（默认尺寸）与一个"放大"窗口，用于验证自适应。
    static constexpr float kW0 = static_cast<float>(kEditorDefaultWidth);
    static constexpr float kH0 = static_cast<float>(kEditorDefaultHeight);
    static constexpr float kW1 = 1920.0f;   // 放大后的窗口
    static constexpr float kH1 = 1080.0f;

    // A1. 滑条贴左下角、说明贴左上角（用默认 1280×720）。
    static void TestLayoutHugsWindowCorners() {
        EditorApp app(JPOV::Config{});
        const EditorApp::PanelLayout L = EditorApp::MakeLayoutForTest(kW0, kH0);
        const jpov::UiRect r0 = L.Row(0);
        const jpov::UiRect r_last = L.Row(EditorApp::PanelLayout::kPanelRows - 1);

        // 左缘贴左边距（需求：滑条靠左，不居中）。
        ExpectTrue(std::abs(r0.pos.x() - EditorApp::PanelLayout::kMarginLeft) < 1e-3f,
                   "滑条左缘应贴左边距（靠左不居中）");
        // 最后一行底缘贴屏底留白（需求：整体贴左下角）。
        const float last_bottom = r_last.pos.y() + r_last.size.y();
        ExpectTrue(std::abs(last_bottom - (kH0 - EditorApp::PanelLayout::kBottom))
                       < 1e-3f,
                   "滑条最后一行底缘应贴屏底（整体贴左下角）");
        // 滑条宽 = 半屏（需求：宽度不变）。
        ExpectTrue(std::abs(r0.size.x() - 0.5f * kW0) < 1e-3f,
                   "滑条宽度应为半屏（宽度不变）");
        // 说明贴左上角。
        const jpov::UiRect help = L.Help();
        ExpectTrue(help.pos.x() < 40.0f && help.pos.y() < 40.0f,
                   "说明文字应贴左上角");
        LOG(INFO) << "OK TestLayoutHugsWindowCorners";
    }

    // A2. ⭐ 窗口放大后布局必须重新贴边（本次修复的核心回归）。
    //     若实现把尺寸写死成默认窗口值，放大后 r_last 底缘就不再贴新屏底。
    static void TestLayoutScalesWithWindow() {
        EditorApp app(JPOV::Config{});
        const EditorApp::PanelLayout L = EditorApp::MakeLayoutForTest(kW1, kH1);
        const jpov::UiRect r_last =
            L.Row(EditorApp::PanelLayout::kPanelRows - 1);
        // 底缘仍贴**新**屏底。
        const float last_bottom = r_last.pos.y() + r_last.size.y();
        ExpectTrue(std::abs(last_bottom - (kH1 - EditorApp::PanelLayout::kBottom))
                       < 1e-3f,
                   "🔴 窗口放大后滑条底缘必须贴新的屏底（写死尺寸会飘到中间）");
        // 宽度随窗口走（半屏），不是固定的旧值。
        ExpectTrue(std::abs(r_last.size.x() - 0.5f * kW1) < 1e-3f,
                   "窗口放大后滑条宽应为新半屏");
        // 说明仍在左上角（不随窗口移动）。
        const jpov::UiRect help = L.Help();
        ExpectTrue(help.pos.x() < 40.0f && help.pos.y() < 40.0f,
                   "窗口放大后说明仍应贴左上角");
        // 归属判定也随窗口走：在新窗口的"原默认尺寸右下方"仍是面板区。
        // （若写死 720 高，这个点会被误判成视口。）
        ExpectTrue(app.PointInPanelForTest(kW1 * 0.5f, kH1 - 30.0f, kW1, kH1),
                   "🔴 放大后用新尺寸判定：底部区域仍须属于面板");
        LOG(INFO) << "OK TestLayoutScalesWithWindow";
    }

    // A3. 几何自洽：PointInPanel 判定的范围必须盖住每一行（防"能画却拖不动"）。
    static void TestGeometryCoversAllRows() {
        EditorApp app(JPOV::Config{});
        for (int i = 0; i < EditorApp::PanelLayout::kPanelRows; ++i) {
            const jpov::UiRect r = EditorApp::MakeLayoutForTest(kW0, kH0).Row(i);
            ExpectTrue(app.PointInPanelForTest(r.pos.x(), r.pos.y(), kW0, kH0),
                       "每行左上角必须判为在面板内");
            ExpectTrue(app.PointInPanelForTest(
                           r.pos.x() + r.size.x(), r.pos.y() + r.size.y(),
                           kW0, kH0),
                       "每行右下角必须判为在面板内");
        }
        LOG(INFO) << "OK TestGeometryCoversAllRows";
    }

    // A4. 左上角说明区计入面板（防隐形触发区），且不与滑条区重叠。
    static void TestHelpRegionIsPanel() {
        EditorApp app(JPOV::Config{});
        const EditorApp::PanelLayout L = EditorApp::MakeLayoutForTest(kW0, kH0);
        const jpov::UiRect help = L.Help();
        ExpectTrue(app.PointInPanelForTest(help.pos.x() + 4.0f,
                                          help.pos.y() + 4.0f, kW0, kH0),
                   "左上角说明文字区必须计入面板（否则拖文字会意外旋转模型）");
        ExpectTrue(help.pos.y() + help.size.y() < L.Row(0).pos.y(),
                   "说明文字区不应与底部滑条区重叠");
        LOG(INFO) << "OK TestHelpRegionIsPanel";
    }

    // B. 左键 drag 归属：按面板拖不旋转 / 按视口拖旋转 / 起点冻结 / Hold 路径。
    static void TestDragOriginDecidesRotate() {
        const EditorApp::PanelLayout L = EditorApp::MakeLayoutForTest(kW0, kH0);
        const jpov::UiRect p_row = L.Row(1);   // 平移X 所在行
        const float panel_x = p_row.pos.x() + p_row.size.x() * 0.5f;
        const float panel_y = p_row.pos.y() + p_row.size.y() * 0.5f;
        const float view_x  = kW0 * 0.5f;
        const float view_y  = kH0 * 0.25f;

        // 路径 A：按在面板上横向拖 → 朝向必须纹丝不动。
        {
            EditorApp app(JPOV::Config{});
            const jpov::Vec3f up0 = app.placement_.up, fr0 = app.placement_.front;
            jpov::InputSnapshot in{};
            in.left.raw = -1; in.mouse_x = panel_x; in.mouse_y = panel_y;
            app.UpdateRotateFromDragForTest(in, kW0, kH0);
            jpov::InputSnapshot in2{};
            in2.left.raw = -1; in2.mouse_x = panel_x + 120.0f; in2.mouse_y = panel_y;
            in2.mouse_dx = 120.0f;
            app.UpdateRotateFromDragForTest(in2, kW0, kH0);
            app.UpdateRotateFromDragForTest(in2, kW0, kH0);
            ExpectTrue(!OriChanged(app.placement_, up0, fr0),
                       "🔴 按在面板上拖动不得改变朝向（本次修复的核心）");
        }
        // 路径 B：按在 3D 视口里横向拖 → 朝向必须变（防修复阉掉功能）。
        {
            EditorApp app(JPOV::Config{});
            const jpov::Vec3f up0 = app.placement_.up, fr0 = app.placement_.front;
            jpov::InputSnapshot in{};
            in.left.raw = -1; in.mouse_x = view_x; in.mouse_y = view_y;
            app.UpdateRotateFromDragForTest(in, kW0, kH0);
            jpov::InputSnapshot in2{};
            in2.left.raw = -1; in2.mouse_x = view_x + 320.0f; in2.mouse_y = view_y;
            in2.mouse_dx = 160.0f;
            app.UpdateRotateFromDragForTest(in2, kW0, kH0);
            app.UpdateRotateFromDragForTest(in2, kW0, kH0);
            ExpectTrue(OriChanged(app.placement_, up0, fr0),
                       "视口内横向拖必须改变朝向（否则修复把功能一起阉了）");
        }
        // 路径 C：归属在起点冻结（起点面板内，之后拖到视口区 → 仍不旋转）。
        {
            EditorApp app(JPOV::Config{});
            const jpov::Vec3f up0 = app.placement_.up, fr0 = app.placement_.front;
            jpov::InputSnapshot in{};
            in.left.raw = -1; in.mouse_x = panel_x; in.mouse_y = panel_y;
            app.UpdateRotateFromDragForTest(in, kW0, kH0);
            jpov::InputSnapshot in2{};
            in2.left.raw = -1; in2.mouse_x = view_x; in2.mouse_y = view_y;
            in2.mouse_dx = 200.0f;
            app.UpdateRotateFromDragForTest(in2, kW0, kH0);
            ExpectTrue(!OriChanged(app.placement_, up0, fr0),
                       "归属应在 drag 起点冻结：起点在面板上则整段按住都不旋转");
        }
        // 路径 D：先在面板上 Hold（原地不动），再移到视口区拖动 → 归属取按下点。
        // （若等 IsDrag 转真才判归属，这里会被误判成起于视口 → 旋转。）
        {
            EditorApp app(JPOV::Config{});
            const jpov::Vec3f up0 = app.placement_.up, fr0 = app.placement_.front;
            jpov::InputSnapshot hold{};
            hold.left.raw = -2; hold.mouse_x = panel_x; hold.mouse_y = panel_y;
            app.UpdateRotateFromDragForTest(hold, kW0, kH0);
            jpov::InputSnapshot drag{};
            drag.left.raw = -1; drag.mouse_x = view_x; drag.mouse_y = view_y;
            drag.mouse_dx = 150.0f;
            app.UpdateRotateFromDragForTest(drag, kW0, kH0);
            app.UpdateRotateFromDragForTest(drag, kW0, kH0);
            ExpectTrue(!OriChanged(app.placement_, up0, fr0),
                       "Hold 后拖走：归属应取按下点（面板内）→ 不得旋转");
        }
        // 路径 E：反向 —— 视口区 Hold 后拖到面板上 → 仍应旋转。
        {
            EditorApp app(JPOV::Config{});
            const jpov::Vec3f up0 = app.placement_.up, fr0 = app.placement_.front;
            jpov::InputSnapshot hold{};
            hold.left.raw = -2; hold.mouse_x = view_x; hold.mouse_y = view_y;
            app.UpdateRotateFromDragForTest(hold, kW0, kH0);
            jpov::InputSnapshot drag{};
            drag.left.raw = -1; drag.mouse_x = panel_x; drag.mouse_y = panel_y;
            drag.mouse_dx = 150.0f;
            app.UpdateRotateFromDragForTest(drag, kW0, kH0);
            ExpectTrue(OriChanged(app.placement_, up0, fr0),
                       "视口区 Hold 后拖到面板：归属取按下点（视口）→ 应旋转");
        }
        LOG(INFO) << "OK TestDragOriginDecidesRotate";
    }
};

}  // namespace jpov_viewer

int main(int /*argc*/, char** argv) {
    google::InitGoogleLogging(argv[0]);
    jpov_viewer::EditorDragOwnershipTest::Run();
    LOG(INFO) << "editor_drag_ownership_test: ALL PASS";
    return 0;
}
