// JPOV picking + highlight gold test —— 共享场景构建
//
// 验证两个新接口：
//   1. GPU color-ID 拾取（Object3DCommand::picking_id + RenderCommandList::pick
//      + JPOV::last_pick()）
//   2. 高亮纯色边框（Object3DCommand::highlight + RenderCommandList::highlight_style，
//      方法 B：stencil + 顶点外扩）
//
// 场景（y-up 世界，地面 XZ 平面）：
//   - 中柱：origin 立着的红色 box，picking_id = 1
//   - 左柱：左侧蓝色 box，picking_id = 2
//   - 右柱：右侧绿色 box，picking_id = 3
//   - 地面：扁白 box，不可拾取（picking_id = 0）
// 相机 (0,4,0) 俯视原点，up=(0,0,1)。
//
// 测试可配置（OneIteration 前设置成员）：
//   - pick_enabled / pick_x / pick_y：本帧是否发起拾取查询及屏幕像素坐标
//   - highlight_id：picking_id 等于此值的物体本帧高亮（0 = 不高亮任何物体）
//
// 注：llvmpipe 下 PBR 光照三稳态非确定，test 不做逐像素光照比对；
//      高亮边框本身是确定的纯色，test 会做“高亮区域包含指定纯色”的像素检查。

#ifndef JPOV_TEST_OBJECT3D_JPOV_PICK_HIGHLIGHT_GOLD_COMMON_H_
#define JPOV_TEST_OBJECT3D_JPOV_PICK_HIGHLIGHT_GOLD_COMMON_H_

#include "tools/jpov/include/jpov/jpov.h"

namespace jpov_pick_highlight {

// picking_id 常量
inline constexpr uint32_t kIdCenter = 1;
inline constexpr uint32_t kIdLeft   = 2;
inline constexpr uint32_t kIdRight  = 3;

class PickHighlightApp : public JPOV {
public:
    using JPOV::JPOV;

    // mesh 句柄（generator/test 在 Init() 后填充）
    uint32_t ground_mesh_ = 0;
    uint32_t center_mesh_ = 0;
    uint32_t left_mesh_   = 0;
    uint32_t right_mesh_  = 0;

    // 拾取查询配置（OneIteration 前设置）
    bool   pick_enabled = false;
    float  pick_x       = 0.0f;
    float  pick_y       = 0.0f;

    // 高亮配置：picking_id 等于此值的 object 本帧高亮（0 = 无高亮）
    uint32_t highlight_id = 0;

    void OneIteration(int64_t frame_count,
                      const jpov::InputSnapshot& input,
                      const jpov::WindowInfo& winfo,
                      jpov::RenderCommandList* cmds) override {
        (void)frame_count; (void)input; (void)winfo;

        const float kResW = 1280.0f;
        const float kResH = 720.0f;
        cmds->camera.fbo_3d_width_  = kResW;
        cmds->camera.fbo_3d_height_ = kResH;

        // 相机 (0,4,0) 俯视原点，up=(0,0,1)（y-up 世界，从上向下看地面）。
        cmds->camera.position = {0.0f, 4.0f, 0.0f};
        cmds->camera.target   = {0.0f, 0.0f, 0.0f};
        cmds->camera.up       = {0.0f, 0.0f, 1.0f};
        cmds->camera.near     = 0.05f;

        // 点光源 + 环境光，照亮场景。
        cmds->point_lights.push_back(
            jpov::PointLight{.position = {0.0f, 6.0f, 0.0f},
                             .color = {1.0f, 1.0f, 1.0f, 1.0f},
                             .linear_radius = 10.0f,
                             .physical_radius = 0.0f,
                             .intensity = 1.0f});
        cmds->ambient = jpov::AmbientLight{.color = {1, 1, 1, 1},
                                           .intensity = 0.3f};

        // 高亮全局样式（统一颜色 + 线宽）。
        cmds->highlight_style = jpov::HighlightStyle{
            .color = {1.0f, 0.85f, 0.3f, 1.0f},   // 金黄色的框
            .outline_width = 0.08f,               // 放大 1.08 倍
        };

        // 地面：扁 box，不可拾取（picking_id 默认 0）。
        cmds->DrawObject3D(
            ground_mesh_,
            jpov::PBRMaterial::SolidColor(jpov::kColorWhite),
            {0.0f, -0.1f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f});

        // 中柱：原点红色 box，picking_id = kIdCenter。
        cmds->DrawObject3D(
            center_mesh_,
            jpov::PBRMaterial::SolidColor(jpov::kColorRed),
            {0.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f},
            /*picking_id=*/kIdCenter,
            /*highlight=*/(highlight_id == kIdCenter));

        // 左柱：左侧蓝色 box，picking_id = kIdLeft。
        cmds->DrawObject3D(
            left_mesh_,
            jpov::PBRMaterial::SolidColor(jpov::kColorBlue),
            {-2.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f},
            /*picking_id=*/kIdLeft,
            /*highlight=*/(highlight_id == kIdLeft));

        // 右柱：右侧绿色 box，picking_id = kIdRight。
        cmds->DrawObject3D(
            right_mesh_,
            jpov::PBRMaterial::SolidColor(jpov::kColorGreen),
            {2.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f},
            /*picking_id=*/kIdRight,
            /*highlight=*/(highlight_id == kIdRight));

        // 拾取查询（enabled 由测试控制）。
        cmds->pick.enabled   = pick_enabled;
        cmds->pick.screen_x  = pick_x;
        cmds->pick.screen_y  = pick_y;
    }
};

}  // namespace jpov_pick_highlight

#endif  // JPOV_TEST_OBJECT3D_JPOV_PICK_HIGHLIGHT_GOLD_COMMON_H_
