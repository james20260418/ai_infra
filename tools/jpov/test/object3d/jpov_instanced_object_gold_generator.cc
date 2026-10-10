// JPOV 静态批量实例（DrawInstancedObject）gold image generator
//
// 渲染「地面 + 太阳 + 3 份 lace_skirt 实例」场景，写 gold image：
//   tools/jpov/test/object3d/instanced_object_1280x720.png
// 输出 640x360（同其它 gold 惯例）。
//
// 被实例化的网格来自载入的 glb（1 个 primitive）；用新 API
// RenderCommandList::DrawInstancedObject(mesh_id, material, instances) 一次画 3 份。

#include <string>

#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/interface/gltf_object.h"
#include "tools/jpov/test/object3d/jpov_instanced_object_gold_common.h"
#include "tools/jpov/test/test_utils.h"

int main() {
    const std::string outpath =
        jpov::GetTestDataDir() + "/object3d/instanced_object_1280x720.png";

    JPOV::Config cfg;
    cfg.title = "JPOV Instanced Object Gold Generator";
    cfg.headless = true;
    jpov_instanced_object::InstancedObjectApp app(cfg);
    app.Init();

    // 地面：扁 box（120×120 米开阔地面），顶面贴 y=0。
    app.ground_mesh_ = app.RegisterMesh(jpov::MeshData::MakeBox(
        /*front_half_width*/ 60.0f, /*up_half_width*/ 0.1f,
        /*left_half_width*/ 60.0f));

    // 被实例化的网格：lace_skirt（MASK + doubleSided，单 primitive）。
    jpov::GltfObject gltf = app.LoadGltf(
        jpov::GetModelsDir() + "/clothing/lace_skirt/lace_skirt_skinned_sample.glb");
    CHECK(!gltf.empty()) << "LoadGltf failed / empty";
    CHECK_EQ(gltf.size(), 1u) << "lace_skirt 应为单 primitive，got " << gltf.size();
    app.cloth_mesh_ = gltf.primitives[0].mesh_id;
    app.cloth_material_ = gltf.primitives[0].material;

    jpov::WindowInfo winfo;
    winfo.width  = 640.0f;
    winfo.height = 360.0f;
    jpov::InputSnapshot input{};
    app.RunOnce(input, winfo, outpath.c_str());
    app.Finalize();

    LOG(INFO) << "instanced_object gold image generated: " << outpath;
    return 0;
}
