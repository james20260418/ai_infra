// JPOV 3D Cube Gold Image Unit Test
//
// 用 gold image 方法验证 3D 正方体渲染正确：
//   1. 使用 Camera(1,1,1) 看向原点，渲染边长 0.5 的正方体(6面颜色不同)
//   2. 保存为 PNG 到 output/jpov_cube3d_gold_test/rendered.png
//   3. 与 expected PNG（git 管理）做二进制文件级比较
//
// 测试通过条件：渲染输出 PNG 与 expected PNG 字节完全相同。

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/common/utils.h"

// 读取文件全部字节
static bool ReadFileBytes(const std::string& path,
                          std::vector<uint8_t>* out) {
    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    if (!ifs.is_open()) {
        LOG(ERROR) << "Failed to open file: " << path;
        return false;
    }
    std::streamsize size = ifs.tellg();
    ifs.seekg(0, std::ios::beg);
    out->resize(static_cast<size_t>(size));
    if (!ifs.read(reinterpret_cast<char*>(out->data()), size)) {
        LOG(ERROR) << "Failed to read file: " << path;
        return false;
    }
    return true;
}

// 正方体顶点（边长 0.5，中心在原点）
static const jpov::Vec3f kVerts[8] = {
    {-0.25f, -0.25f, 0.25f},   // v0: front-bottom-left
    { 0.25f, -0.25f, 0.25f},   // v1: front-bottom-right
    { 0.25f,  0.25f, 0.25f},   // v2: front-top-right
    {-0.25f,  0.25f, 0.25f},   // v3: front-top-left
    {-0.25f, -0.25f, -0.25f},  // v4: back-bottom-left
    { 0.25f, -0.25f, -0.25f},  // v5: back-bottom-right
    { 0.25f,  0.25f, -0.25f},  // v6: back-top-right
    {-0.25f,  0.25f, -0.25f},  // v7: back-top-left
};

static void AddQuad(jpov::RenderCommandList* cmds,
                    int i0, int i1, int i2, int i3,
                    const jpov::Color& color) {
    cmds->DrawTriangle3D(kVerts[i0], kVerts[i1], kVerts[i3], color);
    cmds->DrawTriangle3D(kVerts[i1], kVerts[i2], kVerts[i3], color);
}

// ============ 测试应用 ============

class Cube3dGoldTestApp : public JPOV {
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

        // 6 面
        AddQuad(cmds, 0, 1, 2, 3, jpov::kColorRed);         // 前
        AddQuad(cmds, 4, 5, 6, 7, jpov::kColorGreen);        // 后
        AddQuad(cmds, 1, 5, 6, 2, jpov::kColorBlue);         // 右
        AddQuad(cmds, 4, 0, 3, 7, {1.0f, 1.0f, 0.0f, 1.0f}); // 左(黄)
        AddQuad(cmds, 3, 2, 6, 7, {0.0f, 1.0f, 1.0f, 1.0f}); // 上(青)
        AddQuad(cmds, 4, 5, 1, 0, {1.0f, 0.0f, 1.0f, 1.0f}); // 下(品红)
    }
};

// 解析 expected PNG 路径
static std::string GetExpectedPngPath() {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    if (test_srcdir) {
        std::string p = test_srcdir;
        if (!p.empty() && p.back() != '/') p.push_back('/');
        p += "__main__/tools/jpov/test/cube3d_6faces_1280x720.png";
        return p;
    }
    return jpov::GetProjectRoot() +
           "tools/jpov/test/cube3d_6faces_1280x720.png";
}

// ============ 测试入口 ============

int main() {
    // 1. 加载 expected PNG
    std::string expected_path = GetExpectedPngPath();
    std::vector<uint8_t> expected_bytes;
    if (!ReadFileBytes(expected_path, &expected_bytes)) {
        LOG(FATAL) << "Failed to load expected PNG: " << expected_path;
    }
    LOG(INFO) << "Expected PNG loaded: " << expected_bytes.size() << " bytes";

    // 2. 渲染并保存为 PNG
    std::string outdir = jpov::GetOutputDir() + "jpov_cube3d_gold_test/";
    std::string outpath = outdir + "rendered.png";

    JPOV::Config cfg;
    cfg.title = "3D Cube Gold Test";
    cfg.headless = true;
    Cube3dGoldTestApp app(cfg);
    app.Init();

    jpov::WindowInfo winfo;
    winfo.width  = 640.0f;
    winfo.height = 360.0f;  // 主 FBO 保持 640x360
    jpov::InputSnapshot input{};
    app.RunOnce(input, winfo, outpath.c_str());
    app.Finalize();

    // 3. 加载渲染输出的 PNG
    std::vector<uint8_t> render_bytes;
    if (!ReadFileBytes(outpath, &render_bytes)) {
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
        LOG(INFO) << "TEST PASSED: cube3d gold image match ("
                  << expected_bytes.size() << " bytes)";
        return 0;
    } else {
        LOG(ERROR) << "TEST FAILED: cube3d gold image mismatch";
        return 1;
    }
}
