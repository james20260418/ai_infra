// JPOV Repro Generator — 纯色立方体(cube_hand.obj)缺面复现
//
// 用 object_use_default_color=true 走纯色渲染路径，加载 cube_hand.obj
// （该 OBJ 6 面中 3 面绕序反转，见 winding_probe 诊断），观察缺面现象。
// 输出: /james_pm/ai_infra/tools/jpov/test/repro_purecolor_cube_1280x720.png

#include <cstdio>
#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/src/obj_loader.h"
#include "tools/common/utils.h"

class ReproPureCube : public JPOV {
public:
    using JPOV::JPOV;

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

        // Camera 从右前上方观察立方体中心（与 PBR 一致，能看到三个面）
        cmds->camera.position = {1.9f, 1.3f, 1.9f};
        cmds->camera.target   = {0.5f, 0.5f, 0.5f};

        // 加载 cube_hand.obj → 纯色渲染
        std::string obj_path = jpov::GetProjectRoot() +
                               "tools/jpov/test/cube_hand.obj";
        jpov::MeshData mesh;
        CHECK(jpov::LoadObj(obj_path, &mesh)) << "Failed to load cube_hand.obj";
        uint32_t mesh_id = RegisterMesh(mesh);

        // 纯色模式（object_use_default_color=true → 走 Solid3DProg）
        jpov::PBRMaterial mat;
        mat.base_color = {0.3f, 0.6f, 1.0f, 1.0f};  // 淡蓝
        cmds->object_use_default_color = true;
        cmds->DrawObject3D(
            mesh_id, mat,
            {0.5f, 0.5f, 0.5f},          // center = 立方体中心
            {0.0f, 1.0f, 0.0f},          // up = +Y
            {0.0f, 0.0f, 1.0f});         // front = +Z
    }
};

int main() {
    const char* outpath =
        "/james_pm/ai_infra/tools/jpov/test/repro_purecolor_cube_1280x720.png";

    JPOV::Config cfg;
    cfg.title = "Repro PureColor Cube";
    cfg.headless = true;
    ReproPureCube app(cfg);
    app.Init();

    jpov::WindowInfo winfo;
    winfo.width  = 640.0f;
    winfo.height = 360.0f;

    jpov::InputSnapshot input{};
    app.RunOnce(input, winfo, outpath);
    app.Finalize();

    LOG(INFO) << "repro image generated: " << outpath;
    return 0;
}
