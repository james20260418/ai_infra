// JPOV skeleton gold generator — 蒙皮 T-rest 蓝人 + object3d 对照（M1）
//
// 与 test(jpov_skeleton_gold_test) 共用 jpov_skeleton_gold_common.h 场景，
// 渲同一帧到仓库 gold PNG。验证蒙皮链路(bind pose 真蒙皮左人) ≈ object3d 直画(右人)。
#include <cstdio>
#include <string>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/test/skeleton/jpov_skeleton_gold_common.h"
#include "tools/jpov/test/test_utils.h"

static std::string TestDataDir() {
    const char* e = std::getenv("TEST_SRCDIR");
    if (e) { std::string s = e; if (!s.empty() && s.back()!='/') s.push_back('/');
             return s + "__main__/tools/jpov/test"; }
    return jpov::GetTestDataDir();
}
static std::string Assets() { return jpov::GetProjectRoot() + "tools/jpov/test/object3d/scene_assets/"; }
static std::string Male() { return jpov::GetProjectRoot() + "tools/jpov/test/object3d/mixamo_male/mixamo_male.glb"; }

int main() {
    const std::string outpath = TestDataDir() + jpov_skeleton_gold::GetGoldRelPath();

    JPOV::Config cfg;
    cfg.title = "JPOV Skeleton Gold (T-rest)";
    cfg.headless = true;
    jpov_skeleton_gold::SkeletonGoldApp app(cfg);
    app.Init();
    jpov_skeleton_gold::BuildScene(&app, Assets(), Male());

    jpov::WindowInfo winfo;
    winfo.width = static_cast<float>(jpov_skeleton_gold::kOutW);
    winfo.height = static_cast<float>(jpov_skeleton_gold::kOutH);
    jpov::InputSnapshot input{};
    app.RunOnce(input, winfo, outpath.c_str());
    app.Finalize();
    LOG(INFO) << "skeleton gold generated: " << outpath;
    return 0;
}
