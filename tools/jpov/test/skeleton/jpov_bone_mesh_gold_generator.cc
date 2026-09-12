// JPOV 火柴人 gold generator —— 写仓库 gold image
//
// 与 test(jpov_bone_mesh_gold_test) 共用 jpov_bone_mesh_gold_common.h 场景，
// 渲同一帧到仓库 gold PNG：标准晴天布景（地面 + 天光天色）+ identity 位姿红火柴人。
#include <cstdio>
#include <string>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/test/skeleton/jpov_bone_mesh_gold_common.h"
#include "tools/jpov/test/test_utils.h"

static std::string TestDataDir() {
    const char* e = std::getenv("TEST_SRCDIR");
    if (e) {
        std::string s = e;
        if (!s.empty() && s.back() != '/') s.push_back('/');
        return s + "__main__/tools/jpov/test";
    }
    return jpov::GetTestDataDir();
}

static std::string Assets() {
    return jpov::GetProjectRoot() + "tools/jpov/test/object3d/scene_assets/";
}

int main() {
    const std::string outpath =
        TestDataDir() + jpov_bone_mesh_gold::GetGoldRelPath();
    const std::string nobone_path =
        TestDataDir() + jpov_bone_mesh_gold::GetNoBoneRelPath();

    JPOV::Config cfg;
    cfg.title = "JPOV Bone Mesh Gold (stickman)";
    cfg.headless = true;
    jpov_bone_mesh_gold::BoneMeshGoldApp app(cfg);
    app.Init();
    jpov_bone_mesh_gold::BuildScene(&app,
                                    Assets() + "ground/ground_brick.gltf",
                                    Assets() + "ground/ground_dirt.gltf");

    jpov::WindowInfo winfo;
    winfo.width  = 640.0f;
    winfo.height = 360.0f;
    jpov::InputSnapshot input{};

    // 1) 正常帧（含火柴人）
    app.draw_stickman_ = true;
    app.RunOnce(input, winfo, outpath.c_str());
    LOG(INFO) << "bone mesh gold generated: " << outpath;

    // 2) “无火柴人”基线帧（同场景，只是不画火柴人）——供 test 做可见性门禁
    app.draw_stickman_ = false;
    app.RunOnce(input, winfo, nobone_path.c_str());
    LOG(INFO) << "bone mesh no-bone baseline generated: " << nobone_path;

    app.Finalize();
    return 0;
}
