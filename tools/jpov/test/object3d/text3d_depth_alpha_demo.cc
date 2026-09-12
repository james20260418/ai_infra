// JPOV 3D 文本 — 深度测试 + alpha 混合验证（嵌在地板里的中文字）
//
// 目的（Danis 要求）：确认 3D 文本真的参与
//   (1) 深度测试：文字与地板相交时，被地板几何正确遮挡（下半截不可见）
//   (2) alpha 混合：字形边缘是灰度抗锯齿（不是硬边）
//
// 场景：复用 repeated_mrquad 的地板（brick 平放 quad）+ 相机 + 光照。
// 三行中文，**文字平面垂直于地板**（face 水平朝相机，up=(0,1,0) 世界上方），
// 锚点行都落在地板平面 y=0：
//   - kCenter    : 字心在 y=0 → 下半截穿过地板、被地板遮挡（上半截可见）
//   - kBottomLeft / kBottomRight : 锚点在字底 → 整行在地板面之上
//
//   若深度测试生效：center 行会被地板拦腰切开（清晰的水平切缝）。
//   若深度测试失效：center 行完整浮在地板之上（无切缝）。
//
// ⚠️ 画序：**先地板、后文字**。文字是透明贴片（glDepthMask(GL_FALSE) 不写深度），
//    若先画文字，后续地板会对着"清空后的最远深度"整片通过并覆盖文字。
//    先画地板，文字才能与已写入的地板深度做比较。
//
// 输出: tools/jpov/test/object3d/text3d_depth_alpha_1280x720.png

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/interface/mesh.h"
#include "tools/common/utils.h"
#include "tools/jpov/test/test_utils.h"

namespace {

// 与 repeated_mrquad 一致：XZ 平放 quad（法线 +Y），UV 0~repeat 平铺。
jpov::MeshData BuildGroundQuad(float half_x, float half_z, float repeat) {
    jpov::MeshData mesh;
    mesh.flags = static_cast<jpov::MeshVertexFlags>(
        static_cast<uint8_t>(jpov::MeshVertexFlags::kPosition) |
        static_cast<uint8_t>(jpov::MeshVertexFlags::kNormal) |
        static_cast<uint8_t>(jpov::MeshVertexFlags::kUV) |
        static_cast<uint8_t>(jpov::MeshVertexFlags::kTangent));

    const jpov::Vec3f N(0.0f, 1.0f, 0.0f);
    const jpov::Vec3f T(1.0f, 0.0f, 0.0f);
    const float xp = half_x, xm = -half_x;
    const float zp = half_z, zm = -half_z;

    struct Vtx { jpov::Vec3f pos; jpov::Vec3f n; jpov::Vec2f uv; jpov::Vec3f t; };
    const Vtx v[4] = {
        {{ xp, 0.0f, zp}, N, {repeat, 0.0f}, T},
        {{ xm, 0.0f, zp}, N, {0.0f, 0.0f}, T},
        {{ xm, 0.0f, zm}, N, {0.0f, repeat}, T},
        {{ xp, 0.0f, zm}, N, {repeat, repeat}, T},
    };
    for (int i = 0; i < 4; ++i) {
        mesh.positions.push_back(v[i].pos);
        mesh.normals.push_back(v[i].n);
        mesh.uvs.push_back(v[i].uv);
        mesh.tangents.push_back(v[i].t);
    }
    mesh.indices = {0, 2, 1, 0, 3, 2};
    mesh.Validate();
    return mesh;
}

}  // namespace

class Text3dDepthAlphaApp : public JPOV {
public:
    using JPOV::JPOV;

    void SetTextureIds(uint32_t base, uint32_t normal) {
        tex_base_color_ = base; tex_normal_ = normal;
    }

    void OneIteration(int64_t, const jpov::InputSnapshot&,
                      const jpov::WindowInfo&,
                      jpov::RenderCommandList* cmds) override {
        const float kResW = 1280.0f;
        const float kResH = 720.0f;
        cmds->camera.fbo_3d_width_  = kResW;
        cmds->camera.fbo_3d_height_ = kResH;

        // 与 repeated_mrquad 一致的相机 / 光照 / 材质。
        cmds->camera.position = {5.5f, 4.0f, 5.5f};
        cmds->camera.target   = {0.0f, 0.0f, 0.0f};
        cmds->camera.up       = {0.0f, 1.0f, 0.0f};
        cmds->camera.fov      = 60.0f;
        cmds->camera.near     = 0.1f;
        cmds->camera.far      = 1000.0f;

        cmds->point_lights.push_back({{0.0f, 2.0f, 0.0f}, {1.0f,1.0f,1.0f,1}, 6.0f, 0.5f, 3.0f});
        cmds->ambient = jpov::AmbientLight{.color = {1,1,1,1}, .intensity = 0.1f};
        cmds->tile_culling = true;

        jpov::MeshData mesh = BuildGroundQuad(5.0f, 5.0f, 5.0f);
        uint32_t mesh_id = RegisterMesh(mesh);

        jpov::PBRMaterial mat;
        mat.base_color_tex = tex_base_color_;
        mat.base_color = {1.0f, 1.0f, 1.0f, 1.0f};
        mat.metallic = 0.0f;
        mat.roughness = 0.35f;
        mat.emissive = {0.0f, 0.0f, 0.0f, 1.0f};
        mat.ao = {1.0f, 1.0f, 1.0f, 1.0f};
        mat.normal_tex = tex_normal_;
        mat.normal_scale = 2.0f;

        // ---- 1) 先画地板（写深度，作为文字遮挡的参照）----
        cmds->DrawObject3D(mesh_id, mat,
                           {0.0f, 0.0f, 0.0f},
                           {0.0f, 1.0f, 0.0f},
                           {0.0f, 0.0f, 1.0f});

        // 对照图开关：JPOV_NO_TEXT=1 时只画地板（用于像素级比对文字影响）。
        const bool no_text = std::getenv("JPOV_NO_TEXT") != nullptr;
        if (no_text) return;

        // ---- 2) 再画三行文字（与地板深度比较）----
        //
        // 文字平面**垂直于地板**：face = 水平朝向相机 = (0.707,0,0.707)，
        // up = (0,1,0)（世界上方）→ 字正立，读者从相机方向平视。
        //
        // anchor Y 决定“锚点行”与地板面的关系：
        //   0.0  -> kCenter 字心正好在地板面（理论上切一半）
        //   但在本相机（8.87m 远、俯角~27°）下，字贴近地面时透视压缩很强，
        //   切缝只占几像素、肉眼难辨。故把 anchor 降到 **-0.25**：
        //   字心沉入地板下 → 下半截（约 60%）被地板遮挡，切缝明显可见。
        //   这是**故意夸大**以肉眼验证深度遮挡，不是几何错误。
        const float kInvSqrt2 = 0.70710678f;
        const float kAnchorY  = -0.25f;
        const jpov::Vec3f face_to_cam = {kInvSqrt2, 0.0f, kInvSqrt2};
        const jpov::Vec3f up_world    = {0.0f, 1.0f, 0.0f};

        // 三行沿“画面水平方向”（⟂ 视线，≈(0.707,0,-0.707)）铺开。
        const float kOff = 2.2f;
        // 三行用**不同 anchor Y**，形成"层层沉入地板"的阶梯：
        //   右（左下对齐）: anchor 在字底 → 露出最多
        //   中（center）  : 字心沉到地板下 → 只露一点
        //   左（右下对齐）: anchor 在字底但整体更低 → 露出更少
        // 三者底部都被地板整齐切在同一水平线上。
        const jpov::Vec3f pos_c = {0.0f, 0.45f, 0.0f};
        const jpov::Vec3f pos_l = { kInvSqrt2 * kOff, 0.10f, -kInvSqrt2 * kOff};
        const jpov::Vec3f pos_r = {-kInvSqrt2 * kOff, 0.10f,  kInvSqrt2 * kOff};

        // center 对齐 —— **大半截沉入地板，被地板遮挡**
        cmds->DrawText3D("center", pos_c, face_to_cam, up_world, 1.2f,
                         {1.0f, 1.0f, 0.2f, 1.0f}, "cjk",
                         jpov::TextAlignment::kCenter);

        // 左下对齐 —— 锚点在字底左，同样下沉（整行也被地板切）
        cmds->DrawText3D("左下", pos_l, face_to_cam, up_world, 1.2f,
                         {0.2f, 1.0f, 0.4f, 1.0f}, "cjk",
                         jpov::TextAlignment::kBottomLeft);

        // 右下对齐
        cmds->DrawText3D("右下", pos_r, face_to_cam, up_world, 1.2f,
                         {0.3f, 0.7f, 1.0f, 1.0f}, "cjk",
                         jpov::TextAlignment::kBottomRight);
    }

private:
    uint32_t tex_base_color_ = 0, tex_normal_ = 0;
};

int main() {
    const std::string outpath =
        jpov::GetTestDataDir() + "/object3d/text3d_depth_alpha_1280x720.png";

    JPOV::Config cfg;
    cfg.title = "3D Text Depth+Alpha (CJK, intersecting floor)";
    cfg.headless = true;
    // 中文字体：LxgwWenKai 是 .ttf（TrueType outline，stb_truetype 稳）。
    cfg.fonts = {{"tools/jpov/fonts/LxgwWenKai-Regular.ttf", 0, "cjk"}};
    Text3dDepthAlphaApp app(cfg);
    app.Init();

    std::string root = jpov::GetProjectRoot() + "tools/jpov/test/object3d/";
    jpov::TextureOptions tex_opt;
    tex_opt.repeat = true;
    tex_opt.mipmap = true;
    uint32_t base   = app.RegisterTexture(root + "brick_seamless_512x512.png", tex_opt);
    uint32_t normal = app.RegisterTexture(root + "brick_normal_512x512.png", tex_opt);
    app.SetTextureIds(base, normal);

    jpov::WindowInfo winfo;
    winfo.width  = 640.0f;
    winfo.height = 360.0f;
    jpov::InputSnapshot input{};
    app.RunOnce(input, winfo, outpath.c_str());
    app.Finalize();

    LOG(INFO) << "3D text depth+alpha demo -> " << outpath;
    return 0;
}
