// JPOV 3D 点光源光照 Gold Image Unit Test
//
// 用 gold image 方法验证 Blinn-Phong 光照渲染链路：
//   1. 从 OBJ 文件加载 beetle 模型 → LoadObj → MeshData → RegisterMesh
//   2. 3 个点光源（红/绿/蓝）从不同方向照射
//   3. Blinn-Phong 着色（diffuse + specular + ambient），object_use_default_color=false
//   4. 保存为 PNG 到 output/jpov_lighting_gold_test/rendered.png
//   5. 与 expected PNG（git 管理）做二进制文件级比较
//
// 测试通过条件：渲染输出 PNG 与 expected PNG 字节完全相同。

#include <cstdint>
#include <string>
#include <vector>

#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/src/obj_loader.h"
#include "tools/common/utils.h"
#include "tools/jpov/test/test_utils.h"

// ============ 测试应用 ============

class LightingGoldTestApp : public JPOV {
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

        // 点光源（与 gold generator 完全一致）
        cmds->point_lights.push_back({
            {0.0f, 2.0f, 0.0f},
            {1.0f, 0.3f, 0.2f, 1.0f},
            3.0f
        });
        cmds->point_lights.push_back({
            {-1.5f, 0.5f, 0.8f},
            {0.2f, 0.9f, 0.3f, 1.0f},
            2.5f
        });
        cmds->point_lights.push_back({
            {1.2f, 0.3f, -0.5f},
            {0.2f, 0.4f, 1.0f, 1.0f},
            2.5f
        });

        // 加载 beetle 模型
        std::string obj_path = jpov::GetProjectRoot() +
                               "tools/jpov/test/beetle.obj";
        jpov::MeshData mesh;
        CHECK(jpov::LoadObj(obj_path, &mesh)) << "Failed to load beetle.obj";

        uint32_t mesh_id = RegisterMesh(mesh);

        // 渲染（默认 object_use_default_color=false，走 GGX 光照着色）
        // PBR 材质：baseColor 偏白，metallic=0 / roughness=1（默认）恒光介
        jpov::PBRMaterial mat;
        mat.base_color = {0.8f, 0.8f, 0.8f, 1.0f};  // 漫反射基础色，偏白
        cmds->DrawObject3D(
            mesh_id, mat,
            {0.0f, 0.0f, 0.0f},
            {0.0f, 1.0f, 0.0f},
            {0.0f, 0.0f, 1.0f});
    }
};

// 解析 expected PNG 路径
static std::string GetExpectedPngPath() {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    if (test_srcdir) {
        std::string p = test_srcdir;
        if (!p.empty() && p.back() != '/') p.push_back('/');
        p += "__main__/tools/jpov/test/lighting_beetle_1280x720.png";
        return p;
    }
    return jpov::GetProjectRoot() +
           "tools/jpov/test/lighting_beetle_1280x720.png";
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
    std::string outdir = jpov::GetOutputDir() + "jpov_lighting_gold_test/";
    std::string outpath = outdir + "rendered.png";

    JPOV::Config cfg;
    cfg.title = "3D Lighting Gold Test";
    cfg.headless = true;
    LightingGoldTestApp app(cfg);
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
        LOG(INFO) << "TEST PASSED: lighting gold image match ("
                  << expected_bytes.size() << " bytes)";
        return 0;
    } else {
        LOG(ERROR) << "TEST FAILED: lighting gold image mismatch";
        return 1;
    }
}
