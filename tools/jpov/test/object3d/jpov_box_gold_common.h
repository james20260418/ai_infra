// JPOV box gold test —— 共享场景构建
//
// 验证 MeshData::MakeBox（空间轴对齐盒构造）与 PBRMaterial::SolidColor /
// SolidColorMR（纯色材质便捷构造）两个新增接口。
//
// 场景（y-up 世界，地面 XZ 平面）：
//   - 地面：扁 box（MakeBox, center 下移一半厚，front=+z up=+y），纯白
//   - 立柱：竖直 box（MakeBox, up=+y 高 2 米，front=+z），纯红（SolidColor）
//   - 斜柱：斜放 box（MakeBox, front 斜向），纯蓝（SolidColorMR, metallic=0.8）
//   - 点光源 (0,4,0) 俯视照
// 相机 (0,5,0) 俯视原点，up=(0,0,1)。
//
// 本头文件被 generator/test 共用，保证两边场景几何一致。
//
// 注：llvmpipe 下 PBR 光照三稳态非确定（leader #16 决策），test 只做
// smoke check + 校验 gold image 存在，不做逐像素比对。

#ifndef JPOV_TEST_OBJECT3D_JPOV_BOX_GOLD_COMMON_H_
#define JPOV_TEST_OBJECT3D_JPOV_BOX_GOLD_COMMON_H_

#include "tools/jpov/include/jpov/jpov.h"

namespace jpov_box {

class BoxApp : public JPOV {
public:
    using JPOV::JPOV;

    uint32_t ground_mesh_ = 0;
    uint32_t pillar_mesh_ = 0;
    uint32_t slanted_mesh_ = 0;

    void OneIteration(int64_t frame_count,
                      const jpov::InputSnapshot& input,
                      const jpov::WindowInfo& winfo,
                      jpov::RenderCommandList* cmds) override {
        (void)frame_count; (void)input; (void)winfo;

        const float kResW = 1280.0f;
        const float kResH = 720.0f;
        cmds->camera.fbo_3d_width_  = kResW;
        cmds->camera.fbo_3d_height_ = kResH;

        // 相机 (0,5,0) 俯视原点，up=(0,0,1)（y-up 世界，从上向下看地面）。
        cmds->camera.position = {-1.0f, 5.0f, 1.0f};
        cmds->camera.target   = {0.0f, 0.0f, 0.0f};
        cmds->camera.up       = {0.0f, 0.0f, 1.0f};
        cmds->camera.near     = 0.05f;

        // 点光源 (0,4,0)，照亮整个场景。
        cmds->point_lights.push_back(
            jpov::PointLight{
            .position = {-5.0f, 4.0f, 0.0f},
            .color= {1.0f, 1.0f, 1.0f, 1.0f},
            .linear_radius= 8.0f, 
            .physical_radius = .0f, 
            .intensity = 1.0f});
        
        // Ambient: 亮
        cmds->ambient= jpov::AmbientLight{
            .color = {1,1,1,1},
            .intensity = 0.25f
        };

        // 地面：扁 box（白色）。MakeBox 只给尺寸（局部 +Z=front, +Y=up, +X=left），
        // 摆放靠 Draw 的 center/up/front。center 下移 0.1 使顶面贴 y=0。
        cmds->DrawObject3D(
            ground_mesh_,
            jpov::PBRMaterial::SolidColor(jpov::kColorWhite),
            {0.0f, -0.1f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f});

        // 立柱：竖直 box（红色）。center=(0,1,0) 使底部贴 y=0、高 2 米。
        cmds->DrawObject3D(
            pillar_mesh_,
            jpov::PBRMaterial::SolidColor(jpov::kColorRed),
            {0.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f});

        // 斜柱：斜放 box（蓝色 + metallic=0.8），front 斜向 (1,0,1) 由 Draw 给定。
        cmds->DrawObject3D(
            slanted_mesh_,
            jpov::PBRMaterial::SolidColorMR(jpov::kColorBlue, 0.8f, 0.3f),
            {-1.5f, 0.7f, -1.0f}, {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f, 1.0f});
    }
};

}  // namespace jpov_box

#endif  // JPOV_TEST_OBJECT3D_JPOV_BOX_GOLD_COMMON_H_
