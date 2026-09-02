// JPOV Gold Image Generator — glTF Lantern (GLB) PBR 全通道渲染
//
// 生成 jpov_gltf_lantern_gold_test 的参考图。
//
// 用 Khronos 官方 Lantern.glb（贴图内嵌单文件 GLB，3 primitive，PBR 全通道：
// baseColor + normal + metallic/roughness(ORM) + emissive，无独立 AO）端到端
// 验证：
//   - JPOV::LoadGltf(path) 加载 glTF/GLB（GLB 内嵌 bufferView 贴图自动解出）
//   - ORM 拆包（metallic/roughness）、AO 来源规范（无独立 occlusion → ao=1）
//   - emissive 贴图加载
//   - RenderCommandList::DrawGltfObject 渲染
//
// 输出: tools/jpov/test/object3d/lantern_1280x720.png
// 用途: 内嵌贴图 GLB 加载链路端到端验证 + 供 leader 肉眼判断。

#include <cstdint>
#include <string>

#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/interface/gltf_object.h"
#include "tools/jpov/test/test_utils.h"

class GltfLanternGoldGenerator : public JPOV {
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

        // 相机：3/4 视角（同 pliers test 的三光源风格），适配 lantern 大包围盒
        // （±7.7 × ±12.8 × ±2.3，远高于 pliers 的 ~0.18）。距离放大到 34 能
        // 框住全貌；near 保持 0.01 防近裁剪。
        cmds->camera.position = {0.0f, 18.0f, 30.0f};
        cmds->camera.target   = {0.0f, 0.0f, 0.0f};
        cmds->camera.near     = 0.01f;

        // 三光源对称照明（同其它 PBR gold test）
        cmds->point_lights.push_back({
            {8.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f, 1.0f}, 0.8f, 0.01f, 1.0f});
        cmds->point_lights.push_back({
            {0.0f, 8.0f, 0.0f}, {1.0f, 1.0f, 1.0f, 1.0f}, 0.8f, 0.01f, 1.0f});
        cmds->point_lights.push_back({
            {0.0f, 0.0f, 8.0f}, {1.0f, 1.0f, 1.0f, 1.0f}, 0.8f, 0.01f, 1.0f});

        // Ambient: 适中
        cmds->ambient = jpov::AmbientLight{
            .color = {1, 1, 1, 1},
            .intensity = 0.5f
        };

        // 绘制整个 glTF 对象（3 个 primitive，内部展开为多个 DrawObject3D）
        cmds->DrawGltfObject(gltf_, {0.0f, 0.0f, 0.0f},
                             {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f});
    }

private:
    jpov::GltfObject gltf_;
};

int main() {
    std::string outpath =
        jpov::GetTestDataDir() + "/object3d/lantern_1280x720.png";

    JPOV::Config cfg;
    cfg.title = "glTF Lantern (GLB) PBR Gold Generator";
    cfg.headless = true;
    GltfLanternGoldGenerator app(cfg);
    app.Init();

    // 加载 lantern（内嵌贴图 GLB 单文件，PBR 全通道）
    jpov::GltfObject gltf = app.LoadGltf(
        jpov::GetTestDataDir() + "/object3d/lantern_glb/Lantern.glb");
    CHECK(!gltf.empty()) << "LoadGltf failed / empty";
    LOG(INFO) << "Loaded " << gltf.size() << " glTF primitives";
    app.SetGltfObject(std::move(gltf));

    jpov::WindowInfo winfo;
    winfo.width  = 640.0f;
    winfo.height = 360.0f;
    jpov::InputSnapshot input{};
    app.RunOnce(input, winfo, outpath.c_str());
    app.Finalize();

    LOG(INFO) << "glTF Lantern gold image generated: " << outpath;
    return 0;
}
