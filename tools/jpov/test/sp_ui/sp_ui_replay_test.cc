// JPOV sp_ui 多帧回放测试（GPU，CI 上跑 llvmpipe）—— 单帧仿真的端到端样板
//
// 这个测试要证明的是一条**能力**，外加两道回归：
//   能力：`OneIteration` 收纯数据 ⇒ 伪造 InputSnapshot + 逐帧 RunOnce 就能把一段
//         交互（悬停→按下→松开→拖拽）在无人值守下跑出来，而且能逐帧验像素。
//   回归 1：悬停态必须比常态**亮**、按下态必须比常态**暗**（三态配色没被写反/写丢）。
//   回归 2：伪造的 Click 必须真的进业务逻辑（回调置位）、伪造的 Drag 必须真的改值。
//
// 为什么断言用「ROI 均值 + 相对大小」而不是逐字节 gold：
//   llvmpipe 对同一输入是确定的，但一旦换驱动/改配色，逐字节 gold 必然红且原因难读。
//   相对关系（亮/暗、变了多少）对这类状态机是更稳的判据 —— 仓库里
//   `test/compare_light.h` 也是这个思路。
//
// 帧序列（每帧一张 PNG，落在 output/jpov_sp_ui_replay_test/）：
//   f0 鼠标在外   → 常态
//   f1 悬停按钮   → 悬停态（更亮）
//   f2 按住按钮   → 按下态（更暗），Hold
//   f3 松开       → Click ⇒ 业务回调置位
//   f4 在滑条框内按下并拖动 → 值增大（drag 起始必须在框内）
//   f5 继续拖、且 y 飘出 box 竖直范围 → 值继续增大（一次 drag 语义）

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include <glog/logging.h>

#include "third_party/stb/stb_image.h"
#include "tools/common/utils.h"
#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/sp_ui/sp_ui.h"

namespace {

// 场景尺寸与控件位置（像素，布局坐标；scale=1）。
constexpr float kWidth = 320.0f;
constexpr float kHeight = 240.0f;
constexpr jpov::sp_ui::SpRect kButton = {{20.0f, 20.0f}, {140.0f, 40.0f}};
constexpr jpov::sp_ui::SpRect kSlider = {{20.0f, 100.0f}, {240.0f, 30.0f}};

// 三态配色：亮度差异必须足够大，才能用 ROI 均值判定「变亮/变暗」。
constexpr float kBase = 0.25f;     // 常态
constexpr float kHover = 0.60f;    // 悬停
constexpr float kPressed = 0.10f;  // 按下

class ReplayApp : public JPOV {
public:
    using JPOV::JPOV;

    bool clicked() const { return clicked_; }
    float slider() const { return slider_; }

    void OneIteration(int64_t frame_count, const jpov::InputSnapshot& input,
                      const jpov::WindowInfo& winfo,
                      jpov::RenderCommandList* cmds) override {
        (void)frame_count;
        (void)winfo;

        jpov::sp_ui::SpTheme theme =
            jpov::sp_ui::SpTheme::Default(16.0f);
        theme.background = {0.05f, 0.05f, 0.05f, 1.0f};
        theme.foreground = {1.0f, 1.0f, 1.0f, 1.0f};
        theme.hover = {kBase, kBase, kBase, 1.0f};
        theme.accent = {kHover, kHover, kHover, 1.0f};
        theme.selected = {kPressed, kPressed, kPressed, 1.0f};
        // 纯色板材：不引入材质变量，让 ROI 均值只反映按钮三态色。
        theme.plate_texture_id = 0;

        ui_.Begin(input, theme, kWidth, kHeight, /*frame_dt_ms=*/1000.0f / 30.0f,
                  /*time_seconds=*/0.0f);
        if (ui_.Button("click", kButton)) {
            clicked_ = true;
            ++hits_;
        }
        ui_.SliderFloat("drag", &slider_, kSlider, 0.0f, 100.0f);
        ui_.End();
        ui_.Emit(cmds);
    }

private:
    jpov::sp_ui::SpUi ui_;
    bool clicked_ = false;
    int hits_ = 0;
    float slider_ = 0.0f;
};

jpov::InputSnapshot Idle(float x, float y) {
    jpov::InputSnapshot in{};
    in.mouse_x = x;
    in.mouse_y = y;
    return in;
}
jpov::InputSnapshot Hold(float x, float y) {
    jpov::InputSnapshot in = Idle(x, y);
    in.left.raw = -2;   // Hold（按下未移动）
    return in;
}
jpov::InputSnapshot Click(float x, float y) {
    jpov::InputSnapshot in = Idle(x, y);
    in.left.raw = 1;    // 本帧 1 次 Click
    in.left_clicks[0] = {x, y, 0.5f};
    return in;
}
jpov::InputSnapshot Drag(float x, float y) {
    jpov::InputSnapshot in = Idle(x, y);
    in.left.raw = -1;   // Drag（按下且移动过）
    return in;
}

// 读一张 PNG，返回某矩形内的 RGB 均值亮度（0..255）。
double RoiLuma(const std::string& path, int x0, int y0, int x1, int y1) {
    int w = 0;
    int h = 0;
    int comp = 0;
    unsigned char* img = stbi_load(path.c_str(), &w, &h, &comp, 4);
    CHECK(img != nullptr) << "读不回渲染结果：" << path;
    CHECK(x1 <= w && y1 <= h) << "ROI 越界：" << path;
    double sum = 0.0;
    long long n = 0;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const unsigned char* p = &img[(static_cast<size_t>(y) * w + x) * 4];
            sum += 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2];
            ++n;
        }
    }
    stbi_image_free(img);
    CHECK_GT(n, 0);
    return sum / static_cast<double>(n);
}

}  // namespace

int main() {
    const std::string dir = jpov::GetOutputDir() + "jpov_sp_ui_replay_test/";
    const std::string mk = "mkdir -p '" + dir + "'";
    CHECK_EQ(std::system(mk.c_str()), 0) << "创建输出目录失败";

    JPOV::Config cfg;
    cfg.title = "sp_ui replay test";
    cfg.headless = true;
    ReplayApp app(cfg);
    app.Init();

    jpov::WindowInfo winfo;
    winfo.width = kWidth;
    winfo.height = kHeight;

    // 按钮中心（用于伪造鼠标位置）
    const float bx = kButton.pos.x() + kButton.size.x() * 0.5f;
    const float by = kButton.pos.y() + kButton.size.y() * 0.5f;

    struct Step {
        const char* name;
        jpov::InputSnapshot in;
    };
    const std::vector<Step> steps = {
        {"f0_idle", Idle(kWidth - 10.0f, kHeight - 10.0f)},
        {"f1_hover", Idle(bx, by)},
        {"f2_press", Hold(bx, by)},
        {"f3_click", Click(bx, by)},
        // drag 起始点在滑条框**内**（x 靠右、y 在框内）。
        {"f4_drag_start", Drag(kSlider.pos.x() + 180.0f, kSlider.pos.y() + 15.0f)},
        // 继续拖：x 更靠右，y 已经飘到 box 竖直范围之外。
        {"f5_drag_out", Drag(kSlider.pos.x() + kSlider.size.x() - 5.0f, 235.0f)},
    };

    std::vector<std::string> paths;
    float slider_after_click = 0.0f;
    float slider_after_drag_start = 0.0f;
    for (size_t i = 0; i < steps.size(); ++i) {
        // 关键：伪造快照 + 带帧号 RunOnce ⇒ OneIteration 里拿到真实帧号。
        const std::string path = dir + steps[i].name + ".png";
        app.RunOnce(static_cast<int64_t>(i), steps[i].in, winfo, path.c_str());
        paths.push_back(path);
        if (i == 3) {
            slider_after_click = app.slider();
        }
        if (i == 4) {
            slider_after_drag_start = app.slider();
        }
    }
    const bool clicked = app.clicked();
    const float slider_final = app.slider();
    app.Finalize();

    // ---- 回归 2：伪造的输入真的进了业务逻辑 ----
    CHECK(clicked) << "伪造的 Click 没有触发按钮回调 —— 点击链路断了";
    CHECK(slider_after_drag_start > slider_after_click + 5.0f)
        << "伪造的 Drag（起点在框内）没有改滑条值：" << slider_after_click
        << " → " << slider_after_drag_start;
    CHECK(slider_final > slider_after_drag_start + 5.0f)
        << "drag 已经开始后，鼠标飘出 box 竖直范围应继续写值："
        << slider_after_drag_start << " → " << slider_final;
    LOG(INFO) << "[PASS] 伪造输入进业务逻辑：clicked=" << clicked
              << " slider " << slider_after_click << " → "
              << slider_after_drag_start << " → " << slider_final;

    // ---- 回归 1：三态亮度关系（ROI = 按钮内部，避开边框）----
    const int rx0 = static_cast<int>(kButton.pos.x()) + 6;
    const int ry0 = static_cast<int>(kButton.pos.y()) + 6;
    const int rx1 = static_cast<int>(kButton.pos.x() + kButton.size.x()) - 6;
    const int ry1 = static_cast<int>(kButton.pos.y() + kButton.size.y()) - 6;
    const double luma_idle = RoiLuma(paths[0], rx0, ry0, rx1, ry1);
    const double luma_hover = RoiLuma(paths[1], rx0, ry0, rx1, ry1);
    const double luma_press = RoiLuma(paths[2], rx0, ry0, rx1, ry1);
    LOG(INFO) << "按钮 ROI 亮度：常态=" << luma_idle << " 悬停=" << luma_hover
              << " 按下=" << luma_press;

    CHECK(luma_idle > 5.0) << "常态下按钮没画出来（亮度 " << luma_idle << "）";
    CHECK(luma_hover > luma_idle + 20.0)
        << "悬停态应明显更亮：常态 " << luma_idle << " → 悬停 " << luma_hover;
    CHECK(luma_press < luma_idle - 5.0)
        << "按下态应更暗：常态 " << luma_idle << " → 按下 " << luma_press;
    LOG(INFO) << "[PASS] 三态亮度：按下 < 常态 < 悬停";

    LOG(INFO) << "===== sp_ui 多帧回放测试通过（5 帧，逐帧 PNG 在 "
              << dir << "）=====";
    return 0;
}
