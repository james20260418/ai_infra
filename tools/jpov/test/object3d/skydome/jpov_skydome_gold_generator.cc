// JPOV Skydome gold image generator —— 白天/晚霞/夜间 × 双机位
//
// 渲染 3×2 = 6 张程序化天光 gold image（供肉眼查看，验证 bug-free）：
//   时刻：day / sunset / night
//   机位：from_body（从日月看向场景中心）/ to_body（从场景中心看向日月）
// 文件命名：skydome_<time>_<cam>.png。输出 640x360。
//
// ⚠️ 只 Init/Finalize 一次，循环改 time/cam 调 RunOnce 6 次（避免反复
// 重建 GL context/Xvfb 导致 abort）。

#include <cstdio>
#include <string>

#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/test/object3d/skydome/jpov_skydome_common.h"

int main() {
    const char* times[]  = {"day", "sunset", "night"};
    const char* cams[]   = {"from_body", "to_body"};
    const jpov_skydome::TimePreset time_v[] = {
        jpov_skydome::TimePreset::kDay,
        jpov_skydome::TimePreset::kSunset,
        jpov_skydome::TimePreset::kNight,
    };
    const jpov_skydome::CamPreset cam_v[] = {
        jpov_skydome::CamPreset::kFromBody,
        jpov_skydome::CamPreset::kToBody,
    };

    JPOV::Config cfg;
    cfg.title = "JPOV Skydome Gold Generator";
    cfg.headless = true;
    jpov_skydome::SkydomeSceneApp app(cfg);
    app.Init();
    jpov_skydome::BuildScene(&app);

    jpov::WindowInfo winfo;
    winfo.width  = 640.0f;
    winfo.height = 360.0f;
    jpov::InputSnapshot input{};

    for (int ti = 0; ti < 3; ++ti) {
        for (int ci = 0; ci < 2; ++ci) {
            app.time_ = time_v[ti];
            app.cam_  = cam_v[ci];
            std::string outpath = std::string(
                "/james_pm/ai_infra/tools/jpov/test/object3d/skydome/")
                + "skydome_" + times[ti] + "_" + cams[ci] + ".png";
            app.RunOnce(input, winfo, outpath.c_str());
            LOG(INFO) << "skydome gold: " << outpath;
        }
    }

    app.Finalize();
    LOG(INFO) << "全部 6 张 skydome gold image 生成完毕";
    return 0;
}
