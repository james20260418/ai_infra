// 植被风（单关节）gold image test
//
// 测什么：顶点风在两条绘制路径上必须「动」且「稳定」——
//   · 与 gold 逐像素比对（阈值内）⇒ 公式/属性布局没有被悄悄改坏；
//   · 与同机位「关风」对照组比对必须**显著不同** ⇒ 风确实施加了位移
//     （否则一张"什么都不动"的图也能和 gold 一致，测试形同虚设）。
//
// gold 由 vegetation_wind_gold_generator 出（同场景、同顺序，见
// vegetation_wind_gold_common.h）。阈值口径与实测差见 kExpectations 注释。

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/test/compare_light.h"
#include "tools/jpov/test/test_utils.h"
#include "tools/jpov/test/vegetation_wind/vegetation_wind_gold_common.h"

namespace {

// 一张 gold 的验收口径。
//
// 阈值口径：llvmpipe 下同二进制重复运行逐像素一致（实测 0），所以 gold 卡得住小改动；
// 但风位移会把轮廓整体推几像素，任何"公式等价但实现不同"的重构都会在轮廓上留痕，
// 因此不去死磕 max_diff=0，而是限定「显著变化像素比例」——它抓得住"整棵树塌了/
// 不吹了/被撕成薄片"，又容得下亚像素级的轮廓抖动。
//
// 0.5% 预算的来历：本测试首次落地前的"单关节化"改造（两关节混合 → 单关节）实测
// 差异 2.2%（instance）/ 0.7%（object3d）——那是**故意的语义变化**，gold 已按新语义重出；
// 留给今后的 0.5% 只是"实现变了但语义没变"的余量。
struct Expectation {
    const char* name;
    int pixel_threshold;        // 单像素最大通道差 > 此值算「显著变化」
    double max_changed_ratio;   // 显著变化像素占不透明像素的比例上限
};

const Expectation kExpectations[] = {
    {"vegetation_wind_instance_1280x720", /*pixel_threshold*/ 8, /*max_changed_ratio*/ 0.005},
    {"vegetation_wind_instance_blow_1280x720", 8, 0.005},
    {"vegetation_wind_object3d_1280x720", 8, 0.005},
};

// 关风对照组 vs 开风图：至少这么多比例的像素要变。
// 实测「关风 vs 1.7s」差异 **4.24%**（形变本体 + 随之移动的投影），取 1% 当下限：
// 只要风整段失效（GLSL 提前 return / 关节表没绑上 / joint_id 全 0），就一定低于它。
constexpr double kMinWindChangedRatio = 0.01;

}  // namespace

int main() {
    const std::string gold_dir = jpov::GetTestDataDir() + "/vegetation_wind/";
    const std::string out_dir = jpov::GetOutputDir() + "vegetation_wind_test/";
    std::system(("mkdir -p " + out_dir).c_str());

    // 现场烘一份树干（与 generator 同一份输入/参数 ⇒ 同一份网格）。
    const std::string glb = out_dir + "test_scene_plant.glb";
    std::string bake_error;
    if (!vegwind::BakeSamplePlant(vegwind::SampleAssetDir(), glb, &bake_error)) {
        LOG(ERROR) << "烘焙测试资产失败: " << bake_error;
        return 1;
    }

    JPOV::Config cfg;
    cfg.title = "JPOV Vegetation Wind Gold Test";
    cfg.headless = true;
    vegwind::WindSceneApp app(cfg);
    app.Init();
    app.ground_mesh_ = app.RegisterMesh(jpov::MeshData::MakeBox(
        /*front_half_width*/ 60.0f, /*up_half_width*/ 0.1f,
        /*left_half_width*/ 60.0f));
    if (!app.LoadPlant(glb)) {
        LOG(ERROR) << "载入烘焙产物失败: " << glb;
        app.Finalize();
        return 1;
    }

    jpov::WindowInfo winfo;
    winfo.width = 1280.0f;
    winfo.height = 720.0f;
    jpov::InputSnapshot input{};

    int failures = 0;
    const std::vector<vegwind::NamedShot> shots = vegwind::AllShots();
    size_t checked = 0;
    std::string instance_wind_png;

    for (const vegwind::NamedShot& named : shots) {
        const std::string path = out_dir + named.name + ".png";
        app.SetShot(named.shot);
        app.RunOnce(input, winfo, path.c_str());

        const Expectation* exp = nullptr;
        for (const Expectation& cand : kExpectations) {
            if (std::string(cand.name) == named.name) {
                exp = &cand;
                break;
            }
        }
        // 每张被渲染的 shot 都必须有验收口径 —— 少一条就等于"渲染了但没人看"。
        CHECK(exp != nullptr) << "shot 没有对应阈值: " << named.name;

        const std::string gold = gold_dir + named.name + ".png";
        const jpov::PixelChangeReport rep =
            jpov::ComparePixelChangePng(gold, path, exp->pixel_threshold);
        if (rep.width == 0 || rep.height == 0) {
            LOG(ERROR) << named.name << ": 与 gold 比对失败（图读不到或尺寸不一致）";
            ++failures;
            continue;
        }
        if (rep.opaque == 0) {
            LOG(ERROR) << named.name << ": 两图都没有不透明像素，渲染是空的";
            ++failures;
            continue;
        }
        LOG(INFO) << named.name << ": changed_ratio=" << (rep.changed_ratio * 100.0)
                  << "% (上限 " << (exp->max_changed_ratio * 100.0) << "%), max_diff="
                  << rep.max_diff << ", p99.9=" << rep.p999_diff;
        if (rep.changed_ratio > exp->max_changed_ratio) {
            LOG(ERROR) << named.name << ": 变化像素比例 " << (rep.changed_ratio * 100.0)
                       << "% 超过上限 " << (exp->max_changed_ratio * 100.0) << "%";
            ++failures;
        }
        ++checked;
        if (named.shot.instanced && named.shot.wind_on && named.shot.blow_ratio == 0.0f) {
            instance_wind_png = path;
        }
    }

    // 关风对照组：证明风**真的**在动（gold 一致但风失效是这套测试最危险的假绿）。
    {
        CHECK(!instance_wind_png.empty()) << "没有找到 instance 开风 shot";
        const std::string off_png = out_dir + "vegetation_wind_instance_off.png";
        app.SetShot(vegwind::Shot{.instanced = true, .wind_on = false, .blow_ratio = 0.0f});
        app.RunOnce(input, winfo, off_png.c_str());

        const jpov::PixelChangeReport rep =
            jpov::ComparePixelChangePng(instance_wind_png, off_png, /*threshold*/ 8);
        if (rep.width == 0) {
            LOG(ERROR) << "开风/关风比对失败（图读不到或尺寸不一致）";
            ++failures;
        } else {
            LOG(INFO) << "开风 vs 关风: changed_ratio=" << (rep.changed_ratio * 100.0)
                      << "%, max_diff=" << rep.max_diff;
            if (rep.changed_ratio < kMinWindChangedRatio) {
                LOG(ERROR) << "开风/关风差异只有 " << (rep.changed_ratio * 100.0)
                           << "%，低于 " << (kMinWindChangedRatio * 100.0)
                           << "% ⇒ 风可能整段失效（gold 一致也没有意义）";
                ++failures;
            }
        }
    }

    app.ReleasePlant();
    app.Finalize();

    if (failures != 0) {
        LOG(ERROR) << "植被风 gold test 失败 " << failures << " 项（比对 " << checked << " 张图）";
        return 1;
    }
    LOG(INFO) << "植被风 gold test 通过（比对 " << checked << " 张图 + 开风/关风对照）";
    return 0;
}
