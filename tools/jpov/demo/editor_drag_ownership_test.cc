// JPOV 模型编辑器 — 左键 drag 归属（面板 vs 3D 视口）回归测试
//
// 复现并锁死 Danis 报的 bug：「拖动 平移X 滑条时，RX 跟着一起动」。
//
// 根因（已修）：EditorApp::UpdateRotateFromDrag 无条件消费**任何**左键 drag，
// 没区分「drag 发起在底部面板上」还是「发起在 3D 视口里」。于是拖滑条时，
// 同一个 mouse_dx 被滑条与旋转逻辑各吃一次 → tx 与 rx 同时变。
//
// 修法：drag **起点**若落在面板矩形内 → 本次按住期间旋转逻辑不插手
// （归属在起点冻结，中途拖出/拖入面板不改归属）。
//
// 白盒：本测试类是 EditorApp 的 friend，直接验私有逻辑（不增公共 API）。
// 断言写法纪律（skills/zero-run-code-reading-check）：每条都必须存在能令其
// 失败的合法改动，禁止恒真检查。

#include <cmath>

#include <glog/logging.h>

#include "tools/jpov/demo/editor_app.h"

namespace jpov_viewer {

// friend 类：白盒访问 EditorApp 私有几何/输入逻辑。
// 全部断言方法必须是本类成员（自由函数拿不到 friend 权限）。
class EditorDragOwnershipTest {
public:
    static void Run() {
        TestPointInPanel();
        TestGeometryCoversAllRows();
        TestDragOriginDecidesRotate();
    }

private:
    static void ExpectTrue(bool cond, const char* msg) {
        if (!cond) LOG(FATAL) << msg;
    }

    // 朝向是否与给定基准一致（判定"有没有被旋转动过"）。
    static bool OriChanged(const jpov_viewer::ModelPlacement& p,
                           const jpov::Vec3f& up0, const jpov::Vec3f& fr0) {
        auto d = [](const jpov::Vec3f& a, const jpov::Vec3f& b) {
            return std::abs(a.x()-b.x()) + std::abs(a.y()-b.y()) + std::abs(a.z()-b.z());
        };
        return d(p.up, up0) > 1e-6f || d(p.front, fr0) > 1e-6f;
    }

    // 面板几何：**调用 EditorApp 自己的函数**，不得在此重算
    // （重算 = 两份几何分叉 = 本 bug 的温床）。
    static float Left() { return EditorApp::PanelLeft(); }
    static float Top()  { return EditorApp::PanelTop(); }
    static float Sw()   { return 0.5f * kEditorWidth; }

    // 1. PointInPanel：面板内必命中、视口区必不命中。
    static void TestPointInPanel() {
        EditorApp app(JPOV::Config{});  // 不 Init，只测纯几何

        ExpectTrue(app.PointInPanel(Left() + Sw() * 0.5f, Top() + 15.0f),
                   "面板正中央的点必须判为在面板内");
        ExpectTrue(app.PointInPanel(Left() + Sw() * 0.5f,
                                    kEditorHeight - 2.0f),
                   "面板最后一行下部必须判为在面板内");
        ExpectTrue(!app.PointInPanel(kEditorWidth * 0.5f,
                                     kEditorHeight * 0.25f),
                   "3D 视口中央必须判为不在面板内（否则视口拖拽旋转会失效）");
        ExpectTrue(!app.PointInPanel(Left() + Sw() * 0.5f, Top() - 100.0f),
                   "面板上方 100px 必须判为不在面板内");
        ExpectTrue(!app.PointInPanel(Left() - 100.0f, Top() + 15.0f),
                   "面板左外 100px 必须判为不在面板内");
        ExpectTrue(!app.PointInPanel(Left() + Sw() + 100.0f, Top() + 15.0f),
                   "面板右外 100px 必须判为不在面板内");
        LOG(INFO) << "OK TestPointInPanel";
    }

    // 2. 几何自洽：PointInPanel 判定的纵向范围必须真的盖住所有 PanelRow()。
    //    （防"命中矩形"与"实际画的滑条行"两份几何分叉 —— 正是本 bug 的温床。）
    static void TestGeometryCoversAllRows() {
        EditorApp app(JPOV::Config{});
        for (int i = 0; i < EditorApp::kPanelRows; ++i) {
            const jpov::UiRect r = EditorApp::PanelRow(i);
            // 每行四角必须都判为"在面板内"，否则会出现"能画的滑条拖不动"。
            ExpectTrue(app.PointInPanel(r.pos.x(), r.pos.y()),
                       "每行左上角必须判为在面板内");
            ExpectTrue(app.PointInPanel(r.pos.x() + r.size.x(),
                                        r.pos.y() + r.size.y()),
                       "每行右下角必须判为在面板内");
        }
        LOG(INFO) << "OK TestGeometryCoversAllRows";
    }

    // 3. 组合语义：直接驱动私有 UpdateRotateFromDrag 模拟两条 drag 路径。
    static void TestDragOriginDecidesRotate() {
        // --- 路径 A：按在面板上（平移X 滑条位置）横向拖 → 旋转必须纹丝不动 ---
        {
            EditorApp app(JPOV::Config{});
            const jpov::Vec3f up0 = app.placement_.up;
            const jpov::Vec3f fr0 = app.placement_.front;
            jpov::InputSnapshot in{};
            in.left.raw = -1;  // Drag
            in.mouse_x = Left() + Sw() * 0.5f;
            in.mouse_y = Top() + 1.5f * 40.0f;   // ≈ 平移X 行
            in.mouse_dx = 0.0f;
            app.UpdateRotateFromDrag(in);
            jpov::InputSnapshot in2{};
            in2.left.raw = -1;
            in2.mouse_x = Left() + Sw() * 0.8f;
            in2.mouse_y = Top() + 1.5f * 40.0f;
            in2.mouse_dx = 120.0f;               // 横向拖 120px
            app.UpdateRotateFromDrag(in2);
            app.UpdateRotateFromDrag(in2);
            ExpectTrue(!OriChanged(app.placement_, up0, fr0),
                       "🔴 按在面板上拖动不得改变朝向（本次修复的核心）");
        }

        // --- 路径 B：按在 3D 视口里横向拖 → rx 必须变化（防修复阉掉功能）---
        {
            EditorApp app(JPOV::Config{});
            const jpov::Vec3f up0 = app.placement_.up;
            const jpov::Vec3f fr0 = app.placement_.front;
            jpov::InputSnapshot in{};
            in.left.raw = -1;
            in.mouse_x = kEditorWidth * 0.5f;
            in.mouse_y = kEditorHeight * 0.25f;  // 视口区
            in.mouse_dx = 0.0f;
            app.UpdateRotateFromDrag(in);
            jpov::InputSnapshot in2{};
            in2.left.raw = -1;
            in2.mouse_x = kEditorWidth * 0.5f + 320.0f;
            in2.mouse_y = kEditorHeight * 0.25f;
            in2.mouse_dx = 160.0f;
            app.UpdateRotateFromDrag(in2);
            app.UpdateRotateFromDrag(in2);
            ExpectTrue(OriChanged(app.placement_, up0, fr0),
                       "视口内横向拖必须改变朝向（否则修复把功能一起阉了）");
        }

        // --- 路径 C：归属在起点冻结（起点面板内，之后拖到视口区，仍不旋转）---
        {
            EditorApp app(JPOV::Config{});
            const jpov::Vec3f up0 = app.placement_.up;
            const jpov::Vec3f fr0 = app.placement_.front;
            jpov::InputSnapshot in{};
            in.left.raw = -1;
            in.mouse_x = Left() + Sw() * 0.5f;
            in.mouse_y = Top() + 15.0f;          // 起点：面板内
            in.mouse_dx = 0.0f;
            app.UpdateRotateFromDrag(in);
            jpov::InputSnapshot in2{};
            in2.left.raw = -1;
            in2.mouse_x = kEditorWidth * 0.5f;   // 之后拖到视口区
            in2.mouse_y = kEditorHeight * 0.25f;
            in2.mouse_dx = 200.0f;
            app.UpdateRotateFromDrag(in2);
            ExpectTrue(!OriChanged(app.placement_, up0, fr0),
                       "归属应在 drag 起点冻结：起点在面板上则整段按住都不旋转");
        }

        // --- 路径 D：先在面板上 Hold（原地不动），再移到视口区拖动 ---
        // 这是"归属必须在按下那一帧就记下"的关键回归：若等到 IsDrag 转真才判，
        // 那时鼠标已在视口区，会把起于面板的拖动误判为起于视口。
        {
            EditorApp app(JPOV::Config{});
            const jpov::Vec3f up0 = app.placement_.up;
            const jpov::Vec3f fr0 = app.placement_.front;
            jpov::InputSnapshot hold{};
            hold.left.raw = -2;                  // Hold（按下未动）
            hold.mouse_x = Left() + Sw() * 0.5f; // 按下点：面板内
            hold.mouse_y = Top() + 15.0f;
            app.UpdateRotateFromDrag(hold);
            // 下一帧开始移动 → IsDrag 转真，但坐标已飘到视口区。
            jpov::InputSnapshot drag{};
            drag.left.raw = -1;                  // Drag
            drag.mouse_x = kEditorWidth * 0.5f;  // 已到视口区
            drag.mouse_y = kEditorHeight * 0.25f;
            drag.mouse_dx = 150.0f;
            app.UpdateRotateFromDrag(drag);
            app.UpdateRotateFromDrag(drag);
            ExpectTrue(!OriChanged(app.placement_, up0, fr0),
                       "Hold 后拖走：归属应取按下点（面板内）→ 不得旋转");
        }

        // --- 路径 E：反向 —— 在视口区 Hold，再移到面板上拖动 → 仍应旋转 ---
        {
            EditorApp app(JPOV::Config{});
            const jpov::Vec3f up0 = app.placement_.up;
            const jpov::Vec3f fr0 = app.placement_.front;
            jpov::InputSnapshot hold{};
            hold.left.raw = -2;                  // Hold，按下点在视口区
            hold.mouse_x = kEditorWidth * 0.5f;
            hold.mouse_y = kEditorHeight * 0.25f;
            app.UpdateRotateFromDrag(hold);
            jpov::InputSnapshot drag{};
            drag.left.raw = -1;                  // Drag（已飘到面板上方）
            drag.mouse_x = Left() + Sw() * 0.5f;
            drag.mouse_y = Top() + 15.0f;
            drag.mouse_dx = 150.0f;
            app.UpdateRotateFromDrag(drag);
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
