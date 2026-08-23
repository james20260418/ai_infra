// JPOV box gold image generator —— 验证 MeshData::MakeBox + PBRMaterial 纯色构造
//
// 渲染 box 场景（地面 + 立柱 + 斜柱）并写仓库内 gold image：
//   tools/jpov/test/object3d/box_1280x720.png
//
// 输出: 640x360（同其它 gold 惯例，文件名 1280x720 仅为分辨率标记）。

#include <string>

#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/test/object3d/jpov_box_gold_common.h"
#include "tools/jpov/test/test_utils.h"

int main() {
    const std::string outpath =
        jpov::GetTestDataDir() + "/object3d/box_1280x720.png";

    JPOV::Config cfg;
    cfg.title = "JPOV Box Gold Generator";
    cfg.headless = true;
    jpov_box::BoxApp app(cfg);
    app.Init();

    // 地面：扁 box，只用三个尺寸（局部 +Z=front 2，+Y=up 0.1，+X=left 3）。
    // 摆放靠 Draw 的 center/up/front。
    app.ground_mesh_ = app.RegisterMesh(jpov::MeshData::MakeBox(
        /*front_half_width*/ 2.0f, /*up_half_width*/ 0.1f,
        /*left_half_width*/ 3.0f));

    // 立柱：竖直 box，高 2 米（up 半宽 1），front 半宽 0.5，left 半宽 0.2。
    app.pillar_mesh_ = app.RegisterMesh(jpov::MeshData::MakeBox(
        /*front_half_width*/ 0.5f, /*up_half_width*/ 1.0f,
        /*left_half_width*/ 0.2f));

    // 斜柱：斜放 box，front/up/left 半宽 0.7 / 0.7 / 0.35。
    app.slanted_mesh_ = app.RegisterMesh(jpov::MeshData::MakeBox(
        /*front_half_width*/ 0.7f, /*up_half_width*/ 0.7f,
        /*left_half_width*/ 0.35f));

    jpov::WindowInfo winfo;
    winfo.width  = 640.0f;
    winfo.height = 360.0f;
    jpov::InputSnapshot input{};
    app.RunOnce(input, winfo, outpath.c_str());
    app.Finalize();

    LOG(INFO) << "box gold image generated: " << outpath;
    return 0;
}
