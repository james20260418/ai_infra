// JPOV postprocess — 色彩分级（ASC-CDL）gold image generator。
//
// 以 pliers（glTF PBR）场景为基底，渲染 3 种影视风格的分级效果图：
//   1. grade_autumn.png         — 萧瑟秋日（暖橙褪色、压暗、胶片感）
//   2. grade_bright_spring.png  — 光亮春天（明亮通透、中间调提亮、清新）
//   3. grade_teal_orange.png    — 青橙电影感（暗部青蓝/亮部橙，blockbuster）
// 供 leader 肉眼验收风格效果。场景构建复用 final_adjust_common.h。

#include <cstdio>
#include <string>

#include <glog/logging.h>

#include "tools/common/utils.h"
#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/test/postprocess/final_adjust_common.h"
#include "tools/jpov/test/test_utils.h"

namespace {

// 用 ASC-CDL 参数构造 ColorGrade。
jpov::ColorGrade MakeGrade(const jpov::Vec3f& slope,
                           const jpov::Vec3f& offset,
                           const jpov::Vec3f& power) {
    jpov::ColorGrade g;
    g.slope = slope;
    g.offset = offset;
    g.power = power;
    g.enabled = true;
    return g;
}

}  // namespace

int main() {
    const std::string base = jpov::GetTestDataDir() + "/postprocess/";

    JPOV::Config cfg;
    cfg.title = "Color Grade Gold Generator";
    cfg.headless = true;
    jpov::test::FinalAdjustApp app(cfg);
    app.Init();

    jpov::GltfObject gltf =
        app.LoadGltf(jpov::test::FinalAdjustApp::GetGltfPath());
    CHECK(!gltf.empty()) << "LoadGltf failed / empty";
    app.SetGltfObject(std::move(gltf));

    // ---- 基准（无分级，对照用）----
    app.SetGrade(jpov::ColorGrade{});  // enabled=false 恒等
    jpov::test::RenderFinalAdjust(
        &app, base + "grade_neutral.png",
        jpov::test::FinalAdjustParams{1.0f, 0.0f, 1.0f});

    // ---- 风格 1：萧瑟秋日（暖橙褪色、胶片感、暗部偏暖）----
    // 相对 pliers 天然暖基色进一步增强暖感：R 升 / B 降 → R-B 显著高于基准；
    // offset 微抬(褪色雾化)；power 略压暗部厚重。
    app.SetGrade(MakeGrade(
        {1.18f, 1.04f, 0.78f},   // slope: R↑ / G微↑ / B↓ → 明显暖化
        {0.05f, 0.03f, 0.00f},   // offset: 微抬暗部 → 胶片褪色感
        {0.95f, 1.00f, 1.10f})); // power: R<1提亮暖 / B>1压暗蓝 → 暖而沉稳
    jpov::test::RenderFinalAdjust(
        &app, base + "grade_autumn.png",
        jpov::test::FinalAdjustParams{1.0f, 0.0f, 1.0f});

    // ---- 风格 2：光亮春天（明亮通透、中间调提亮、清新微冷）----
    // 相对 pliers 天然暖基色往『冷+亮』走：B↑(清新) / R 压回平衡；power<<1 提亮；
    // 目标是整体比基准更亮、更冷（R-B 明显低于基准），体现春日清爽。
    app.SetGrade(MakeGrade(
        {1.02f, 1.06f, 1.18f},   // slope: R微↑ / G↑ / B↑↑ → 冷调通透
        {0.04f, 0.05f, 0.08f},   // offset: 抬暗部 → 明亮无死黑
        {0.80f, 0.84f, 0.90f})); // power<<1 → 中间调大幅提亮，画面明亮
    jpov::test::RenderFinalAdjust(
        &app, base + "grade_bright_spring.png",
        jpov::test::FinalAdjustParams{1.0f, 0.0f, 1.0f});

    // ---- 风格 3：青橙电影感（暗部青蓝 / 亮部橙，高反差 blockbuster）----
    // split-tone：offset(lift) B↑→暗部青蓝；slope(gain) R↑G↓→亮部橙；power>1 高反差。
    app.SetGrade(MakeGrade(
        {1.22f, 0.98f, 0.80f},   // slope: R↑ / G平 / B↓ → 亮部偏橙
        {0.00f, 0.03f, 0.12f},   // offset: B突出 → 暗部青蓝
        {1.20f, 1.12f, 0.92f})); // power>1 → 中间调压暗高反差；B 略低配合
    jpov::test::RenderFinalAdjust(
        &app, base + "grade_teal_orange.png",
        jpov::test::FinalAdjustParams{1.0f, 0.0f, 1.0f});

    app.Finalize();

    LOG(INFO) << "color grade gold images generated (autumn / bright_spring / teal_orange)";
    return 0;
}
