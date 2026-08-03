// JPOV Gold Image Generator — 3D Object3D 静态模型渲染 gold image
//
// 生成 object3d gold test 的参考图片：
//   - 从 OBJ 文件加载 beetle 模型 → LoadObj → MeshData → RegisterMesh
//   - DrawObject3D 纯色渲染（texture_id=0），center=原点，up=(0,1,0)，front=(0,0,1)
//   - Camera (1, 1, 1) 看向原点，透视投影
//   - 渲染分辨率 1280x720（主 FBO 2x，MSAA 抗锯齿）
//
// 输出: tools/jpov/test/object3d_beetle_1280x720.png

#include <cstdio>
#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/src/obj_loader.h"
#include "tools/common/utils.h"

class Object3DGoldGenerator : public JPOV {
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

        // ---- 设置 Camera ----
        cmds->camera.position = {1.0f, 1.0f, 1.0f};
        cmds->camera.target   = {0.0f, 0.0f, 0.0f};

        // ---- 加载 OBJ 模型 → 注册 GPU mesh ----
        std::string obj_path = jpov::GetProjectRoot() +
                               "tools/jpov/test/beetle.obj";
        jpov::MeshData mesh;
        CHECK(jpov::LoadObj(obj_path, &mesh)) << "Failed to load beetle.obj";

        uint32_t mesh_id = RegisterMesh(mesh);
        uint32_t texture_id = 0;  // 纯色渲染

        // ---- 渲染模型（中心在原点，自然朝向） ----
        cmds->DrawObject3D(
            mesh_id, texture_id,
            {0.2f, 0.6f, 1.0f, 1.0f},  // 淡蓝色
            {0.0f, 0.0f, 0.0f},          // center = 原点
            {0.0f, 1.0f, 0.0f},          // up = +Y
            {0.0f, 0.0f, 1.0f});         // front = +Z
    }
};

int main() {
    const char* outpath =
        "/james_pm/ai_infra/tools/jpov/test/object3d_beetle_1280x720.png";

    JPOV::Config cfg;
    cfg.title = "3D Object3D Gold Generator";
    cfg.headless = true;
    Object3DGoldGenerator app(cfg);
    app.Init();

    jpov::WindowInfo winfo;
    winfo.width  = 640.0f;
    winfo.height = 360.0f;

    jpov::InputSnapshot input{};
    app.RunOnce(input, winfo, outpath);
    app.Finalize();

    LOG(INFO) << "Gold image generated: " << outpath;
    return 0;
}
