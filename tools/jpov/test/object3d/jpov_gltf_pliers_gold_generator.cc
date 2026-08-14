// JPOV Gold Image Generator — glTF Pliers PBR 全通道渲染
//
// 生成 jpov_gltf_pliers_gold_test 的参考图。
//
// 本 generator 演示新的 glTF 加载 API：
//   - JPOV::LoadGltf(path)  → GltfObject（内部：纯净 loader + MeshManager/
//     TextureManager 上传 + 多 mesh + 贴图去重 + ORM 拆包）
//   - RenderCommandList::DrawGltfObject(obj, center, up, front)  渲染
//   - JPOV::ReleaseGltf(obj) 整体释放 GPU 资源
//
// pliers.gltf 有 3 个 mesh（handle_02_low + handle_01_low + center_low），
// 共享同一套贴图（diff / normal_gl / arm-ORM）。本测试渲染完整的 3 个 mesh，
// 验证 glTF loader 的多 primitive 加载 + PBR 全通道渲染。
//
// 输出: tools/jpov/test/object3d/pliers_1280x720.png
// 用途: glTF loader 端到端验证 + 供 leader 肉眼判断 loader/材质正确性。

#include <cstdint>
#include <string>

#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/interface/gltf_object.h"

class GltfPliersGoldGenerator : public JPOV {
public:
    using JPOV::JPOV;

    void SetGltfObject(jpov::GltfObject obj) { gltf_ = std::move(obj); }

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

        // 相机：3/4 视角完整看 pliers（长轴沿 Z，长度 ~0.18），
        // 距离适中（~0.11）既能看清全部细节又不裁切边缘。
        // near 设小到 0.01：模型极小（0.18 长），相机距中心仅 ~0.11，
        // 默认 near=0.1 会切掉朝相机一侧的钳口/把手，露出内部中空。
        const float cx = 0.0075f;
        const float cz = -0.04f;
        cmds->camera.position = {0.075f, 0.08f, cz + 0.045f};
        cmds->camera.target   = {cx, 0.0f, cz};
        cmds->camera.near     = 0.01f;

        // 三光源对称照明（同其它 PBR gold test）
        cmds->point_lights.push_back({
            {0.15f, 0.0f, 0.0f}, {2.0f, 2.0f, 2.0f, 1.0f}, 0.5f, 0.01f});
        cmds->point_lights.push_back({
            {0.0f, 0.15f, 0.0f}, {2.0f, 2.0f, 2.0f, 1.0f}, 0.5f, 0.01f});
        cmds->point_lights.push_back({
            {0.0f, 0.0f, 0.15f}, {2.0f, 2.0f, 2.0f, 1.0f}, 0.5f, 0.01f});

        // 绘制整个 glTF 对象（内部展开为多个 DrawObject3D，无新命令体）
        cmds->DrawGltfObject(gltf_, {0.0f, 0.0f, 0.0f},
                             {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f});
    }

private:
    jpov::GltfObject gltf_;
};

int main() {
    const char* outpath =
        "/james_pm/ai_infra/tools/jpov/test/object3d/pliers_1280x720.png";

    JPOV::Config cfg;
    cfg.title = "glTF Pliers PBR Gold Generator";
    cfg.headless = true;
    GltfPliersGoldGenerator app(cfg);
    app.Init();

    // 加载 pliers 场景（内部上传 mesh + 贴图，含 ORM 拆包）
    jpov::GltfObject gltf = app.LoadGltf(
        "/james_pm/ai_infra/tools/jpov/test/object3d/pliers_gltf/pliers.gltf");
    CHECK(!gltf.empty()) << "LoadGltf failed / empty";
    LOG(INFO) << "Loaded " << gltf.size() << " glTF primitives";
    app.SetGltfObject(std::move(gltf));

    jpov::WindowInfo winfo;
    winfo.width  = 640.0f;
    winfo.height = 360.0f;
    jpov::InputSnapshot input{};
    app.RunOnce(input, winfo, outpath);
    app.Finalize();

    LOG(INFO) << "glTF pliers gold image generated: " << outpath;
    return 0;
}
