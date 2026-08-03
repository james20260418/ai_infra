// JPOV Image2D Gold Image Unit Test
//
// 用 gold image 方法验证 Image2D（PNG 纹理渲染）正确：
//   1. 加载透明背景的 stamp PNG 纹理
//   2. 在纯色矩形上叠加绘制印章（验证 alpha blend）
//   3. 保存为 PNG → 与 expected PNG 二进制比较
//
// 测试通过条件：渲染输出 PNG 与 expected PNG 字节完全相同。

#include <cstdint>
#include <string>
#include <vector>

#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/common/utils.h"
#include "tools/jpov/test/test_utils.h"

namespace {

// 获取 stamp 纹理路径（bazel runfiles 或项目根目录）
std::string GetStampPath() {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    if (test_srcdir) {
        std::string p = test_srcdir;
        if (!p.empty() && p.back() != '/') p.push_back('/');
        p += "__main__/tools/jpov/test/stamp_200x200.png";
        return p;
    }
    return jpov::GetProjectRoot() +
           "tools/jpov/test/stamp_200x200.png";
}

std::string GetExpectedPngPath() {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    if (test_srcdir) {
        std::string p = test_srcdir;
        if (!p.empty() && p.back() != '/') p.push_back('/');
        p += "__main__/tools/jpov/test/image2d_stamp_640x360.png";
        return p;
    }
    return jpov::GetProjectRoot() +
           "tools/jpov/test/image2d_stamp_640x360.png";
}

}  // namespace

// ============ 测试应用 ============

class Image2DGoldTestApp : public JPOV {
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
        // 先画彩色矩形底
        cmds->DrawRect({30, 200}, {200, 130},
                       {0.2f, 0.4f, 0.8f, 1.0f});
        // 再叠印章
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

// ============ 测试入口 ============

int main() {
    // 1. 渲染并保存为 PNG
    std::string outdir = jpov::GetOutputDir() + "jpov_image2d_gold_test/";
    std::string outpath = outdir + "rendered.png";

    JPOV::Config cfg;
    cfg.title = "Image2D Gold Test";
    cfg.headless = true;
    Image2DGoldTestApp app(cfg);
    app.Init();

    // 注册 stamp 纹理
    std::string stamp_path = GetStampPath();
    uint32_t stamp_id = app.RegisterTexture(stamp_path);
    LOG(INFO) << "Stamp texture registered: " << stamp_path
              << " → id=" << stamp_id;
    app.SetStampId(stamp_id);

    jpov::WindowInfo winfo;
    winfo.width  = 640.0f;
    winfo.height = 360.0f;
    jpov::InputSnapshot input{};
    app.RunOnce(input, winfo, outpath.c_str());
    app.Finalize();

    // 2. 加载 expected PNG
    std::string expected_path = GetExpectedPngPath();
    std::vector<uint8_t> expected_bytes;
    if (!jpov::ReadFileBytes(expected_path, &expected_bytes)) {
        LOG(FATAL) << "Failed to load expected PNG: " << expected_path;
    }
    LOG(INFO) << "Expected PNG loaded: " << expected_bytes.size() << " bytes";

    // 3. 加载渲染输出的 PNG
    std::vector<uint8_t> render_bytes;
    if (!jpov::ReadFileBytes(outpath, &render_bytes)) {
        LOG(FATAL) << "Failed to load rendered PNG: " << outpath;
    }
    LOG(INFO) << "Rendered PNG: " << render_bytes.size() << " bytes";

    // 4. 二进制比较
    if (render_bytes.size() != expected_bytes.size()) {
        LOG(ERROR) << "Size mismatch: rendered=" << render_bytes.size()
                    << ", expected=" << expected_bytes.size();
        return 1;
    }

    bool pass = true;
    for (size_t i = 0; i < render_bytes.size(); ++i) {
        if (render_bytes[i] != expected_bytes[i]) {
            LOG(ERROR) << "Byte mismatch at offset " << i
                        << ": got 0x" << std::hex
                        << static_cast<int>(render_bytes[i])
                        << ", expected 0x"
                        << static_cast<int>(expected_bytes[i]);
            pass = false;
            break;
        }
    }

    if (pass) {
        LOG(INFO) << "TEST PASSED: image2d stamp gold image match ("
                  << expected_bytes.size() << " bytes)";
        return 0;
    } else {
        LOG(ERROR) << "TEST FAILED: image2d stamp gold image mismatch";
        return 1;
    }
}
