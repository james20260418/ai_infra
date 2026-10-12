// JPOV 植被查看器（vegetation viewer）— 主程序。
//
// 用途：肉眼验收 **L-System 生成的植被拓扑骨架**（MVP：每根枝画成一根空间棒棒）。
// 树种为内置「龙爪槐」。视角变换与 model/skylight viewer 相同（右键拖拽/滚轮缩放）。
//
// 交互面板 4 个 knob（改动即重建骨架，确定性）：展开代数 / 长度缩放 / 基本粗细 / 随机种子。
//
// headless 拍摄（--capture <out_dir>）：输出一组「生长扫谱」图（代数递增）+ 一张成品
// 三视角，供交付验收 / 自动化核对。
//
// 架构：与 skylight viewer 同构（主程序只做装配 + 事件循环；渲染核心 + 面板在
// vegetation_viewer_app.h）。
//
// 编译运行（Linux，需 DISPLAY/WSLg）：
//   ./tools/jpov/build_jpov_vegetation_viewer.sh
//   → output/jpov_vegetation_viewer/jpov_vegetation_viewer

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/demo/vegetation_viewer_app.h"
#include "tools/jpov/demo/view_config.h"

namespace {

using jpov_vegetation::VegetationApp;

constexpr int kWidth  = jpov_vegetation::kViewerWidth;
constexpr int kHeight = jpov_vegetation::kViewerHeight;
constexpr double kPi  = 3.14159265358979323846;

// 交互窗口 / 拍摄共用的字体声明（路径相对 exe 的 fonts/，build 脚本同构拷贝）。
std::vector<JPOV::FontEntry> ViewerFonts() {
    return {
        {"fonts/NotoSansCJK-Regular.ttc", 0, jpov::kFontBuiltinCJK},
        {"fonts/DejaVuSans.ttf",          0, jpov::kFontBuiltinLatin},
    };
}

// headless 拍摄：无窗口批量出图。
int RunCapture(const std::string& out_dir) {
    JPOV::Config cfg;
    cfg.title    = "JPOV — 植被查看器（headless capture）";
    cfg.width    = kWidth;
    cfg.height   = kHeight;
    cfg.headless = true;
    cfg.fonts    = ViewerFonts();

    VegetationApp app(cfg);
    app.SetShowPanel(false);   // 纯 3D 截图
    app.Init();
    app.InstallScene();

    jpov::WindowInfo winfo;
    winfo.width  = kWidth;
    winfo.height = kHeight;
    const jpov::InputSnapshot input{};

    auto shoot = [&](const std::string& name) {
        const std::string path = out_dir + "/" + name + ".png";
        app.RunOnce(input, winfo, path.c_str());
        LOG(INFO) << "capture: " << path;
    };

    // ── A. 生长扫谱：代数递增，看骨架从主干长成完整树冠 ──
    app.view_ = jpov_viewer::DefaultView();
    app.view_.phi   = 0.12;
    app.view_.theta = kPi / 4.0;
    app.view_.R     = 8.0;
    for (int gen : {2, 5, 8, 12, 16, 20, 25}) {
        app.generations_ = gen;
        app.RegenerateNow();
        shoot("growth_gen" + std::to_string(gen));
    }

    // ── B. 成品三视角（代数 25）──
    app.generations_ = 25;
    app.RegenerateNow();
    struct ViewAngle { const char* name; float phi; float theta; float R; };
    const ViewAngle kAngles[] = {
        {"hero",   0.12f, 0.7853982f, 8.0f},
        {"front", 0.05f, 0.0f,        8.0f},
        {"side",  0.05f, 1.5707963f, 8.0f},
        {"closeup", 0.05f, 0.7853982f, 4.0f},
        {"top",   1.30f, 0.7853982f, 8.0f},
    };
    for (const ViewAngle& va : kAngles) {
        app.view_.phi   = va.phi;
        app.view_.theta = va.theta;
        app.view_.R     = va.R;
        shoot(std::string("dragon_claw_") + va.name);
    }

    // ── C. 叶片生长扫谱（代数 25，叶大小随 growth 张开；同机位）──
    app.view_.phi   = 0.12f;
    app.view_.theta = kPi / 4.0;
    app.view_.R     = 8.0f;
    for (float g : {0.15f, 0.4f, 0.7f, 1.0f}) {
        app.leaf_growth_ = g;
        app.RegenerateNow();
        shoot("leafgrowth_" + std::to_string(static_cast<int>(g * 100)));
    }

    app.Finalize();
    return 0;
}

// 预实验：把几个内置树型的「纯拓扑」各出一张 3/4 视图（棒棒、无叶），
// 用来对照「现有 L-System 拓扑能长成啥样」。
int RunTopologyCapture(const std::string& out_dir) {
    JPOV::Config cfg;
    cfg.title    = "JPOV — 植被预实验（拓扑）";
    cfg.width    = kWidth;
    cfg.height   = kHeight;
    cfg.headless = true;
    cfg.fonts    = ViewerFonts();

    VegetationApp app(cfg);
    app.SetShowPanel(false);
    app.use_tube_    = false;   // 纯拓扑：棒棒
    app.show_leaves_ = false;   // 不要叶
    app.Init();
    app.InstallScene();

    jpov::WindowInfo winfo;
    winfo.width  = kWidth;
    winfo.height = kHeight;
    const jpov::InputSnapshot input{};

    const char* kTrees[] = {"dragonclaw", "recursive", "recursive_stochastic"};
    for (const char* name : kTrees) {
        app.SelectTree(name);
        app.tropism_ = 0.0f;   // 纯拓扑：关向性
        app.RegenerateNow();
        app.view_ = jpov_viewer::DefaultView();
        app.view_.phi   = 0.12;
        app.view_.theta = kPi / 4.0;
        app.view_.R     = 9.0;
        const std::string path = out_dir + "/topo_" + name + ".png";
        app.RunOnce(input, winfo, path.c_str());
        LOG(INFO) << "topo capture: " << path;
    }
    app.Finalize();
    return 0;
}

// 预实验（验收三件套）：① 递归自相似 ② 向性 ③ 随机差异。
// 用现有引擎，仅改规则与向性参数，出一组对照图（管状、无叶）。
int RunIngredientStudy(const std::string& out_dir) {
    JPOV::Config cfg;
    cfg.title    = "JPOV — 植被预实验（三件套）";
    cfg.width    = kWidth;
    cfg.height   = kHeight;
    cfg.headless = true;
    cfg.fonts    = ViewerFonts();

    VegetationApp app(cfg);
    app.SetShowPanel(false);
    app.use_tube_    = true;    // 管状枝干（比棒棒好看）
    app.show_leaves_ = false;   // 纯拓扑：不要叶
    app.Init();
    app.InstallScene();

    jpov::WindowInfo winfo;
    winfo.width  = kWidth;
    winfo.height = kHeight;
    const jpov::InputSnapshot input{};

    struct Scenario { const char* label; const char* tree; float tropism; int seed; };
    const Scenario kScenarios[] = {
        {"1_recursive",         "recursive",            0.00f, 0},
        {"2_recursive+tropism", "recursive",            0.30f, 0},
        {"3_stoch_seed1",       "recursive_stochastic", 0.00f, 1},
        {"4_stoch_seed2",       "recursive_stochastic", 0.00f, 2},
        {"5_stoch+tropism",     "recursive_stochastic", 0.30f, 1},
    };
    for (const Scenario& sc : kScenarios) {
        app.SelectTree(sc.tree);
        app.tropism_ = sc.tropism;
        app.seed_    = sc.seed;
        app.RegenerateNow();
        app.view_ = jpov_viewer::DefaultView();
        app.view_.phi   = 0.12f;
        app.view_.theta = kPi / 4.0;
        app.view_.R     = 9.0f;
        const std::string path = out_dir + "/ing_" + sc.label + ".png";
        app.RunOnce(input, winfo, path.c_str());
        LOG(INFO) << "ingredient capture: " << path;
    }
    app.Finalize();
    return 0;
}

// 生长扫谱（预实验用）：对指定树型 + 向性 + seed，按「展开代数」递增出图，
// 让 Danis 在固定机位下看骨架从主干长成完整冠。
int RunGrowthCapture(const std::string& out_dir, const std::string& tree,
                     float tropism, int seed) {
    JPOV::Config cfg;
    cfg.title    = "JPOV — 植被生长扫谱（预实验）";
    cfg.width    = kWidth;
    cfg.height   = kHeight;
    cfg.headless = true;
    cfg.fonts    = ViewerFonts();

    VegetationApp app(cfg);
    app.SetShowPanel(false);
    app.use_tube_    = true;    // 管状枝干
    app.show_leaves_ = false;   // 纯拓扑生长（不纠结叶/纹理）
    app.Init();
    app.InstallScene();
    app.SelectTree(tree);
    if (tropism > 0.0f) {
        app.tropism_ = tropism;   // 未显式给则用 SelectTree 默认（recursive 系列 = 0.5）
    }
    app.seed_ = seed;

    jpov::WindowInfo winfo;
    winfo.width  = kWidth;
    winfo.height = kHeight;
    const jpov::InputSnapshot input{};

    app.view_ = jpov_viewer::DefaultView();
    app.view_.phi   = 0.12f;
    app.view_.theta = kPi / 4.0;
    app.view_.R     = 9.0f;

    const bool recursive = tree.rfind("recursive", 0) == 0;
    static const int kGensRecursive[] = {1, 2, 3, 4, 6, 8, 11, 14, 16};
    static const int kGensDefault[]   = {2, 5, 8, 12, 16, 20, 25};
    const int* kGens = recursive ? kGensRecursive : kGensDefault;
    const int  n     = recursive ? 9 : 7;
    for (int i = 0; i < n; ++i) {
        app.generations_ = kGens[i];
        app.RegenerateNow();
        const std::string path =
            out_dir + "/grow_" + tree + "_gen" + std::to_string(kGens[i]) + ".png";
        app.RunOnce(input, winfo, path.c_str());
        LOG(INFO) << "growth capture: " << path;
    }
    // 成品大图（同机位，最大代数）。
    app.generations_ = kGens[n - 1];
    app.RegenerateNow();
    struct ViewAngle { const char* name; float phi; float theta; float R; };
    const ViewAngle kFinal[] = {
        {"final",  0.12f, 0.7853982f, 6.0f},
        {"front",  0.05f, 0.0f,        6.0f},
        {"side",   0.05f, 1.5707963f,  6.0f},
        {"top",    1.35f, 0.7853982f,  6.0f},
    };
    for (const ViewAngle& va : kFinal) {
        app.view_.phi   = va.phi;
        app.view_.theta = va.theta;
        app.view_.R     = va.R;
        app.RunOnce(input, winfo,
                    (out_dir + "/grow_" + tree + "_" + va.name + ".png").c_str());
    }
    // 分叉特写（抬高注视点 + 拉近）——看领口缝合。
    app.view_.phi = 0.12f;
    app.view_.theta = 0.7853982f;
    app.view_.R = 1.1f;
    for (float ay : {0.8f, 1.3f, 1.8f}) {
        app.aim_y_ = ay;
        app.RunOnce(input, winfo,
                    (out_dir + "/macro_" + tree + "_y" +
                     std::to_string(static_cast<int>(ay * 100)) + ".png").c_str());
    }
    app.aim_y_ = 0.0f;
    app.Finalize();
    return 0;
}

// 分叉缝合的单点对照：粗干 + 一条侧枝，分别用 junction_blend = 0 / 1 / 2.5 近拍。
int RunJunctionStudy(const std::string& out_dir) {
    JPOV::Config cfg;
    cfg.title = "JPOV — 分叉缝合（junction）";
    cfg.width = kWidth;
    cfg.height = kHeight;
    cfg.headless = true;
    cfg.fonts = ViewerFonts();
    VegetationApp app(cfg);
    app.SetShowPanel(false);
    app.use_tube_ = true;
    app.show_leaves_ = false;
    app.Init();
    app.InstallScene();
    app.SelectTree("junction");
    jpov::WindowInfo winfo;
    winfo.width = kWidth;
    winfo.height = kHeight;
    const jpov::InputSnapshot input{};
    struct Blend { float v; const char* tag; };
    const Blend kBlends[] = {{0.0f, "off"}, {2.0f, "b2"}, {4.0f, "b4"}};
    for (const Blend& bl : kBlends) {
        app.junction_blend_ = bl.v;
        app.RegenerateNow();
        app.view_ = jpov_viewer::DefaultView();
        app.view_.phi = 0.10f;
        app.view_.theta = 0.7853982f;
        app.view_.R = 1.3f;
        app.aim_y_ = 2.0f;   // 侧枝起点（干中点 = F(10)=2m）
        app.RunOnce(input, winfo,
                    (out_dir + "/junction_" + bl.tag + ".png").c_str());
    }
    app.Finalize();
    return 0;
}

// 「花园」：杉/松/龙爪槐/灌木 四棵并排出图。
int RunGardenCapture(const std::string& out_dir) {
    JPOV::Config cfg;
    cfg.title = "JPOV — 预实验花园（4 棵）";
    cfg.width = kWidth;
    cfg.height = kHeight;
    cfg.headless = true;
    cfg.fonts = ViewerFonts();
    VegetationApp app(cfg);
    app.SetShowPanel(false);
    app.use_tube_ = true;
    app.show_leaves_ = true;
    app.Init();
    app.InstallScene();
    app.SelectGarden();
    jpov::WindowInfo winfo;
    winfo.width = kWidth;
    winfo.height = kHeight;
    const jpov::InputSnapshot input{};
    app.aim_y_ = 1.6f;
    struct V { const char* name; float phi; float theta; float R; };
    const V kViews[] = {
        {"hero",  0.14f, 0.7853982f, 12.0f},
        {"front", 0.10f, 0.0f,        12.0f},
    };
    for (const V& v : kViews) {
        app.view_ = jpov_viewer::DefaultView();
        app.view_.phi = v.phi;
        app.view_.theta = v.theta;
        app.view_.R = v.R;
        app.RunOnce(input, winfo,
                    (out_dir + "/garden_" + v.name + ".png").c_str());
    }
    app.Finalize();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::string out_dir;
    std::string topo_dir;
    std::string study_dir;
    std::string growth_dir;
    std::string junction_dir;
    std::string garden_dir;
    std::string tree = "dragonclaw";
    float tropism = 0.0f;
    int   seed    = 0;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--capture") == 0 && i + 1 < argc) {
            out_dir = argv[i + 1];
        } else if (std::strcmp(argv[i], "--topo") == 0 && i + 1 < argc) {
            topo_dir = argv[i + 1];
        } else if (std::strcmp(argv[i], "--study") == 0 && i + 1 < argc) {
            study_dir = argv[i + 1];
        } else if (std::strcmp(argv[i], "--growth") == 0 && i + 1 < argc) {
            growth_dir = argv[i + 1];
        } else if (std::strcmp(argv[i], "--junction") == 0 && i + 1 < argc) {
            junction_dir = argv[i + 1];
        } else if (std::strcmp(argv[i], "--garden") == 0 && i + 1 < argc) {
            garden_dir = argv[i + 1];
        } else if (std::strcmp(argv[i], "--tree") == 0 && i + 1 < argc) {
            tree = argv[i + 1];
        } else if (std::strcmp(argv[i], "--tropism") == 0 && i + 1 < argc) {
            tropism = std::strtof(argv[i + 1], nullptr);
        } else if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            seed = std::atoi(argv[i + 1]);
        }
    }
    if (!garden_dir.empty()) {
        return RunGardenCapture(garden_dir);
    }
    if (!junction_dir.empty()) {
        return RunJunctionStudy(junction_dir);
    }
    if (!study_dir.empty()) {
        return RunIngredientStudy(study_dir);
    }
    if (!growth_dir.empty()) {
        return RunGrowthCapture(growth_dir, tree, tropism, seed);
    }
    if (!topo_dir.empty()) {
        return RunTopologyCapture(topo_dir);
    }
    if (!out_dir.empty()) {
        return RunCapture(out_dir);
    }

    // ── 交互模式 ──
    JPOV::Config cfg;
    cfg.title      = "JPOV — 植被查看器（L-System 龙爪槐）";
    cfg.width      = kWidth;
    cfg.height     = kHeight;
    cfg.resizable  = false;
    cfg.target_fps = static_cast<int>(jpov_vegetation::kViewerFps);
    cfg.headless   = false;
    cfg.fonts      = ViewerFonts();

    VegetationApp app(cfg);
    app.SetShowPanel(true);
    app.Init();
    app.InstallTextMeasure();
    app.InstallScene();

    // --tree/--tropism/--seed 也作用于交互模式（便于直接拖「展开代数」滑条看生长）。
    if (tree != "dragonclaw") {
        app.SelectTree(tree);
    }
    if (tropism > 0.0f) {
        app.tropism_ = tropism;
    }
    app.seed_ = seed;
    if (tree != "garden") {
        app.RegenerateNow();   // garden 已在 SelectTree 里建好，不能再重建
    }

    app.view_ = jpov_viewer::DefaultView();
    app.view_.phi   = 0.12;
    app.view_.theta = kPi / 4.0;
    app.view_.R     = 8.0;

    app.Run();
    app.Finalize();
    return 0;
}
