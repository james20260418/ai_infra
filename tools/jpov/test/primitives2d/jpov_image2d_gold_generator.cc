// JPOV Image2D Gold Generator — 生成 image2d_stamp_640x360.png 参考图片
//
// 运行方式: bazel run //tools/jpov/test:jpov_image2d_gold_generator
// 生成 output/jpov_image2d_gold/ 下的 PNG → 手工复制到 test/ 目录作为 gold image

#include <cstdint>
#include <string>
#include <vector>

#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/common/utils.h"

namespace {

std::string GetStampPath() {
    return jpov::GetProjectRoot() +
           "tools/jpov/test/primitives2d/stamp_200x200.png";
}

}  // namespace

class Image2DGoldGeneratorApp : public JPOV {
public:
    using JPOV::JPOV;
    void SetStampId(uint32_t id) { stamp_id_ = id; }

    void OneIteration(int64_t frame_count,
                      const jpov::InputSnapshot& input,
                      const jpov::WindowInfo& winfo,
                      jpov::RenderCommandList* cmds) override {
        (void)frame_count;
        (void)input;
        (void)winfo;

        // ---- 背景：深灰色矩形 ----
        cmds->DrawRect({0, 0}, {640, 360},
                       {0.15f, 0.15f, 0.15f, 1.0f});

        // ---- #1: 正常透明度印章（左上区域，120x120）----
        cmds->DrawImage(stamp_id_,
                        {50, 40}, {120, 120},
                        {1.0f, 1.0f, 1.0f, 1.0f});

        // ---- #2: 半透明印章（右上区域，150x150）----
        cmds->DrawImage(stamp_id_,
                        {280, 25}, {150, 150},
                        {1.0f, 1.0f, 1.0f, 0.5f});

        // ---- #3: 印章叠在彩色背景上（左下）----
        cmds->DrawRect({30, 200}, {200, 130},
                       {0.2f, 0.4f, 0.8f, 1.0f});
        cmds->DrawImage(stamp_id_,
                        {50, 200}, {120, 120},
                        {1.0f, 1.0f, 1.0f, 0.7f});

        // ---- #4: 拉伸绘制（右下）----
        cmds->DrawImage(stamp_id_,
                        {380, 200}, {220, 140},
                        {1.0f, 1.0f, 1.0f, 0.85f});
    }

private:
    uint32_t stamp_id_ = 0;
};

int main() {
    JPOV::Config cfg;
    cfg.title = "Image2D Gold Generator";
    cfg.headless = true;
    Image2DGoldGeneratorApp app(cfg);
    app.Init();

    uint32_t stamp_id = app.RegisterTexture(GetStampPath());
    LOG(INFO) << "Stamp texture: id=" << stamp_id;
    app.SetStampId(stamp_id);

    std::string outdir = jpov::GetOutputDir() + "jpov_image2d_gold/";
    std::string outpath = outdir + "image2d_stamp_640x360.png";

    jpov::WindowInfo winfo;
    winfo.width  = 640.0f;
    winfo.height = 360.0f;
    jpov::InputSnapshot input{};
    app.RunOnce(input, winfo, outpath.c_str());
    app.Finalize();

    LOG(INFO) << "Gold image generated: " << outpath;
    LOG(INFO) << "Copy to: tools/jpov/test/primitives2d/image2d_stamp_640x360.png";
    return 0;
}
