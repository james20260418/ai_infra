// JPOV 透明绘制 gold image generator —— 渲染「透明盒前后遮挡」场景并写仓库内 gold：
//   tools/jpov/test/object3d/transparent_1280x720.png
//
// 输出 640x360（同其它 gold 惯例，文件名 1280x720 仅为分辨率标记）。

#include <string>

#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/test/object3d/jpov_transparent_gold_common.h"
#include "tools/jpov/test/test_utils.h"

int main() {
    const std::string outpath =
        jpov::GetTestDataDir() + "/object3d/transparent_1280x720.png";

    JPOV::Config cfg;
    cfg.title = "JPOV Transparent Gold Generator";
    cfg.headless = true;
    jpov_transparent::TransparentApp app(cfg);
    app.Init();

    app.ground_mesh_ = app.RegisterMesh(
        jpov::MeshData::MakeBox(/*front*/ 4.0f, /*up*/ 0.1f, /*left*/ 4.0f));
    app.glass_mesh_ = app.RegisterMesh(
        jpov::MeshData::MakeBox(/*front*/ 1.0f, /*up*/ 1.0f, /*left*/ 1.0f));
    app.solid_mesh_ = app.RegisterMesh(
        jpov::MeshData::MakeBox(/*front*/ 0.4f, /*up*/ 0.4f, /*left*/ 0.4f));

    jpov::WindowInfo winfo;
    winfo.width = 640.0f;
    winfo.height = 360.0f;
    jpov::InputSnapshot input{};
    app.RunOnce(input, winfo, outpath.c_str());
    app.Finalize();

    LOG(INFO) << "transparent gold image generated: " << outpath;
    return 0;
}
