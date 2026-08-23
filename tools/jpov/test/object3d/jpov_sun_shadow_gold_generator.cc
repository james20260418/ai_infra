// JPOV 太阳影子（最简版）gold image generator
//
// 渲染太阳影子场景（平板 + 立柱，sun direction=(-1,-1,1)）并写 gold image：
//   tools/jpov/test/object3d/sun_shadow_1280x720.png
// 输出 640x360（同其它 gold 惯例）。

#include <string>

#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/test/object3d/jpov_sun_shadow_common.h"
#include "tools/jpov/test/test_utils.h"

int main() {
    const std::string outpath =
        jpov::GetTestDataDir() + "/object3d/sun_shadow_1280x720.png";

    JPOV::Config cfg;
    cfg.title = "JPOV Sun Shadow Gold Generator";
    cfg.headless = true;
    jpov_sun_shadow::SunShadowApp app(cfg);
    app.Init();

    // 平板：扁 box，front=4 / up=0.1 / left=4（8×8 米地面）。
    app.ground_mesh_ = app.RegisterMesh(jpov::MeshData::MakeBox(
        /*front_half_width*/ 4.0f, /*up_half_width*/ 0.1f,
        /*left_half_width*/ 4.0f));
    // 立柱：竖直 box，front=0.5 / up=1.0 / left=0.5（1×1×2 米）。
    app.pillar_mesh_ = app.RegisterMesh(jpov::MeshData::MakeBox(
        /*front_half_width*/ 0.5f, /*up_half_width*/ 1.0f,
        /*left_half_width*/ 0.5f));

    jpov::WindowInfo winfo;
    winfo.width  = 640.0f;
    winfo.height = 360.0f;
    jpov::InputSnapshot input{};
    app.RunOnce(input, winfo, outpath.c_str());
    app.Finalize();

    LOG(INFO) << "sun shadow gold image generated: " << outpath;
    return 0;
}
