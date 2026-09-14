// JPOV 3D 文本深度/alpha gold — 公共场景（generator 与 test 共用）
//
// 场景（Danis 指定规格）：repeated_mrquad 的 brick 地板（y=0 水平面）+
// 同一相机/光照/材质；三行中文**共用同一 anchor (0,0,0)、同一 face (+X)、
// 同一 up (+Y)**，唯一变量是 alignment（kCenter / kBottomLeft / kBottomRight）。
//
// face=+X → 文字平面 = x=0 的 YZ 竖平面 → 与 y=0 地板面相交于一条水平线。
// 因此 kCenter 行的墨迹（y∈[-0.35,+0.35]）下半截穿过地板、被地板几何遮挡。
//
// 本场景的验证价值（两层，缺一不可）：
//   A. gold 字节比对：输出确定性（已验重跑 MD5 一致），任何渲染回归会被抓。
//   B. **结构性断言**（test 侧）：gold 只能证明"和上次一样"，不能证明
//      "深度遮挡真的发生"。故 test 会额外用 JPOV_NO_TEXT 渲一帧纯地板，
//      逐像素求差，断言：
//        - kCenter 行的可见高度 < 其几何完整高度（被地板切掉一截）
//        - 被切掉的部分在地板平面之下
//      若深度测试失效（文字整片浮在地板上），该断言失败。
//
// ⚠️ 画序铁律：先地板、后文字。文字是透明贴片（glDepthMask(GL_FALSE)
//    不写深度），若先画文字，后续地板会对着"清空后的最远深度"整片通过
//    并覆盖文字，测不出遮挡。
//
// ⚠️ 环境变量 JPOV_NO_TEXT=1：只画地板（供 test 做像素级 diff 对照）。

#ifndef JPOV_TEST_OBJECT3D_TEXT3D_DEPTH_ALPHA_COMMON_H_
#define JPOV_TEST_OBJECT3D_TEXT3D_DEPTH_ALPHA_COMMON_H_

#include <cstdlib>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/interface/mesh.h"

namespace jpov_text3d_depth_alpha {

// 输出/比对分辨率（与 other object3d gold 一致：渲染 1280x720，窗口 640x360）。
constexpr float kResW = 1280.0f;
constexpr float kResH = 720.0f;
constexpr int   kOutW = 640;
constexpr int   kOutH = 360;

inline const char* GetGoldName() {
    return "text3d_depth_alpha_1280x720.png";
}

// XZ 平放 quad（法线 +Y），UV 0~repeat 平铺 —— 与 repeated_mrquad 逐字一致，
// 保证地板外观/深度与既有 gold 可比。
inline jpov::MeshData BuildGroundQuad(float half_x, float half_z, float repeat) {
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

// 文字规格（generator 与 test 必须完全一致，否则字节比对红）。
// 放在公共头，杜绝两处手抄分叉（PR #60 曾因此踩坑）。
inline constexpr const char* kFontAlias = "cjk";
inline constexpr float kFontHeightWorld = 1.2f;
inline constexpr float kAnchorY = 0.0f;

}  // namespace jpov_text3d_depth_alpha

// 场景 app：generator 与 test 共用一个类，保证帧完全一致。
class Text3dDepthAlphaApp : public JPOV {
public:
    using JPOV::JPOV;

    void SetTextureIds(uint32_t base, uint32_t normal) {
        tex_base_color_ = base; tex_normal_ = normal;
    }

    void OneIteration(int64_t, const jpov::InputSnapshot&,
                      const jpov::WindowInfo&,
                      jpov::RenderCommandList* cmds) override {
        using namespace jpov_text3d_depth_alpha;

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

        const bool no_text  = std::getenv("JPOV_NO_TEXT") != nullptr;
        const bool no_floor = std::getenv("JPOV_NO_FLOOR") != nullptr;
        // 1) 先画地板（写深度，作为文字遮挡的参照）。
        //    JPOV_NO_FLOOR 时不画（供 test 得到“文字完整”的基准）。
        if (!no_floor) {
            cmds->DrawObject3D(mesh_id, mat,
                               {0.0f, 0.0f, 0.0f},
                               {0.0f, 1.0f, 0.0f},
                               {0.0f, 0.0f, 1.0f});
        }

        // JPOV_NO_TEXT：只画地板，不画文字（供 test 做像素级 diff）。
        if (no_text) {
            return;
        }

        // 2) 三行文字：统一 anchor / face / up，唯一变量 alignment。
        const jpov::Vec3f anchor_all = {0.0f, kAnchorY, 0.0f};
        const jpov::Vec3f face_pos_x = {1.0f, 0.0f, 0.0f};
        const jpov::Vec3f up_world_y = {0.0f, 1.0f, 0.0f};

        cmds->DrawText3D("center", anchor_all, face_pos_x, up_world_y,
                         kFontHeightWorld, {1.0f, 1.0f, 0.2f, 1.0f}, kFontAlias,
                         jpov::TextAlignment::kCenter);
        cmds->DrawText3D("左下", anchor_all, face_pos_x, up_world_y,
                         kFontHeightWorld, {0.2f, 1.0f, 0.4f, 1.0f}, kFontAlias,
                         jpov::TextAlignment::kBottomLeft);
        cmds->DrawText3D("右下", anchor_all, face_pos_x, up_world_y,
                         kFontHeightWorld, {0.3f, 0.7f, 1.0f, 1.0f}, kFontAlias,
                         jpov::TextAlignment::kBottomRight);
    }

private:
    uint32_t tex_base_color_ = 0, tex_normal_ = 0;
};

// 场景搭建（相机/材质以外的资源）：须在 app.Init() 之后调用。
// 返回 app（已注册纹理；mesh 由 OneIteration 每帧注册，与 repeated_mrquad 一致）。
inline void BuildText3dDepthAlphaScene(Text3dDepthAlphaApp* app) {
    const std::string root = jpov::GetProjectRoot() + "tools/jpov/test/object3d/";
    jpov::TextureOptions tex_opt;
    tex_opt.repeat = true;
    tex_opt.mipmap = true;
    const uint32_t base   = app->RegisterTexture(root + "brick_seamless_512x512.png", tex_opt);
    const uint32_t normal = app->RegisterTexture(root + "brick_normal_512x512.png", tex_opt);
    app->SetTextureIds(base, normal);
}

#endif  // JPOV_TEST_OBJECT3D_TEXT3D_DEPTH_ALPHA_COMMON_H_
