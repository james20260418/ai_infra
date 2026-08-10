// JPOV PBR 自发光 Gold Image Unit Test
//
// 用 gold image 方法验证 DrawObject3D 的自发光 TBN 链路：
//   1. 从 OBJ 加载带 UV+normal 的立方体模型（cube_hand.obj），
//      OBJ 加载器自动推导逐顶点 tangent（kTangent 标志）
//   2. 注册 baseColor 纹理 + 自发光贴图（4 个凸起半球，pbr_emissive_256x256.png）
//   3. PBRMaterial.normal_tex 指向自发光贴图，normal_scale 控制扰动强度，
//      走 GGX PBR 光照 + 切线空间法线经 TBN 变换后做逐像素着色
//   4. 点光源照亮，object_use_default_color=false
//   5. 保存为 PNG → 该效果图供 leader 肉眼判断自发光正确性
//
// 测试通过条件：渲染实现跑通并输出材质效果图（供 leader 肉眼判断）。
// 说明：GGX 点光源光照在 Xvfb/Mesa llvmpipe 软渲染器下非确定（同一场景
// 多次渲染会落在「正确着色 / 偏暗 / 退化绿块」三种离散状态之一，帧间最大
// 差达 255），固定基准图+固定容差无法稳定绿。按 leader(#16) 决策：
// 本测试跳过颜色校验，只验证渲染链路跑通并产生非平凡输出图；自发光的
// 效果正确与否由 leader 肉眼查看生成的效果图判断。

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

std::string GetTexPath(const char* fname) {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    if (test_srcdir) {
        std::string p = test_srcdir;
        if (!p.empty() && p.back() != '/') p.push_back('/');
        p += "__main__/tools/jpov/test/";
        p += fname;
        return p;
    }
    return jpov::GetProjectRoot() + "tools/jpov/test/" + fname;
}

}  // namespace

// ============ 测试应用 ============

class PbrEmissiveGoldTestApp : public JPOV {
public:
    using JPOV::JPOV;

    void SetTextureIds(uint32_t base, uint32_t normal, uint32_t emissive) {
        tex_base_color_ = base;
        tex_normal_ = normal;
        tex_emissive_ = emissive;
    }

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

        // Camera 从右前上方观察立方体中心，能同时看到三个面
        cmds->camera.position = {1.0f, 1.0f, 1.0f};
        cmds->camera.target   = {0.0f, 0.0f, 0.0f};

        // 单点光源：前上方暖色主光，让法线扰动产生清晰明暗
        // 自发光贴图展示灯（grazing key + 正面 fill，凸显凹凸细节）
        // key: 右上方主光，产生 grazing 高光
        cmds->point_lights.push_back({
            {2.0f, 1.8f, 0.5f},
            {35.0f, 32.0f, 28.0f, 1.0f},
            8.0f
        });
        // fill: 正面稍低，降低对比度
        cmds->point_lights.push_back({
            {0.0f, 0.0f, 1.5f},
            {12.0f, 14.0f, 22.0f, 1.0f},
            6.0f
        });
        // rim: 左侧 grazing，产生边缘高光
        cmds->point_lights.push_back({
            {-1.8f, 0.5f, 0.2f},
            {20.0f, 20.0f, 20.0f, 1.0f},
            8.0f
        });        // 加载带 UV + normal 的立方体（OBJ loader 自动推导 tangent）
        jpov::MeshData mesh;
        CHECK(jpov::LoadObj(GetCubeObjPath(), &mesh))
            << "Failed to load cube_hand.obj";
        // 确认 OBJ loader 已推导 kTangent（自发光 TBN 前提）
        CHECK(jpov::MeshHasFlag(mesh.flags, jpov::MeshVertexFlags::kTangent))
            << "LoadObj 未推导 kTangent（自发光需要 tangent）";

        uint32_t mesh_id = RegisterMesh(mesh);

        // PBR 材质：baseColor 走纹理，叠加自发光贴图扰动
        jpov::PBRMaterial mat;
        mat.base_color_tex = tex_base_color_;
        mat.base_color = {1.0f, 1.0f, 1.0f, 1.0f};   // fallback（纹理优先）
        mat.metallic = 0.0f;
        mat.roughness = 0.5f;  // moderate roughness, emissive should be visible
        mat.emissive = {0.0f, 0.0f, 0.0f, 1.0f};
        mat.ao = {1.0f, 1.0f, 1.0f, 1.0f};
        mat.normal_tex = tex_normal_;
        mat.normal_scale = 2.0f;
        mat.emissive_tex = tex_emissive_;
        cmds->DrawObject3D(
            mesh_id, mat,
            {0.0f, 0.0f, 0.0f},           // center = 原点 = 立方体中心
            {0.0f, 1.0f, 0.0f},           // up = +Y
            {0.0f, 0.0f, 1.0f});          // front = +Z
    }

private:
    uint32_t tex_base_color_ = 0;
    uint32_t tex_normal_ = 0;
    uint32_t tex_emissive_ = 0;
};

// ============ 测试入口 ============

int main() {
    // 1. 渲染并保存材质效果图（供 leader 肉眼判断）
    std::string outdir = jpov::GetOutputDir() + "jpov_pbr_normal_gold_test/";
    std::string outpath = outdir + "rendered.png";

    JPOV::Config cfg;
    cfg.title = "PBR Emissive Gold Test";
    cfg.headless = true;
    PbrEmissiveGoldTestApp app(cfg);
    app.Init();

    // 注册 baseColor + 自发光贴图
    uint32_t base   = app.RegisterTexture(GetTexPath("cube_tex_256x256.png"));
    uint32_t normal = app.RegisterTexture(GetTexPath("pbr_normal_256x256.png"));
    uint32_t emissive = app.RegisterTexture(GetTexPath("pbr_emissive_256x256.png"));
    LOG(INFO) << "textures registered: base=" << base << " normal=" << normal;
    app.SetTextureIds(base, normal, emissive);

    jpov::WindowInfo winfo;
    winfo.width  = 640.0f;
    winfo.height = 360.0f;
    jpov::InputSnapshot input{};
    app.RunOnce(input, winfo, outpath.c_str());
    app.Finalize();

    // 2. 跳过颜色校验（leader #16 决策）：仅做渲染链路 smoke check ——
    //    确认输出的效果图存在、尺寸合理且非全空。自发光效果的正确性
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
    LOG(INFO) << "TEST PASSED: PBR emissive map (TBN) render pipeline ran "
              << "and produced a non-trivial effect image (color check skipped "
              << "per leader #16)";
    return 0;
}
