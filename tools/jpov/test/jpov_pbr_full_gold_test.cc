// JPOV 综合 PBR 全通道材质 Gold Image Unit Test
//
// 用 gold image 方法验证 DrawObject3D 的六通道材质链路同时启用：
// baseColor + metallic + roughness + emissive + AO + normal（法线映射 TBN）。
// 这是 PBR 材质系统的综合集成测试——此前 basecolor / textures / normal
// 三个 gold test 各自只覆盖子集，这里把所有通道叠加到同一模型同一帧：
//   1. 从 OBJ 加载带 UV+normal 的立方体（cube_hand.obj）
//   2. 注册 6 个通道纹理：baseColor / metallic / roughness / emissive / AO / normal
//   3. PBRMaterial 六通道 *_tex 全部指向纹理，走 GGX PBR 光照 + 逐像素采样
//      + 法线映射 TBN（metallic/roughness/AO 采样 .r，emissive 采样 .rgb）
//   4. 双点光源照亮，object_use_default_color=false
//   5. 保存为 PNG → 该效果图供 leader 肉眼判断材质正确性
//
// 测试通过条件：渲染实现跑通并输出材质效果图（供 leader 肉眼判断）。
// 说明：GGX 点光源光照在 Xvfb/Mesa llvmpipe 软渲染器下非确定（同一场景
// 多次渲染会落在「正确着色 / 偏暗 / 退化绿块」三种离散状态之一，帧间最大
// 差达 255），固定基准图+固定容差无法稳定绿。按 leader(#16) 决策：
// 本测试跳过颜色校验，只验证渲染链路跑通并产生非平凡输出图；材质效果的
// 正确与否由 leader 肉眼查看生成的效果图判断。

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

class PbrFullGoldTestApp : public JPOV {
public:
    using JPOV::JPOV;

    void SetTextureIds(uint32_t base, uint32_t met, uint32_t rough,
                       uint32_t emissive, uint32_t ao, uint32_t normal) {
        tex_base_color_ = base;
        tex_metallic_ = met;
        tex_roughness_ = rough;
        tex_emissive_ = emissive;
        tex_ao_ = ao;
        tex_normal_ = normal;
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

        // Camera 从右前上方观察立方体中心，能同时看到三个面（纹理可见）
        cmds->camera.position = {1.9f, 1.3f, 1.9f};
        cmds->camera.target   = {0.5f, 0.5f, 0.5f};

        // 点光源：暖色主光 + 冷色补光，突出法线扰动与金属高光
        cmds->point_lights.push_back({
            {0.5f, 2.4f, 0.5f},          // position（立方体上方偏前）
            {1.0f, 0.95f, 0.85f, 1.0f},   // color（暖白）
            3.5f                          // linear_radius
        });
        cmds->point_lights.push_back({
            {-0.3f, 0.5f, -0.6f},        // position（左前下方）
            {0.55f, 0.6f, 1.0f, 1.0f},   // color（冷蓝）
            3.0f                          // linear_radius
        });

        // 加载带 UV + normal 的立方体
        std::string obj_path = GetCubeObjPath();
        jpov::MeshData mesh;
        CHECK(jpov::LoadObj(obj_path, &mesh)) << "Failed to load cube_hand.obj";
        // 法线映射 TBN 前提：OBJ loader 自动推导 tangent
        CHECK(jpov::MeshHasFlag(mesh.flags, jpov::MeshVertexFlags::kTangent))
            << "LoadObj 未推导 kTangent（法线映射需要 tangent）";

        uint32_t mesh_id = RegisterMesh(mesh);

        // PBR 材质：六通道全部走纹理采样（逐像素材质参数场 + 法线扰动）
        jpov::PBRMaterial mat;
        mat.base_color_tex = tex_base_color_;
        mat.base_color = {1.0f, 1.0f, 1.0f, 1.0f};   // fallback（纹理优先）
        mat.has_metallic_tex = true;
        mat.metallic_tex = tex_metallic_;
        mat.metallic = 0.0f;                          // fallback
        mat.has_roughness_tex = true;
        mat.roughness_tex = tex_roughness_;
        mat.roughness = 1.0f;                         // fallback
        mat.emissive_tex = tex_emissive_;
        mat.emissive = {0.0f, 0.0f, 0.0f, 1.0f};      // fallback
        mat.ao_tex = tex_ao_;
        mat.ao = {1.0f, 1.0f, 1.0f, 1.0f};            // fallback
        mat.normal_tex = tex_normal_;
        mat.normal_scale = 1.0f;
        cmds->DrawObject3D(
            mesh_id, mat,
            {0.5f, 0.5f, 0.5f},           // center = 立方体中心
            {0.0f, 1.0f, 0.0f},           // up = +Y
            {0.0f, 0.0f, 1.0f});          // front = +Z
    }

private:
    uint32_t tex_base_color_ = 0;
    uint32_t tex_metallic_ = 0;
    uint32_t tex_roughness_ = 0;
    uint32_t tex_emissive_ = 0;
    uint32_t tex_ao_ = 0;
    uint32_t tex_normal_ = 0;
};

// ============ 测试入口 ============

int main() {
    // 1. 渲染并保存材质效果图（供 leader 肉眼判断）
    std::string outdir = jpov::GetOutputDir() + "jpov_pbr_full_gold_test/";
    std::string outpath = outdir + "rendered.png";

    JPOV::Config cfg;
    cfg.title = "PBR Full Gold Test";
    cfg.headless = true;
    PbrFullGoldTestApp app(cfg);
    app.Init();

    // 注册 6 个通道纹理
    const uint32_t num_textures = 6;
    uint32_t base  = app.RegisterTexture(GetTexPath("cube_tex_256x256.png"));
    uint32_t met   = app.RegisterTexture(GetTexPath("pbr_metallic_256x256.png"));
    uint32_t rough = app.RegisterTexture(GetTexPath("pbr_roughness_256x256.png"));
    uint32_t emiss = app.RegisterTexture(GetTexPath("pbr_emissive_256x256.png"));
    uint32_t ao    = app.RegisterTexture(GetTexPath("pbr_ao_256x256.png"));
    uint32_t norm  = app.RegisterTexture(GetTexPath("pbr_normal_256x256.png"));
    LOG(INFO) << "textures registered: base=" << base << " metallic=" << met
              << " roughness=" << rough << " emissive=" << emiss << " ao=" << ao
              << " normal=" << norm;
    // 六通道必须都能注册成功（句柄非 0），否则说明资源加载失败
    if (base == 0 || met == 0 || rough == 0 || emiss == 0 || ao == 0 || norm == 0) {
        LOG(ERROR) << "Failed to register one or more texture channels";
        return 1;
    }
    app.SetTextureIds(base, met, rough, emiss, ao, norm);

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

    // 渲染链路跑通 + 六通道纹理全部注册成功 + 输出非平凡效果图：通过。
    // 颜色正确性由 leader 肉眼判断。
    LOG(INFO) << "TEST PASSED: PBR full (6-channel, incl. normal TBN) render "
              << "pipeline ran and produced a non-trivial effect image "
              << "(color check skipped per leader #16; " << num_textures
              << " textures registered)";
    return 0;
}
