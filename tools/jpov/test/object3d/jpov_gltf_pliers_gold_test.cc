// JPOV glTF Pliers PBR Gold Image Test
//
// 用 glTF 2.0 加载 Poly Haven pliers 模型，验证端到端链路：
//   1. GltfLoader 解析 .gltf → MeshData（含 POSITION/NORMAL/UV + tangent）
//   2. 材质贴图路径提取 → ORM 通道拆包为独立灰度图
//   3. PBRMaterial 所有通道都走纹理（baseColor / normal / metallic / roughness / AO）
//   4. 三光源对称照明，生成效果图
//
// pliers 模型信息：
//   - 3 个 mesh（handle_02_low + handle_01_low + center_low），共 1 材质
//   - 本测试渲染第一个 mesh（handle_02_low.001，1741 顶点）
//   - 贴图: pliers_diff.jpg (baseColor), pliers_nor_gl.jpg (normal),
//           pliers_arm.jpg (ORM → 拆为 AO/roughness/metallic 三张)
//
// 测试通过条件：渲染链路跑通并输出非平凡效果图（参照 cube_normal test 的
// leader #16 决策：跳过颜色校验，leader 肉眼判断效果）。

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <glog/logging.h>

#include "third_party/stb/stb_image.h"

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/src/gltf_loader.h"
#include "tools/jpov/src/orm_unpack.h"
#include "tools/common/utils.h"

namespace {

std::string GetGltfPath() {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    if (test_srcdir) {
        std::string p = test_srcdir;
        if (!p.empty() && p.back() != '/') {
            p.push_back('/');
        }
        p += "__main__/tools/jpov/test/object3d/pliers_gltf/pliers.gltf";
        return p;
    }
    return jpov::GetProjectRoot() +
        "tools/jpov/test/object3d/pliers_gltf/pliers.gltf";
}

std::string GetOutputDir() {
    return jpov::GetOutputDir() + "jpov_gltf_pliers_test/";
}

// 把任意 PNG 字节流写入文件，然后从文件注册纹理。
//
// 因为 TextureManager 只提供 LoadFromFile（不接受内存 buffer），
// 所以 ORM 解包产出的 PNG 字节流需要写临时文件再加载。
uint32_t WriteAndRegisterPng(JPOV* app,
                              const std::string& outdir,
                              const std::string& name,
                              const std::vector<unsigned char>& png_bytes) {
    const std::string path = outdir + name;
    FILE* f = std::fopen(path.c_str(), "wb");
    CHECK(f != nullptr) << "Cannot write " << path;
    std::fwrite(png_bytes.data(), 1, png_bytes.size(), f);
    std::fclose(f);
    return app->RegisterTexture(path);
}

}  // namespace

// ============ 测试应用 ============

class GltfPliersTestApp : public JPOV {
public:
    using JPOV::JPOV;

    void SetTextureIds(uint32_t base_color, uint32_t normal,
                       uint32_t metallic, uint32_t roughness, uint32_t ao) {
        tex_base_color_ = base_color;
        tex_normal_ = normal;
        tex_metallic_ = metallic;
        tex_roughness_ = roughness;
        tex_ao_ = ao;
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

        // 相机从正前方观察 pliers（Z-up 坐标系，pliers: X[-0.01,0.03] Y[-0.01,0.01] Z[-0.13,0.05]）
        // pliers 中心约在 (0.0075, 0, -0.04)
        const float cx = 0.0075f;
        const float cz = -0.04f;
        cmds->camera.position = {cx, 0.12f, cz + 0.08f};
        cmds->camera.target   = {cx, 0.0f, cz};

        // 三光源对称照明（同 cube_normal test）
        cmds->point_lights.push_back({
            {0.15f, 0.0f, 0.0f},
            {2.0f, 2.0f, 2.0f, 1.0f},
            0.5f,
            0.01f
        });
        cmds->point_lights.push_back({
            {0.0f, 0.15f, 0.0f},
            {2.0f, 2.0f, 2.0f, 1.0f},
            0.5f,
            0.01f
        });
        cmds->point_lights.push_back({
            {0.0f, 0.0f, 0.15f},
            {2.0f, 2.0f, 2.0f, 1.0f},
            0.5f,
            0.01f
        });

        jpov::MeshData mesh;
        jpov::GltfMaterialInfo mat_info;
        CHECK(jpov::LoadGltf(GetGltfPath(), &mesh, &mat_info))
            << "Failed to load pliers.gltf";
        CHECK(jpov::MeshHasFlag(mesh.flags, jpov::MeshVertexFlags::kTangent))
            << "LoadGltf 未推导 kTangent";

        uint32_t mesh_id = RegisterMesh(mesh);

        // PBR 材质：所有通道走纹理（全 6 通道 PBR）
        jpov::PBRMaterial mat;
        mat.base_color_tex = tex_base_color_;
        mat.base_color = {1.0f, 1.0f, 1.0f, 1.0f};

        mat.normal_tex = tex_normal_;
        mat.normal_scale = mat_info.normal_scale;

        mat.has_metallic_tex = true;
        mat.metallic_tex = tex_metallic_;
        mat.metallic = mat_info.metallic_factor;

        mat.has_roughness_tex = true;
        mat.roughness_tex = tex_roughness_;
        mat.roughness = mat_info.roughness_factor;

        mat.ao_tex = tex_ao_;
        mat.ao = {1.0f, 1.0f, 1.0f, 1.0f};

        mat.emissive = {0.0f, 0.0f, 0.0f, 1.0f};

        cmds->DrawObject3D(
            mesh_id, mat,
            {0.0f, 0.0f, 0.0f},           // center
            {0.0f, 1.0f, 0.0f},           // up = +Y
            {0.0f, 0.0f, 1.0f});          // front = +Z
    }

private:
    uint32_t tex_base_color_ = 0;
    uint32_t tex_normal_ = 0;
    uint32_t tex_metallic_ = 0;
    uint32_t tex_roughness_ = 0;
    uint32_t tex_ao_ = 0;
};

// ============ 测试入口 ============

int main() {
    // 1. 加载 ORM 贴图 → CPU 拆包
    std::string gltf_dir;
    {
        const char* test_srcdir = std::getenv("TEST_SRCDIR");
        if (test_srcdir) {
            std::string p = test_srcdir;
            if (!p.empty() && p.back() != '/') {
                p.push_back('/');
            }
            gltf_dir = p +
                "__main__/tools/jpov/test/object3d/pliers_gltf/";
        } else {
            gltf_dir = jpov::GetProjectRoot() +
                "tools/jpov/test/object3d/pliers_gltf/";
        }
    }

    // 读取 ORM 贴图（pliers_arm.jpg = AO/Roughness/Metallic 三合一）
    const std::string arm_path = gltf_dir + "pliers_arm.jpg";
    int ow = 0;
    int oh = 0;
    int oc = 0;
    unsigned char* arm_pixels = stbi_load(arm_path.c_str(), &ow, &oh, &oc, 4);
    CHECK(arm_pixels != nullptr)
        << "Failed to load ORM texture: " << arm_path
        << " (" << stbi_failure_reason() << ")";
    LOG(INFO) << "ORM texture loaded: " << ow << "x" << oh;

    std::string outdir = GetOutputDir();
    std::system(("mkdir -p " + outdir).c_str());

    // ORM 拆包：R=AO, G=Roughness, B=Metallic
    std::vector<unsigned char> ao_png  =
        jpov::ExtractChannelToPng(arm_pixels, ow, oh, 0);
    std::vector<unsigned char> rough_png =
        jpov::ExtractChannelToPng(arm_pixels, ow, oh, 1);
    std::vector<unsigned char> metal_png =
        jpov::ExtractChannelToPng(arm_pixels, ow, oh, 2);
    stbi_image_free(arm_pixels);

    CHECK(!ao_png.empty()) << "ORM AO extract failed";
    CHECK(!rough_png.empty()) << "ORM Roughness extract failed";
    CHECK(!metal_png.empty()) << "ORM Metallic extract failed";
    LOG(INFO) << "ORM channels extracted: AO=" << ao_png.size()
              << "b RGH=" << rough_png.size()
              << "b MET=" << metal_png.size() << "b";

    // 2. 渲染效果图
    std::string outpath = outdir + "rendered.png";

    JPOV::Config cfg;
    cfg.title = "glTF Pliers PBR Test";
    cfg.headless = true;
    GltfPliersTestApp app(cfg);
    app.Init();

    // 注册 baseColor + normal 贴图（直接从 disk）
    const std::string base_path  = gltf_dir + "pliers_diff.jpg";
    const std::string norm_path  = gltf_dir + "pliers_nor_gl.jpg";

    uint32_t tex_base  = app.RegisterTexture(base_path);
    uint32_t tex_norm  = app.RegisterTexture(norm_path);
    uint32_t tex_met   = WriteAndRegisterPng(&app, outdir, "metal.png", metal_png);
    uint32_t tex_rgh   = WriteAndRegisterPng(&app, outdir, "rough.png", rough_png);
    uint32_t tex_ao    = WriteAndRegisterPng(&app, outdir, "ao.png", ao_png);

    LOG(INFO) << "Textures: base=" << tex_base << " normal=" << tex_norm
              << " metallic=" << tex_met << " roughness=" << tex_rgh
              << " ao=" << tex_ao;

    app.SetTextureIds(tex_base, tex_norm, tex_met, tex_rgh, tex_ao);

    jpov::WindowInfo winfo;
    winfo.width  = 640.0f;
    winfo.height = 360.0f;
    jpov::InputSnapshot input{};
    app.RunOnce(input, winfo, outpath.c_str());
    app.Finalize();

    // 3. Smoke check：效果图存在且非空
    int rnd_w = 0;
    int rnd_h = 0;
    int rnd_comp = 0;
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
    int max_r = 0;
    int max_g = 0;
    int max_b = 0;
    for (int i = 0; i < total_pixels; ++i) {
        if (rnd_pixels[i * 4 + 3] > 0) {
            ++non_transparent;
        }
        if (rnd_pixels[i * 4 + 0] > max_r) {
            max_r = rnd_pixels[i * 4 + 0];
        }
        if (rnd_pixels[i * 4 + 1] > max_g) {
            max_g = rnd_pixels[i * 4 + 1];
        }
        if (rnd_pixels[i * 4 + 2] > max_b) {
            max_b = rnd_pixels[i * 4 + 2];
        }
    }
    stbi_image_free(rnd_pixels);

    const float coverage =
        static_cast<float>(non_transparent) / total_pixels;
    LOG(INFO) << "Non-transparent coverage=" << (coverage * 100.0f)
              << "%, max RGB=(" << max_r << "," << max_g << "," << max_b << ")";

    CHECK_GT(non_transparent, 0)
        << "Rendered image is entirely empty (no object drawn)";

    LOG(INFO) << "TEST PASSED: glTF pliers PBR render pipeline ran "
              << "and produced a non-trivial effect image "
              << "(color check skipped per leader #16)";
    return 0;
}
