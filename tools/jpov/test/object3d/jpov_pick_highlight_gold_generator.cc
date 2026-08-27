// JPOV picking + highlight gold image generator
//
// 渲染“中心柱高亮 + 全场景”的定格画面，写仓库内 gold image 供肉眼参考：
//   tools/jpov/test/object3d/pick_highlight_1280x720.png
//
// 与 test 共用 jpov_pick_highlight_gold_common.h，保证场景几何一致。

#include <string>

#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/test/object3d/jpov_pick_highlight_gold_common.h"
#include "tools/jpov/test/test_utils.h"

int main() {
    const std::string outpath =
        jpov::GetTestDataDir() + "/object3d/pick_highlight_1280x720.png";

    JPOV::Config cfg;
    cfg.title = "JPOV PickHighlight Gold Generator";
    cfg.headless = true;
    jpov_pick_highlight::PickHighlightApp app(cfg);
    app.Init();

    app.ground_mesh_ = app.RegisterMesh(jpov::MeshData::MakeBox(3.0f, 0.1f, 3.0f));
    app.center_mesh_ = app.RegisterMesh(jpov::MeshData::MakeBox(0.6f, 1.0f, 0.6f));
    app.left_mesh_   = app.RegisterMesh(jpov::MeshData::MakeBox(0.5f, 1.0f, 0.5f));
    app.right_mesh_  = app.RegisterMesh(jpov::MeshData::MakeBox(0.5f, 1.0f, 0.5f));

    // 高亮中心柱（金色边框），供肉眼参考。
    app.highlight_id = jpov_pick_highlight::kIdCenter;
    app.pick_enabled = false;

    jpov::WindowInfo winfo;
    winfo.width  = 1280.0f;
    winfo.height = 720.0f;
    jpov::InputSnapshot input{};
    app.RunOnce(input, winfo, outpath.c_str());
    app.Finalize();

    LOG(INFO) << "pick+highlight gold image generated: " << outpath;
    return 0;
}
