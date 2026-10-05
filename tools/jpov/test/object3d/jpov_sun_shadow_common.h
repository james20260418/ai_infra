// JPOV 太阳影子 gold test —— 共享场景构建（覆盖级联边界版）
//
// 验证 DirectionalLight（太阳平行光）+ 级联 shadow map（CSM）的实现。
// 场景（y-up 世界，地面 XZ 平面）：
//   - 大片地面：大 box（MakeBox），纯白，顶面贴 y=0，向四周铺开很远。
//   - 若干立柱：竖直 box，纯红，立在离相机由近及远的不同距离处（跨越级联距离带
//     8/24/56/152m 附近的边界），使一张 gold 就能看清「近处 → 远处」影子颗粒度的
//     渐变与级联混合带的过渡。
//   - 太阳 direction=(-1,-1,0.25)：斜上方偏 -x 照，立柱在地面上投出横向长影。
//
// 相机近乎贴地、朝 +z 平视，让远近立柱在地平面上依次排开（近大远小）。
//   ⇒ 画面纵向 ≈ 从近级联（C0）扫到远级联（C3/C4），级联边界落在画面中。
//
// 本头文件被 generator/test 共用，保证两边场景几何/光照完全一致。

#ifndef JPOV_TEST_OBJECT3D_JPOV_SUN_SHADOW_COMMON_H_
#define JPOV_TEST_OBJECT3D_JPOV_SUN_SHADOW_COMMON_H_

#include "tools/jpov/include/jpov/jpov.h"

namespace jpov_sun_shadow {

class SunShadowApp : public JPOV {
public:
    using JPOV::JPOV;

    uint32_t ground_mesh_ = 0;
    uint32_t pillar_mesh_ = 0;

    // 立柱世界位置（底部贴地 y=0）。由近及远排布，跨越级联边界
    // （cascade_ranges 默认 8/24/56/152/440），x 交错以免前后遮挡。
    static constexpr int kPillarCount = 4;
    jpov::Vec3f pillars_[kPillarCount] = {
        {  0.0f, 1.0f,   5.0f},
        {  5.0f, 1.0f,  16.0f},
        { -5.0f, 1.0f,  38.0f},
        {  8.0f, 1.0f,  85.0f},
    };

    void OneIteration(int64_t frame_count,
                      const jpov::InputSnapshot& input,
                      const jpov::WindowInfo& winfo,
                      jpov::RenderCommandList* cmds) override {
        (void)frame_count; (void)input; (void)winfo;

        const float kResW = 1280.0f;
        const float kResH = 720.0f;
        cmds->camera.fbo_3d_width_  = kResW;
        cmds->camera.fbo_3d_height_ = kResH;

        // 相机稍高、略俯：地平面填满画面，远近立柱依次排开（近大远小）。
        cmds->camera.position = {0.0f, 14.0f, -16.0f};
        cmds->camera.target   = {0.0f,  0.0f,  14.0f};
        cmds->camera.up       = {0.0f,  1.0f,   0.0f};
        cmds->camera.near     = 0.05f;

        // 太阳平行光：direction=(-1,-0.4,0.12)，低仰角（~22°）斜照，投出较长的横向影
        // （能跨过近处级联边界 8m / 24m）。
        cmds->sun = jpov::DirectionalLight{
            /*direction*/ {-1.0f, -0.4f, 0.12f},
            /*color*/ {1.0f, 1.0f, 1.0f, 1.0f},
            /*intensity*/ 3.0f,
        };

        // Ambient: 暗
        cmds->ambient= jpov::AmbientLight{
            .color = {1,1,1,1},
            .intensity = 0.3f
        };

        cmds->tone_mapping = true;

        // 地面：扁 box，白色。center=(0,-0.1,0) 使顶面贴 y=0。
        cmds->DrawObject3D(
            ground_mesh_,
            jpov::PBRMaterial::SolidColor(jpov::kColorWhite),
            {0.0f, -0.1f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f});

        // 立柱：竖直 box，红色。center y=1 使底部贴 y=0。
        for (const jpov::Vec3f& p : pillars_) {
            cmds->DrawObject3D(
                pillar_mesh_,
                jpov::PBRMaterial::SolidColor(jpov::kColorRed),
                p, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f});
        }
    }
};

}  // namespace jpov_sun_shadow

#endif  // JPOV_TEST_OBJECT3D_JPOV_SUN_SHADOW_COMMON_H_
