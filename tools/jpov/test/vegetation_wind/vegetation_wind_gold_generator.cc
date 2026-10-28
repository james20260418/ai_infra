// 植被风 gold image 生成器（写仓库里的基准图）。
//
// 用法（**只在故意改观感/风公式时跑**，跑完把新图一起提交）：
//   bazel run //tools/jpov/test/vegetation_wind:vegetation_wind_gold_generator
//
// 产物（场景见 vegetation_wind_gold_common.h，与 test 共用）：
//   vegetation_wind_instance_1280x720.png       instance pass：三棵树 + 风
//   vegetation_wind_instance_blow_1280x720.png  instance pass：+ 逐实例吹倒
//   vegetation_wind_object3d_1280x720.png       object3d 路径：单棵树 + 风

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/test/test_utils.h"
#include "tools/jpov/test/vegetation_wind/vegetation_wind_gold_common.h"

int main() {
    const std::string dir = jpov::GetTestDataDir() + "/vegetation_wind/";
    std::system(("mkdir -p " + dir).c_str());

    // 现场烘一份树干（无叶子）：关节表 + 逐顶点 (joint_id, phase) 都在产物里。
    // glb 是中间产物，写 output/ 不写 gold 目录（gold 目录只放基准图）。
    const std::string work_dir = jpov::GetOutputDir() + "vegetation_wind_gold/";
    std::system(("mkdir -p " + work_dir).c_str());
    const std::string glb = work_dir + "plant.glb";
    std::string bake_error;
    if (!vegwind::BakeSamplePlant(vegwind::SampleAssetDir(), glb, &bake_error)) {
        std::fprintf(stderr, "烘焙测试资产失败: %s\n", bake_error.c_str());
        return 1;
    }

    JPOV::Config cfg;
    cfg.title = "JPOV Vegetation Wind Gold Generator";
    cfg.headless = true;
    vegwind::WindSceneApp app(cfg);
    app.Init();
    app.ground_mesh_ = app.RegisterMesh(jpov::MeshData::MakeBox(
        /*front_half_width*/ 60.0f, /*up_half_width*/ 0.1f,
        /*left_half_width*/ 60.0f));
    if (!app.LoadPlant(glb)) {
        std::fprintf(stderr, "载入烘焙产物失败: %s\n", glb.c_str());
        app.Finalize();
        return 1;
    }

    jpov::WindowInfo winfo;
    winfo.width = 1280.0f;
    winfo.height = 720.0f;
    jpov::InputSnapshot input{};

    const std::vector<vegwind::NamedShot> shots = vegwind::AllShots();
    for (const vegwind::NamedShot& named : shots) {
        const std::string path = dir + named.name + ".png";
        app.SetShot(named.shot);
        app.RunOnce(input, winfo, path.c_str());
        std::printf("gold: %s\n", path.c_str());
    }

    app.ReleasePlant();
    app.Finalize();
    return 0;
}
