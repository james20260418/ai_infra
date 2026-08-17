// JPOV 太阳影子（最简版）gold test —— 共享场景构建
//
// 验证 DirectionalLight（太阳平行光）+ 正交 shadow map 的最小实现。
// 场景（y-up 世界，地面 XZ 平面）：
//   - 平板：扁 box（MakeBox），纯白，铺地面
//   - 立柱：竖直 box（MakeBox），纯红，立在平板中央
//   - 太阳 direction=(-1,-1,1)：从斜上方照，立柱在平板上投下影子
//
// 相机 (0,5,0) 俯视原点，up=(0,0,1)。
// 本头文件被 generator/test 共用，保证场景几何一致。

#ifndef JPOV_TEST_OBJECT3D_JPOV_SUN_SHADOW_COMMON_H_
#define JPOV_TEST_OBJECT3D_JPOV_SUN_SHADOW_COMMON_H_

#include "tools/jpov/include/jpov/jpov.h"

namespace jpov_sun_shadow {

class SunShadowApp : public JPOV {
public:
    using JPOV::JPOV;

    uint32_t ground_mesh_ = 0;
    uint32_t pillar_mesh_ = 0;

    void OneIteration(int64_t frame_count,
                      const jpov::InputSnapshot& input,
                      const jpov::WindowInfo& winfo,
                      jpov::RenderCommandList* cmds) override {
        (void)frame_count; (void)input; (void)winfo;

        const float kResW = 1280.0f;
        const float kResH = 720.0f;
        cmds->camera.fbo_3d_width_  = kResW;
        cmds->camera.fbo_3d_height_ = kResH;

        // 相机 (0,5,0) 俯视原点，up=(0,0,1)。
        cmds->camera.position = {0.0f, 5.0f, 0.0f};
        cmds->camera.target   = {0.0f, 0.0f, 0.0f};
        cmds->camera.up       = {0.0f, 0.0f, 1.0f};
        cmds->camera.near     = 0.05f;

        // 太阳平行光：direction=(-1,-1,1)，从斜上方（偏 -x、+z）照射。
        cmds->sun = jpov::DirectionalLight{
            /*direction*/ {-1.0f, -1.0f, 1.0f},
            /*color*/ {1.0f, 1.0f, 1.0f, 1.0f},
            /*intensity*/ 3.0f,
        };

        // 平板：扁 box，白色。center=(0,-0.1,0) 使顶面贴 y=0。
        cmds->DrawObject3D(
            ground_mesh_,
            jpov::PBRMaterial::SolidColor(jpov::kColorWhite),
            {0.0f, -0.1f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f});

        // 立柱：竖直 box，红色。center=(0,1,0) 使底部贴 y=0、高 2 米。
        cmds->DrawObject3D(
            pillar_mesh_,
            jpov::PBRMaterial::SolidColor(jpov::kColorRed),
            {0.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f});
    }
};

}  // namespace jpov_sun_shadow

#endif  // JPOV_TEST_OBJECT3D_JPOV_SUN_SHADOW_COMMON_H_
