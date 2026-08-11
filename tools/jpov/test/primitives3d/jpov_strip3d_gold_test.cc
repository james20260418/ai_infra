// JPOV 3D Strip Ring Gold Image Unit Test
//
// 用 gold image 方法验证 Strip3DCommand 环状条带渲染正确：
//   1. 使用 Camera(1,1,1) 看向原点，渲染半径 0.5、宽度 0.2 的环状条带
//   2. 保存为 PNG 到 output/jpov_strip3d_gold_test/rendered.png
//   3. 与 expected PNG（git 管理）做二进制文件级比较
//
// 测试通过条件：渲染输出 PNG 与 expected PNG 字节完全相同。

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/common/utils.h"
#include "tools/jpov/test/primitives3d/test_utils.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ============ 测试应用 ============

class Strip3dGoldTestApp : public JPOV {
public:
    using JPOV::JPOV;

    void OneIteration(int64_t frame_count,
                      const jpov::InputSnapshot& input,
                      const jpov::WindowInfo& winfo,
                      jpov::RenderCommandList* cmds) override {
        (void)frame_count;
        (void)input;
        (void)winfo;

        const float kResW = 1280.0f;
        const float kResH = 720.0f;
        cmds->camera.fbo_3d_width_  = kResW;
        cmds->camera.fbo_3d_height_ = kResH;

        // Camera (1,1,1) 看向原点
        cmds->camera.position = {1.0f, 1.0f, 1.0f};
        cmds->camera.target   = {0.0f, 0.0f, 0.0f};

        // ---- 构造环状条带 ----
        const float kRingRadius = 0.5f;
        const float kRingWidth  = 0.2f;
        const float kInnerRadius = kRingRadius - kRingWidth * 0.5f;  // 0.4
        const float kOuterRadius = kRingRadius + kRingWidth * 0.5f;  // 0.6
        const int kSegments = 60;

        std::vector<jpov::Vec3f> verts;
        verts.reserve(kSegments * 2);

        for (int i = 0; i < kSegments; ++i) {
            float angle = 2.0f * static_cast<float>(M_PI) * i / kSegments;
            float cx = std::cos(angle);
            float cz = std::sin(angle);

            // 先外后内，使条带三角形法线朝上
            verts.push_back({cx * kOuterRadius, 0.0f, cz * kOuterRadius});
            verts.push_back({cx * kInnerRadius, 0.0f, cz * kInnerRadius});
        }

        // 闭合环：首部重复前 2 个顶点
        verts.push_back(verts[0]);
        verts.push_back(verts[1]);

        cmds->DrawStrip3D(verts, {0.0f, 0.6f, 1.0f, 1.0f});  // 浅蓝色
    }
};

// 解析 expected PNG 路径
static std::string GetExpectedPngPath() {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    if (test_srcdir) {
        std::string p = test_srcdir;
        if (!p.empty() && p.back() != '/') p.push_back('/');
        p += "__main__/tools/jpov/test/primitives3d/strip3d_ring_1280x720.png";
        return p;
    }
    return jpov::GetProjectRoot() +
           "tools/jpov/test/primitives3d/strip3d_ring_1280x720.png";
}

// ============ 测试入口 ============

int main() {
    // 1. 加载 expected PNG
    std::string expected_path = GetExpectedPngPath();
    std::vector<uint8_t> expected_bytes;
    if (!jpov::ReadFileBytes(expected_path, &expected_bytes)) {
        LOG(FATAL) << "Failed to load expected PNG: " << expected_path;
    }
    LOG(INFO) << "Expected PNG loaded: " << expected_bytes.size() << " bytes";

    // 2. 渲染并保存为 PNG
    std::string outdir = jpov::GetOutputDir() + "jpov_strip3d_gold_test/";
    std::string outpath = outdir + "rendered.png";

    JPOV::Config cfg;
    cfg.title = "3D Strip Ring Gold Test";
    cfg.headless = true;
    Strip3dGoldTestApp app(cfg);
    app.Init();

    jpov::WindowInfo winfo;
    winfo.width  = 640.0f;
    winfo.height = 360.0f;
    jpov::InputSnapshot input{};
    app.RunOnce(input, winfo, outpath.c_str());
    app.Finalize();

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
        LOG(INFO) << "TEST PASSED: strip3d ring gold image match ("
                  << expected_bytes.size() << " bytes)";
        return 0;
    } else {
        LOG(ERROR) << "TEST FAILED: strip3d ring gold image mismatch";
        return 1;
    }
}
