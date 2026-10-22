// vegetation v2 查看器 gold test —— 保护「植物组合查看器」的出图。
//
// 两张基准图，两种阈值（为什么不能只用一张，见 vegetation_v2_gold_common.h 的注释）：
//
//   1) cards gold（debug 绿卡视图）：**不透明纯色盒子**，llvmpipe 下逐字节确定
//      （实测连续 6 次 diff = 0）⇒ 阈值 0.5 的**精确**几何锁。
//      锁住 anchor / up / normal / width / height + anchor_mode → UV 的推导，
//      也就是「叶卡到底贴在枝的哪儿」。
//   2) leaf gold（贴叶图 + 树皮）：alpha cutout + 覆盖率 mip 在 llvmpipe 下会落在
//      几种离散状态（实测 8×8 ROI 均值差 0 ~ 27 跳）⇒ 只能宽阈值当烟雾报警。
//      叶子整片消失 64、树皮掉了 46 会被抓住。
//
// gold 目录：tools/jpov/test/vegetation_v2/
// 重新生成：bazel run //tools/jpov/test/vegetation_v2:jpov_vegetation_v2_gold_generator

#include <cstdio>
#include <string>

#include <glog/logging.h>

#include "third_party/stb/stb_image.h"

#include "tools/jpov/test/compare_light.h"
#include "tools/jpov/test/test_utils.h"
#include "tools/jpov/test/vegetation_v2/vegetation_v2_gold_common.h"

namespace {

// 几何锁：实测噪声 0（连续 6 次），给 0.5 留一点跨机器余量。
constexpr double kCardsMaxRoiDiff = 0.5;
// 材质烟雾报警：实测噪声上界 ~27（叶子 cutout 的 llvmpipe 状态跳变），
// 已知回归下限 ~46（没贴树皮）。给 35。
constexpr double kLeafMaxRoiDiff = 35.0;
constexpr int kRoiRows = 8;
constexpr int kRoiCols = 8;

std::string GoldDir() { return jpov::GetTestDataDir() + "/vegetation_v2/"; }

// 渲染 + 比对一张，返回是否通过（先把 gold 存在性、分辨率、非全黑都查掉）。
bool CheckOne(const char* gold_name, bool with_leaf, double max_diff_allowed,
              const std::string& outdir) {
  const std::string gold = GoldDir() + gold_name;
  {
    FILE* f = std::fopen(gold.c_str(), "rb");
    CHECK(f != nullptr) << "gold image 缺失，请先跑 jpov_vegetation_v2_gold_generator: "
                        << gold;
    std::fclose(f);
  }
  const std::string rendered = outdir + gold_name;
  CHECK(vegv2::goldtest::RenderGolden(with_leaf, rendered)) << "gold 场景渲染失败";

  // 健全性：分辨率对、画面不是全黑（防「渲染全黑但均值差为 0」的假绿）。
  int w = 0;
  int h = 0;
  int ch = 0;
  unsigned char* px = stbi_load(rendered.c_str(), &w, &h, &ch, 4);
  CHECK(px != nullptr) << "读不到渲染结果: " << rendered;
  CHECK_EQ(w, vegv2::goldtest::kWidth);
  CHECK_EQ(h, vegv2::goldtest::kHeight);
  long long nonblack = 0;
  for (int i = 0; i < w * h; ++i) {
    if (px[i * 4 + 0] + px[i * 4 + 1] + px[i * 4 + 2] > 24) {
      ++nonblack;
    }
  }
  const double ratio = static_cast<double>(nonblack) / (w * h);
  stbi_image_free(px);
  CHECK_GT(ratio, 0.10) << gold_name << ": 画面几乎全黑，渲染链路可能整体挂了";

  const double max_diff =
      jpov::CompareLightMeanRoiPng(gold, rendered, kRoiRows, kRoiCols);
  CHECK_GE(max_diff, 0.0) << gold_name << ": 两张图尺寸不一致";
  LOG(INFO) << gold_name << ": 8x8 ROI 最大通道均值差 = " << max_diff << "（阈值 "
            << max_diff_allowed << "），非黑占比 " << ratio * 100.0 << "%";
  if (max_diff > max_diff_allowed) {
    LOG(ERROR) << "GOLD COMPARE FAILED: " << gold_name << " diff=" << max_diff
               << " > " << max_diff_allowed << "\n  渲染结果: " << rendered
               << "\n  gold:     " << gold
               << "\n  若观感变化是有意的，重跑 generator 并提交新 gold。";
    return false;
  }
  return true;
}

}  // namespace

int main() {
  const std::string outdir = jpov::GetOutputDir() + "vegetation_v2_gold/";
  std::system(("mkdir -p " + outdir).c_str());

  const bool cards_ok = CheckOne(vegv2::goldtest::kCardsGold, /*with_leaf=*/false,
                                 kCardsMaxRoiDiff, outdir);
  const bool leaf_ok = CheckOne(vegv2::goldtest::kLeafGold, /*with_leaf=*/true,
                                kLeafMaxRoiDiff, outdir);
  if (!cards_ok || !leaf_ok) {
    return 1;
  }
  LOG(INFO) << "GOLD COMPARE PASSED（cards + leaf）";
  return 0;
}
