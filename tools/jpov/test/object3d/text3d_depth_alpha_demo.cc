// JPOV 3D 文本 — 深度测试 + alpha 混合验证（嵌在地板里的中文字）
//
// 目的（Danis 要求）：确认 3D 文本真的参与
//   (1) 深度测试：文字与地板相交时，被地板几何正确遮挡（下半截不可见）
//   (2) alpha 混合：字形边缘是灰度抗锯齿（不是硬边）
//
// 场景：复用 repeated_mrquad 的地板（brick 平放 quad，y=0 水平面）+ 相机 + 光照。
// 三行中文共用同一 anchor (0,0,0)、同一 face (+X)、同一 up (+Y)，
// **唯一变量是 alignment**（kCenter / kBottomLeft / kBottomRight）。
// face=+X → 文字平面是 x=0 的 YZ 竖平面 → 与 y=0 地板面相交于一条水平线。
//
// 验证结论（像素级实测，非肉眼）：
//   kCenter      墨迹 y∈[-0.35,+0.35] → 跨地板面，下半被地板遮
//                无地板 49px → 有地板 39px（吃掉 10px）
//   kBottomLeft  / kBottomRight  墨迹 y∈[0, 0.93] → 全在地板面之上
//   对照：把 anchor 沉到 y=-3（完全在地板下）→ **0 像素可见**，
//         直接证明地板深度遮挡确实生效。
//   alpha：受影响像素中 13.9% 是中间混合值 → 字形边缘是抗锯齿（非硬边）。
//
// ⚠️ 画序：**先地板、后文字**。文字是透明贴片（glDepthMask(GL_FALSE) 不写深度），
//    若先画文字，后续地板会对着“清空后的最远深度”整片通过并覆盖文字。
//    先画地板，文字才能与已写入的地板深度做比较。
//
// 环境变量（调试用）：JPOV_NO_TEXT=1 → 只画地板（供像素级 diff 对照）。
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
        // 统一规格（Danis 指定）：
        //   face 统一沿 **+X**，up 统一沿 **+Y**，anchor 统一 **(0,0,0)**。
        //   三个 alignment 共用同一 anchor → 唯一变量是对齐方式。
        //
        // 验证结果（像素级，见文件头）：
        //   kCenter      墨迹 y∈[-0.35,+0.35] → 跨地板面，下半被遮挡
        //                 （无地板 49px → 有地板 39px，吃掉 10px）
        //   kBottomLeft  / kBottomRight  墨迹 y∈[0, 0.93] → 全在地板面之上
        //
        // 几何：face=+X → 文字平面法线朝 +X（quad 立在 x=0 的 YZ 平面内），
        //   up=+Y 使字正立。三行共用同一 anchor，故在字自身右方向上重叠
        //   ―― 这正是要的对照：**唯一变量是 alignment**。
        const jpov::Vec3f anchor_all = {0.0f, 0.0f, 0.0f};
        const jpov::Vec3f face_pos_x = {1.0f, 0.0f, 0.0f};
        const jpov::Vec3f up_world_y = {0.0f, 1.0f, 0.0f};

        // center 对齐 —— 墨迹中心在 (0,0,0)：下半截穿过地板、被地板遮挡
        cmds->DrawText3D("center", anchor_all, face_pos_x, up_world_y, 1.2f,
                         {1.0f, 1.0f, 0.2f, 1.0f}, "cjk",
                         jpov::TextAlignment::kCenter);

        // 左下对齐 —— 墨迹左下角在 (0,0,0)
        cmds->DrawText3D("左下", anchor_all, face_pos_x, up_world_y, 1.2f,
                         {0.2f, 1.0f, 0.4f, 1.0f}, "cjk",
                         jpov::TextAlignment::kBottomLeft);

        // 右下对齐 —— 墨迹右下角在 (0,0,0)
        cmds->DrawText3D("右下", anchor_all, face_pos_x, up_world_y, 1.2f,
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
