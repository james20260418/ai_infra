// JPOV PBR baseColor 纹理 Gold Image Unit Test
//
// 用 gold image 方法验证 DrawObject3D 的 baseColor 纹理采样链路：
//   1. 从 OBJ 加载带 UV+normal 的立方体模型（cube_hand.obj）
//   2. RegisterTexture 注册四象限彩色纹理（cube_tex_256x256.png）
//   3. PBRMaterial.base_color_tex 指向纹理，走 GGX PBR 光照 + 逐像素
//      纹理采样（其余通道取常值：metallic=0 / roughness=1）
//   4. 点光源照亮，object_use_default_color=false
//   5. 保存为 PNG → 该效果图供 leader 肉眼判断材质正确性
//
// 测试通过条件：渲染实现跑通并输出材质效果图（供 leader 肉眼判断）。
// 说明：GGX 点光源光照在 Xvfb/Mesa llvmpipe 软渲染器下非确定（同一场景
// 多次渲染会落在「正确着色 / 偏暗 / 退化绿块」三种离散状态之一，帧间最大
// 差达 255），固定基准图+固定容差无法稳定绿。按 leader(#16) 决策：
// 本测试跳过颜色校验，只验证渲染链路跑通并产生非平凡输出图；材质效果的
// 正确与否由 leader 肉眼查看生成的效果图判断。
// 纹理为四象限（红/绿/蓝/黄）+ 中心白点，用于验证 UV 象限映射与朝向。

#include <cstdint>
#include <string>

#include <glog/logging.h>

#include "third_party/stb/stb_image.h"

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/src/obj_loader.h"
#include "tools/common/utils.h"

namespace {

std::string GetCubeObjPath() {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    if (test_srcdir) {
        std::string p = test_srcdir;
        if (!p.empty() && p.back() != '/') p.push_back('/');
        p += "__main__/tools/jpov/test/cube_hand.obj";
        return p;
    }
    return jpov::GetProjectRoot() + "tools/jpov/test/cube_hand.obj";
}

std::string GetTexturePath() {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    if (test_srcdir) {
        std::string p = test_srcdir;
        if (!p.empty() && p.back() != '/') p.push_back('/');
        p += "__main__/tools/jpov/test/cube_tex_256x256.png";
        return p;
    }
    return jpov::GetProjectRoot() + "tools/jpov/test/cube_tex_256x256.png";
}

}  // namespace

// ============ 测试应用 ============

class PbrBaseColorGoldTestApp : public JPOV {
public:
    using JPOV::JPOV;

    void SetTextureId(uint32_t id) { tex_id_ = id; }

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

        // Camera (1,1,1) 看向原点（与 cube3d gold test 一致）
        cmds->camera.position = {1.0f, 1.0f, 1.0f};
        cmds->camera.target   = {0.0f, 0.0f, 0.0f};

        // 点光源：一个暖色主光 + 一个冷色补光
        cmds->point_lights.push_back({
            {0.5f, 2.4f, 0.5f},          // position（立方体上方偏前）
            {1.0f, 0.95f, 0.85f, 1.0f},   // color（暖白）
            5.0f                          // linear_radius
        });
        cmds->point_lights.push_back({
            {-0.3f, 0.5f, -0.6f},        // position（左前下方）
            {0.55f, 0.6f, 1.0f, 1.0f},   // color（冷蓝）
            5.0f                          // linear_radius
        });

        // 加载带 UV + normal 的立方体
        std::string obj_path = GetCubeObjPath();
        jpov::MeshData mesh;
        CHECK(jpov::LoadObj(obj_path, &mesh)) << "Failed to load cube_hand.obj";

        uint32_t mesh_id = RegisterMesh(mesh);

        // PBR 材质：baseColor 走纹理采样；metallic/roughness 用常值默认
        jpov::PBRMaterial mat;
        mat.base_color_tex = tex_id_;      // 纹理优先
        mat.base_color = {1.0f, 1.0f, 1.0f, 1.0f};  // fallback（应为白色，纹理优先）
        mat.metallic = 0.0f;
        mat.roughness = 1.0f;
        cmds->DrawObject3D(
            mesh_id, mat,
            {0.0f, 0.0f, 0.0f},           // center (cube centered at origin) = 立方体中心
            {0.0f, 1.0f, 0.0f},           // up = +Y
            {0.0f, 0.0f, 1.0f});          // front = +Z
    }

private:
    uint32_t tex_id_ = 0;
};

// ============ 测试入口 ============

int main() {
    // 1. 渲染并保存材质效果图（供 leader 肉眼判断）
    std::string outdir = jpov::GetOutputDir() + "jpov_pbr_basecolor_gold_test/";
    std::string outpath = outdir + "rendered.png";

    JPOV::Config cfg;
    cfg.title = "PBR baseColor Gold Test";
    cfg.headless = true;
    PbrBaseColorGoldTestApp app(cfg);
    app.Init();

    // 注册纹理
    std::string tex_path = GetTexturePath();
    uint32_t tex_id = app.RegisterTexture(tex_path);
    LOG(INFO) << "baseColor texture registered: " << tex_path
              << " → id=" << tex_id;
    app.SetTextureId(tex_id);

    jpov::WindowInfo winfo;
    winfo.width  = 640.0f;
    winfo.height = 360.0f;
    jpov::InputSnapshot input{};
    app.RunOnce(input, winfo, outpath.c_str());
    app.Finalize();

    // 2. 跳过颜色校验（leader #16 决策）：仅做渲染链路 smoke check ——
    //    确认输出的效果图存在、尺寸合理且非全空。材质效果的正确性
    //    由 leader 肉眼查看生成的效果图判断，不做固定基准图颜色比对
    //    （llvmpipe 光照三稳态非确定，见文件头注释）。
    int rnd_w = 0, rnd_h = 0, rnd_comp = 0;
    unsigned char* rnd_pixels = stbi_load(outpath.c_str(),
                                          &rnd_w, &rnd_h, &rnd_comp, 4);
    if (!rnd_pixels) {
        LOG(FATAL) << "Failed to load rendered PNG: " << outpath
                    << " (" << stbi_failure_reason() << ")";
    }
    LOG(INFO) << "Rendered effect image: " << rnd_w << "x" << rnd_h
              << " RGBA (" << rnd_comp << " native channels)";

    // 尺寸合理（非退化）
    if (rnd_w <= 0 || rnd_h <= 0) {
        LOG(ERROR) << "Rendered image has invalid dimensions: "
                   << rnd_w << "x" << rnd_h;
        stbi_image_free(rnd_pixels);
        return 1;
    }

    // 非全空：统计非透明(alpha>0)像素数，应占一定比例（确认有物体被渲染）
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

    if (non_transparent == 0) {
        LOG(ERROR) << "Rendered image is entirely empty (no object drawn)";
        return 1;
    }

    // 渲染链路跑通 + 输出非平凡效果图：通过。颜色正确性由 leader 肉眼判断。
    LOG(INFO) << "TEST PASSED: PBR baseColor render pipeline ran and produced "
              << "a non-trivial effect image (color check skipped per leader #16)";
    return 0;
}
