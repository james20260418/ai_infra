// JPOV postprocess — 曝光（fixed EV）接口 gold test。
//
// 以 pliers 场景为基底，验证 exposure（固定 EV）接口：
//   - EV=0  （exposure=1.0）：基准，无曝光（物理锚点亮度）
//   - EV=+1 （exposure=2.0）：整体提亮一倍
//   - EV=-1 （exposure=0.5）：整体压暗一半
//
// 用像素统计断言（非逐像素，容忍 llvmpipe PBR 抖动）。物体 mask 来自
// EV=0 图的非黑像素（几何不随曝光变），复用到三档 → 排除背景，只统计物体。
//
// 核心断言（曝光是 tone map 前的线性缩放 → 确定性方向）：
//   1. 三档场景都渲染出物体（mask 非空）
//   2. 平均亮度：EV=+1 的 mean > EV=0 的 mean > EV=-1 的 mean
//   3. 相对亮度（可选辅助）：EV=0 物体 mean 应明显高于 EV=-1（压暗生效）
//
// test 基于**当前渲染**（非读旧 gold）：可捕获曝光接口的代码回归。

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/test/postprocess/final_adjust_common.h"

int main() {
    const std::string outdir =
        jpov::GetOutputDir() + "jpov_exposure_test/";
    std::system(("mkdir -p " + outdir).c_str());

    JPOV::Config cfg;
    cfg.title = "Exposure Test";
    cfg.headless = true;
    jpov::test::FinalAdjustApp app(cfg);
    app.Init();

    jpov::GltfObject gltf =
        app.LoadGltf(jpov::test::FinalAdjustApp::GetGltfPath());
    CHECK(!gltf.empty()) << "LoadGltf failed / empty";
    app.SetGltfObject(std::move(gltf));

    // ---- 渲染三档曝光 ----
    const std::string evm_path = outdir + "ev_-1.png";  // exposure=0.5
    jpov::test::RenderFinalAdjust(
        &app, evm_path, jpov::test::FinalAdjustParams{1.0f, 0.0f, 0.5f});

    const std::string ev0_path = outdir + "ev_0.png";   // exposure=1.0
    jpov::test::RenderFinalAdjust(
        &app, ev0_path, jpov::test::FinalAdjustParams{1.0f, 0.0f, 1.0f});

    const std::string evp_path = outdir + "ev_+1.png";  // exposure=2.0
    jpov::test::RenderFinalAdjust(
        &app, evp_path, jpov::test::FinalAdjustParams{1.0f, 0.0f, 2.0f});
    app.Finalize();

    // ---- 读取三档亮度 ----
    std::vector<float> evm_lum, ev0_lum, evp_lum;
    int w = 0, h = 0;
    CHECK(jpov::test::LoadLumPng(evm_path, &evm_lum, &w, &h));
    CHECK(jpov::test::LoadLumPng(ev0_path, &ev0_lum, &w, &h));
    CHECK(jpov::test::LoadLumPng(evp_path, &evp_lum, &w, &h));

    // ---- 物体 mask（来自 EV=0 图非黑，几何不随曝光变）----
    const std::vector<bool> obj_mask =
        jpov::test::BuildMaskFromPng(ev0_path, w, h);
    long obj_count = 0;
    for (bool b : obj_mask) if (b) ++obj_count;
    CHECK_GT(obj_count, 0) << "场景未渲染出物体（mask 为空？）";
    LOG(INFO) << "物体 mask 像素数=" << obj_count << "/" << (w * h);

    const jpov::test::LumStats evm =
        jpov::test::ComputeMaskedStats(evm_lum, obj_mask);
    const jpov::test::LumStats ev0 =
        jpov::test::ComputeMaskedStats(ev0_lum, obj_mask);
    const jpov::test::LumStats evp =
        jpov::test::ComputeMaskedStats(evp_lum, obj_mask);

    LOG(INFO) << "EV=-1(exposure=0.5)[物体]: mean=" << evm.mean
              << " std=" << evm.std << " min=" << evm.min_v
              << " max=" << evm.max_v;
    LOG(INFO) << "EV=0 (exposure=1.0)[物体]: mean=" << ev0.mean
              << " std=" << ev0.std << " min=" << ev0.min_v
              << " max=" << ev0.max_v;
    LOG(INFO) << "EV=+1(exposure=2.0)[物体]: mean=" << evp.mean
              << " std=" << evp.std << " min=" << evp.min_v
              << " max=" << evp.max_v;

    // ---- 断言 1：场景渲染出物体 ----
    LOG(INFO) << "PASS: 场景渲染出物体（mask 非空）";

    // ---- 断言 2（核心，对比度/亮度方向）：EV=+1 > EV=0 > EV=-1 的 mean ----
    CHECK_GT(evp.mean, ev0.mean)
        << "曝光增大应提亮（EV=+1 mean 应 > EV=0），实际 evp="
        << evp.mean << " ev0=" << ev0.mean;
    CHECK_GT(ev0.mean, evm.mean)
        << "曝光减小应压暗（EV=0 mean 应 > EV=-1），实际 ev0="
        << ev0.mean << " evm=" << evm.mean;
    LOG(INFO) << "PASS: 平均亮度 EV=+1(" << evp.mean << ") > EV=0("
              << ev0.mean << ") > EV=-1(" << evm.mean << ")";

    // ---- 断言 3（辅助，量化方向）：EV=0 比 EV=-1 显著亮（压暗真实生效）----
    // exposure 差 2 倍（0.5 vs 1.0），ACES 非线性压缩后仍应明显可分。
    const double ev0_boost = ev0.mean / (evm.mean > 0.0 ? evm.mean : 1.0);
    LOG(INFO) << "亮度比 EV=0/EV=-1 = " << ev0_boost;
    CHECK_GT(ev0_boost, 1.1)
        << "EV=0 应明显亮于 EV=-1（ratio 1.1 容差），实际 ratio="
        << ev0_boost;
    LOG(INFO) << "PASS: 曝光方向与量级正确";

    // ---- 断言 4（量级 / 反加性错误）：EV=0 物体 mean 应在合理中间调范围 ----
    // 曝光是**乘法**因子：pliers 中等光照下 EV=0(×1) 经 ACES 后物体 mean 应
    // 落在中间调（约 60~160）。若实现被错误写成加法（hdr+exposure），所有
    // 像素会被线性抬升到高亮甚至 clip（如 mean>200），本断言将 FAIL。
    // 这一条是方向断言（2/3）的补充：单纯方向单调无法区分乘/加错误实现。
    CHECK(ev0.mean > 60.0 && ev0.mean < 160.0)
        << "EV=0 物体 mean 应在中间调(60~160)以内（乘法曝光的合理范围），"
           "实际 ev0.mean=" << ev0.mean
        << " —— 疑似曝光被错误实现为加法（整体爆亮）";
    LOG(INFO) << "PASS: EV=0 物体 mean(" << ev0.mean
              << ") 在合理中间调范围（乘法曝光）";

    LOG(INFO) << "TEST PASSED: 曝光（fixed EV）接口生效，方向/量级正确";
    return 0;
}
