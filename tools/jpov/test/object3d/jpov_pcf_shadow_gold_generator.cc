// JPOV 阴影 PCF 采样核 gold image generator
//
// 渲染「PCF 对比」场景（天光查看器同款：地面 + 三方块 + 桌子 + 橡树 + 三个蓝人），
// 用**软化态**采样核（黄金角螺旋 N=25 / R=4 ≈ 9 纹素）写 gold image：
//   tools/jpov/test/object3d/pcf_golden_spiral_1280x720.png
//
// 供 jpov_pcf_shadow_gold_test 保护「黄金角螺旋采样」不被改坏（含粗细双闸门，
// 见 test：固定 3×3 硬边会让细粒度逐像素闸门失败）。

#include <string>

#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/test/object3d/jpov_pcf_shadow_common.h"
#include "tools/jpov/test/test_utils.h"

int main() {
    const std::string outpath =
        jpov::GetTestDataDir() + "/object3d/pcf_golden_spiral_1280x720.png";

    JPOV::Config cfg;
    cfg.title = "JPOV PCF Golden-Spiral Gold Generator";
    cfg.headless = true;
    jpov_pcf_shadow::PcfShadowApp app(cfg);
    app.Init();
    jpov_pcf_shadow::AssembleScene(app);

    // gold = 软化态：黄金角螺旋 N=25 / R=4（采样盘直径 ≈ 9 纹素）。
    app.SetShadowPcf(jpov::ShadowPcfConfig{
        /*mode*/ jpov::ShadowPcfConfig::Mode::kGoldenSpiral,
        /*tap_count*/ 25, /*radius_texels*/ 4.0f});

    jpov::WindowInfo winfo;
    winfo.width  = 1280.0f;
    winfo.height = 720.0f;
    jpov::InputSnapshot input{};
    app.RunOnce(input, winfo, outpath.c_str());
    app.Finalize();

    LOG(INFO) << "pcf golden-spiral gold image generated: " << outpath;
    return 0;
}
