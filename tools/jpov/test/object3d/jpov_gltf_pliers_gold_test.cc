// JPOV glTF Pliers PBR Gold Image Test
//
// 用 glTF 2.0 加载 Poly Haven pliers 模型，验证端到端新 API：
//   1. JPOV::LoadGltf(path) → GltfObject（多 mesh + 贴图去重 + ORM 拆包）
//   2. RenderCommandList::DrawGltfObject(obj, ...) 渲染整个对象
//   3. JPOV::ReleaseGltf(obj) 整体释放
//
// pliers.gltf 有 3 个 mesh（handle_02_low + handle_01_low + center_low），
// 共享同一套贴图（diff / normal_gl / arm-ORM）。本测试渲染完整的 3 个 mesh，
// 验证 glTF loader 的多 primitive 加载 + PBR 全 6 通道（baseColor/normal/
// metallic/roughness/AO）渲染。
//
// 测试通过条件：渲染链路跑通并输出非平凡效果图（参照 cube_normal test 的
// leader #16 决策：跳过颜色校验，leader 肉眼判断效果）。

#include <cstdint>
#include <string>

#include <glog/logging.h>

#include "third_party/stb/stb_image.h"

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/interface/gltf_object.h"
#include "tools/common/utils.h"

namespace {

std::string GetGltfPath() {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    if (test_srcdir) {
        std::string p = test_srcdir;
        if (!p.empty() && p.back() != '/') p.push_back('/');
        p += "__main__/tools/jpov/test/object3d/pliers_gltf/pliers.gltf";
        return p;
    }
    return jpov::GetProjectRoot() +
        "tools/jpov/test/object3d/pliers_gltf/pliers.gltf";
}

std::string GetOutputDir() {
    return jpov::GetOutputDir() + "jpov_gltf_pliers_test/";
}

}  // namespace

// ============ 测试应用 ============

class GltfPliersTestApp : public JPOV {
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

        // 相机与 generator 一致：3/4 视角完整看 pliers（长轴沿 Z）
        // near=0.01 避免近裁剪面切掉朝相机一侧的手柄/钳口（模型极小）。
        const float cx = 0.0075f;
        const float cz = -0.04f;
        cmds->camera.position = {0.075f, 0.08f, cz + 0.045f};
        cmds->camera.target   = {cx, 0.0f, cz};
        cmds->camera.near     = 0.01f;

        // 三光源对称照明（同 cube_normal test）
        cmds->point_lights.push_back({
            {0.15f, 0.0f, 0.0f}, {2.0f, 2.0f, 2.0f, 1.0f}, 0.5f, 0.01f});
        cmds->point_lights.push_back({
            {0.0f, 0.15f, 0.0f}, {2.0f, 2.0f, 2.0f, 1.0f}, 0.5f, 0.01f});
        cmds->point_lights.push_back({
            {0.0f, 0.0f, 0.15f}, {2.0f, 2.0f, 2.0f, 1.0f}, 0.5f, 0.01f});

        // 绘制整个 glTF 对象（3 个 mesh，内部展开为多个 DrawObject3D）
        cmds->DrawGltfObject(gltf_, {0.0f, 0.0f, 0.0f},
                             {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f});
    }

private:
    jpov::GltfObject gltf_;
};

// ============ 测试入口 ============

int main() {
    std::string outdir = GetOutputDir();
    std::system(("mkdir -p " + outdir).c_str());
    const std::string outpath = outdir + "rendered.png";

    JPOV::Config cfg;
    cfg.title = "glTF Pliers PBR Test";
    cfg.headless = true;
    GltfPliersTestApp app(cfg);
    app.Init();

    // 用新 API 加载 pliers（内部：纯净 loader + MeshManager/TextureManager
    // 上传 + 多 mesh + 贴图去重 + ORM 拆包）
    jpov::GltfObject gltf = app.LoadGltf(GetGltfPath());
    CHECK(!gltf.empty()) << "LoadGltf failed / empty";
    CHECK_EQ(gltf.size(), 3u)
        << "pliers.gltf 应有 3 个 primitive（handle_02 + handle_01 + center）";
    LOG(INFO) << "Loaded " << gltf.size() << " glTF primitives";
    app.SetGltfObject(std::move(gltf));

    jpov::WindowInfo winfo;
    winfo.width  = 640.0f;
    winfo.height = 360.0f;
    jpov::InputSnapshot input{};
    app.RunOnce(input, winfo, outpath.c_str());
    app.Finalize();

    // 资源生命周期：RenderGltf 的 GPU 资源随 app 持有，Finalize() 销毁
    // Renderer 时由 MeshManager/TextureManager 统一释放（本测试单帧渲染，
    // 依赖 Finalize 清理即可）。若需提前释放，调用 app 上的 ReleaseGltf。

    // 3. Smoke check：效果图存在且非空
    int rnd_w = 0, rnd_h = 0, rnd_comp = 0;
    unsigned char* rnd_pixels = stbi_load(outpath.c_str(),
                                           &rnd_w, &rnd_h, &rnd_comp, 4);
    CHECK(rnd_pixels != nullptr)
        << "Failed to load rendered PNG: " << outpath
        << " (" << stbi_failure_reason() << ")";
    LOG(INFO) << "Rendered effect image: " << rnd_w << "x" << rnd_h;

    CHECK_GT(rnd_w, 0);
    CHECK_GT(rnd_h, 0);

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

    CHECK_GT(non_transparent, 0)
        << "Rendered image is entirely empty (no object drawn)";

    LOG(INFO) << "TEST PASSED: glTF pliers PBR render pipeline ran "
              << "and produced a non-trivial effect image "
              << "(color check skipped per leader #16)";
    return 0;
}
