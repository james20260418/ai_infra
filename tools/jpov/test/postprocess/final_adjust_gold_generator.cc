// JPOV postprocess — 最终亮度/对比度微调 gold image generator。
//
// 以 pliers（glTF PBR 全通道）场景为基底，渲染两张不同最终微调参数的图：
//   1. final_adjust_highcontrast_lowbright.png：
//        contrast=1.8, brightness=-0.08 → 高对比 + 整体压暗
//   2. final_adjust_lowcontrast.png：
//        contrast=0.4, brightness=0.0    → 低对比（明暗被压缩趋平）
//
// 供 final_adjust_gold_test 做像素范围检查（验证对比/亮度接口确实生效）。
// 场景构建复用 final_adjust_common.h（与 test 共享，防参数分叉）。

#include <cstdio>
#include <string>

#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/test/postprocess/final_adjust_common.h"
#include "tools/jpov/test/test_utils.h"

int main() {
    const std::string base = jpov::GetTestDataDir() + "/postprocess/";

    JPOV::Config cfg;
    cfg.title = "Final Adjust Gold Generator";
    cfg.headless = true;
    jpov::test::FinalAdjustApp app(cfg);
    app.Init();

    // 加载 pliers 场景（内部上传 mesh + 贴图，含 ORM 拆包）
    jpov::GltfObject gltf =
        app.LoadGltf(jpov::test::FinalAdjustApp::GetGltfPath());
    CHECK(!gltf.empty()) << "LoadGltf failed / empty";
    LOG(INFO) << "Loaded " << gltf.size() << " glTF primitives";
    app.SetGltfObject(std::move(gltf));

    // 图 1：高对比 + 低亮度
    jpov::test::RenderFinalAdjust(
        &app, base + "final_adjust_highcontrast_lowbright.png",
        jpov::test::FinalAdjustParams{1.8f, -0.08f});

    // 图 2：低对比
    jpov::test::RenderFinalAdjust(
        &app, base + "final_adjust_lowcontrast.png",
        jpov::test::FinalAdjustParams{0.4f, 0.0f});

    app.Finalize();

    LOG(INFO) << "final_adjust gold images generated (highcontrast_lowbright + "
                 "lowcontrast)";
    return 0;
}
