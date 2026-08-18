// JPOV Gold Image Test — 平放 wall 石材 + 单点光源
//
// 渲染一块平放在地上的 wall 石板（scene_assets/wall_rock/wall_rock.gltf），
// 单点光源 (0,3,0) 顶光，tile culling 开，太阳关。作为逐步过渡到草地
// repeated test 的第一步基线：确认已知法线好的 wall 在顶光下法线细节正常。
//
// 测试通过条件：渲染链路跑通 + 输出非平凡效果图（leader #16 决策：不做
// 逐像素颜色比对，效果正确性由 leader/Danis 肉眼查看 generator 产出的
// gold image repeated_wall_1280x720.png 判断）。

#include <cstdio>
#include <cstring>
#include <string>

#include <glog/logging.h>

#include "third_party/stb/stb_image.h"

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/interface/gltf_object.h"
#include "tools/common/utils.h"

namespace {

std::string GetWallPath() {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    if (test_srcdir) {
        std::string p = test_srcdir;
        if (!p.empty() && p.back() != '/') p.push_back('/');
        p += "__main__/tools/jpov/test/object3d/scene_assets/wall_rock/wall_rock.gltf";
        return p;
    }
    return jpov::GetProjectRoot() +
        "tools/jpov/test/object3d/scene_assets/wall_rock/wall_rock.gltf";
}

std::string GetOutputDir() { return jpov::GetOutputDir() + "jpov_repeated_wall_test/"; }

// 仓库内 gold image 路径（generator 写入，供肉眼/比对参考）。
std::string GetGoldPath() {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    if (test_srcdir) {
        std::string p = test_srcdir;
        if (!p.empty() && p.back() != '/') p.push_back('/');
        p += "__main__/tools/jpov/test/object3d/repeated_wall_1280x720.png";
        return p;
    }
    return jpov::GetProjectRoot() +
        "tools/jpov/test/object3d/repeated_wall_1280x720.png";
}

}  // namespace

// ============ 测试应用 ============

class RepeatedWallTestApp : public JPOV {
public:
    using JPOV::JPOV;

    void SetGltfObject(jpov::GltfObject obj) { gltf_ = std::move(obj); }

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

        cmds->camera.position = {3.5f, 3.0f, 3.5f};
        cmds->camera.target   = {0.0f, 0.1f, 0.0f};
        cmds->camera.up       = {0.0f, 1.0f, 0.0f};

        // 单点光源顶光 (0,3,0)。tile culling 默认 true；不设 sun（太阳关）。
        cmds->point_lights.push_back({
            {0.0f, 3.0f, 0.0f}, {1.0f, 1.0f, 1.0f, 1.0f}, 12.0f, 0.5f});

        // 平放 wall：恒等朝向（glTF loader 已 z-up→y-up，大面在局部 XZ 平面）
        cmds->DrawGltfObject(gltf_, {0.0f, 0.1f, 0.0f},
                             {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f});
    }

private:
    jpov::GltfObject gltf_;
};

// ============ 测试入口 ============

int main() {
    const std::string outdir = GetOutputDir();
    std::system(("mkdir -p " + outdir).c_str());
    const std::string outpath = outdir + "rendered.png";

    // 校验 gold image 存在（generator 生成；缺失=回归）
    {
        const std::string gold = GetGoldPath();
        FILE* f = std::fopen(gold.c_str(), "rb");
        CHECK(f != nullptr) << "gold image 缺失，请先跑 "
            "jpov_repeated_wall_gold_generator: " << gold;
        std::fclose(f);
    }

    JPOV::Config cfg;
    cfg.title = "Repeated Wall (flat, top light) Gold Test";
    cfg.headless = true;
    RepeatedWallTestApp app(cfg);
    app.Init();

    jpov::GltfObject gltf = app.LoadGltf(GetWallPath());
    CHECK(!gltf.empty()) << "LoadGltf wall failed / empty";
    app.SetGltfObject(std::move(gltf));

    jpov::WindowInfo winfo;
    winfo.width  = 640.0f;
    winfo.height = 360.0f;
    jpov::InputSnapshot input{};
    app.RunOnce(input, winfo, outpath.c_str());
    app.Finalize();

    // Smoke check：效果图存在且非空
    int rnd_w = 0, rnd_h = 0, rnd_comp = 0;
    unsigned char* rnd_pixels = stbi_load(outpath.c_str(),
                                          &rnd_w, &rnd_h, &rnd_comp, 4);
    CHECK(rnd_pixels != nullptr) << "Failed to load rendered PNG: " << outpath
        << " (" << (stbi_failure_reason() ? stbi_failure_reason() : "?") << ")";
    LOG(INFO) << "Rendered effect image: " << rnd_w << "x" << rnd_h;

    const int total_pixels = rnd_w * rnd_h;
    int non_transparent = 0;
    int max_r = 0, max_g = 0, max_b = 0;
    for (int i = 0; i < total_pixels; ++i) {
        if (rnd_pixels[i * 4 + 3] > 0) ++non_transparent;
        if (rnd_pixels[i * 4 + 0] > max_r) max_r = rnd_pixels[i * 4 + 0];
        if (rnd_pixels[i * 4 + 1] > max_g) max_g = rnd_pixels[i * 4 + 1];
        if (rnd_pixels[i * 4 + 2] > max_b) max_b = rnd_pixels[i * 4 + 2];
    }
    stbi_image_free(rnd_pixels);

    const float coverage = static_cast<float>(non_transparent) / total_pixels;
    LOG(INFO) << "Non-transparent coverage=" << (coverage * 100.0f)
              << "%, max RGB=(" << max_r << "," << max_g << "," << max_b << ")";

    CHECK_GT(non_transparent, 0) << "Rendered image empty (wall not drawn)";

    LOG(INFO) << "TEST PASSED: repeated wall (flat + top light) pipeline ran "
              << "and produced a non-trivial effect image "
              << "(color check skipped per leader #16)";
    return 0;
}
