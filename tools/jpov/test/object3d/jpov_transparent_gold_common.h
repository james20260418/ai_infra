// JPOV 透明绘制 gold —— 共享场景（generator / test 共用，保证两边几何一致）
//
// 验证 `AlphaMode::kBlend`（加权混合 OIT）的「**透明盒前后遮挡**」经典用例：
//
//   - 地面（白）：扁 box。
//   - 透明玻璃盒（kBlend，青绿，双面薄壳）：包住一个不透明红盒。
//   - 透明盒**外**另放一个不透明红盒（无玻璃遮挡）作对照。
//
// 覆盖点：
//   1. 玻璃**前脸**在不透明红盒之前 → 隔着它看红盒应被染青绿（透过率 + 加权色）。
//   2. 玻璃**后脸**在不透明红盒之后 → 被遮挡 → 手动深度测试应把它丢弃（不写 WBO）。
//   3. 盒外红盒 → 玻璃前无遮挡 → 保持本色（对照）。
//
// 相机固定；`tone_mapping=true`（kBlend 仅 HDR 路径支持，见 renderer 的 CHECK）。
//
// 本头文件被 generator / test 共用 ⇒ 两边场景逐字节一致（zero 分叉）。

#ifndef JPOV_TEST_OBJECT3D_JPOV_TRANSPARENT_GOLD_COMMON_H_
#define JPOV_TEST_OBJECT3D_JPOV_TRANSPARENT_GOLD_COMMON_H_

#include "tools/jpov/include/jpov/jpov.h"

namespace jpov_transparent {

class TransparentApp : public JPOV {
public:
    using JPOV::JPOV;

    uint32_t ground_mesh_ = 0;   // 地面（白扁 box）
    uint32_t solid_mesh_ = 0;    // 不透明红盒（盒内 + 盒外各画一次）
    uint32_t glass_mesh_ = 0;    // 透明玻璃盒（kBlend）

    // 玻璃材质：青绿半透明、双面薄壳（front+back 都进 WBO）。
    jpov::PBRMaterial glass_mat_ = [] {
        jpov::PBRMaterial m;
        m.base_color = jpov::Color{0.12f, 0.46f, 0.52f, 0.35f};  // 青绿玻璃
        m.metallic = 0.0f;
        m.roughness = 0.10f;
        m.alpha_mode = jpov::AlphaMode::kBlend;
        m.double_sided = true;
        return m;
    }();

    void OneIteration(int64_t frame_count,
                      const jpov::InputSnapshot& input,
                      const jpov::WindowInfo& winfo,
                      jpov::RenderCommandList* cmds) override {
        (void)frame_count;
        (void)input;
        (void)winfo;

        cmds->camera.fbo_3d_width_ = 1280.0f;
        cmds->camera.fbo_3d_height_ = 720.0f;
        cmds->camera.position = {3.6f, 2.6f, 5.2f};
        cmds->camera.target = {0.0f, 0.9f, 0.0f};
        cmds->camera.up = {0.0f, 1.0f, 0.0f};
        cmds->camera.fov = 45.0f;
        cmds->camera.near = 0.05f;
        cmds->camera.far = 100.0f;
        cmds->tone_mapping = true;  // 玻璃 pass 需线性 HDR 域

        cmds->ambient = jpov::AmbientLight{jpov::Color{1.0f, 1.0f, 1.0f, 1.0f},
                                           0.55f};
        cmds->point_lights.push_back(jpov::PointLight{
            /*position=*/jpov::Vec3f(3.0f, 6.0f, 4.0f),
            /*color=*/jpov::Color{1.0f, 1.0f, 1.0f, 1.0f},
            /*linear_radius=*/30.0f,
            /*physical_radius=*/0.0f,
            /*intensity=*/1.0f});

        // 地面：扁 box（白），顶面贴 y=0。
        cmds->DrawObject3D(ground_mesh_,
                           jpov::PBRMaterial::SolidColor(jpov::kColorWhite),
                           /*center*/ {0.0f, -0.1f, 0.0f},
                           /*up*/ {0.0f, 1.0f, 0.0f},
                           /*front*/ {0.0f, 0.0f, 1.0f});

        // 透明玻璃盒（kBlend）：包住盒内红盒。
        cmds->DrawObject3D(glass_mesh_, glass_mat_,
                           /*center*/ {0.0f, 0.9f, 0.0f},
                           /*up*/ {0.0f, 1.0f, 0.0f},
                           /*front*/ {0.0f, 0.0f, 1.0f});

        // 盒内不透明红盒（被玻璃前脸染青绿、玻璃后脸被它遮挡而丢弃）。
        cmds->DrawObject3D(solid_mesh_,
                           jpov::PBRMaterial::SolidColor(jpov::kColorRed),
                           /*center*/ {0.0f, 0.9f, 0.0f},
                           /*up*/ {0.0f, 1.0f, 0.0f},
                           /*front*/ {0.0f, 0.0f, 1.0f});

        // 盒外不透明红盒（玻璃前无遮挡，对照）。
        cmds->DrawObject3D(solid_mesh_,
                           jpov::PBRMaterial::SolidColor(jpov::kColorRed),
                           /*center*/ {2.2f, 0.4f, 0.0f},
                           /*up*/ {0.0f, 1.0f, 0.0f},
                           /*front*/ {0.0f, 0.0f, 1.0f});
    }
};

}  // namespace jpov_transparent

#endif  // JPOV_TEST_OBJECT3D_JPOV_TRANSPARENT_GOLD_COMMON_H_
