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
constexpr int kCardsRoi = 8;

// 材质图不用 ROI 做主判据 —— 2026-10-22 实测：叶子 cutout 在 llvmpipe 下会**整块**
// 落到另一种离散状态，同一份源码、同一个二进制，两次渲染 4×4 ROI 最大差就能到 124
// （≈0.08% 的像素翻面），阈值 15 的 ROI 判据会随机变红；而真回归大得多（去掉叶图
// 21.9% 像素变化）。所以这里改用**显著变化像素占比**：
//   * 状态翻转：0.085%
//   * 叶子整片没了：21.9%
// 阈值 2% 卡在中间，既不会因离散状态抖动，又能抓住真回归。ROI 值仍然打印（诊断用）。
constexpr double kLeafMaxChangedRatio = 0.02;

// glb 展示态：风时间已钉死、烘焙产物逐字节可复现（实测两次 bake md5 相同、三次渲染
// md5 相同）。但烘焙 + cutout 仍可能撞上那种整块离散状态，所以双判据：
//   ROI ≤ 2.0（8×8）**且** 显著变化像素 ≤ 1%。
// 能抓住：关节表算错（枝条乱扭/炸开）、风没生效（树纹丝不动）、贴图没内嵌（全白/全黑）、
// 顶点挂错枝（整根枝飞走）。
constexpr double kGlbMaxRoiDiff = 2.0;
constexpr int kGlbRoi = 8;
constexpr double kGlbMaxChangedRatio = 0.01;

// 逐像素「显著变化」的判据（单像素 RGB 三通道差的最大值）—— 与 kLeaf/kGlb 的
// changed_ratio 配套使用。
constexpr int kPixelThreshold = 8;

std::string GoldDir() { return jpov::GetTestDataDir() + "/vegetation_v2/"; }

// 渲染 + 比对一张，返回是否通过（先把 gold 存在性、分辨率、非全黑都查掉）。
// mode：0 = debug 绿卡、1 = 贴叶图 + 树皮、2 = glb 展示态（烘焙→载入→带风）。
// 判据（两项都要过）：
//   roi        —— ROI 均值最大通道差上限（大范围亮度/颜色漂移）
//   ratio      —— 显著变化像素占比上限（局部结构变化）；< 0 = 不看这一项
bool CheckOne(const char* gold_name, int mode, double max_diff_allowed, int roi,
              double max_changed_ratio, const std::string& outdir) {
  const std::string gold = GoldDir() + gold_name;
  {
    FILE* f = std::fopen(gold.c_str(), "rb");
    CHECK(f != nullptr) << "gold image 缺失，请先跑 jpov_vegetation_v2_gold_generator: "
                        << gold;
    std::fclose(f);
  }
  const std::string rendered = outdir + gold_name;
  const bool rendered_ok = (mode == 2)
                               ? vegv2::goldtest::RenderGlbGolden(rendered)
                               : vegv2::goldtest::RenderGolden(/*with_leaf=*/mode == 1, rendered);
  CHECK(rendered_ok) << "gold 场景渲染失败";

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

  const double max_diff = jpov::CompareLightMeanRoiPng(gold, rendered, roi, roi);
  CHECK_GE(max_diff, 0.0) << gold_name << ": 两张图尺寸不一致";
  LOG(INFO) << gold_name << ": " << roi << "x" << roi << " ROI 最大通道均值差 = "
            << max_diff << "（阈值 " << max_diff_allowed << "），非黑占比 "
            << ratio * 100.0 << "%";
  bool ok = (max_diff <= max_diff_allowed);

  double changed = -1.0;
  if (max_changed_ratio >= 0.0) {
    const jpov::PixelChangeReport rep =
        jpov::ComparePixelChangePng(gold, rendered, kPixelThreshold);
    changed = rep.changed_ratio;
    LOG(INFO) << gold_name << ": 显著变化像素（|Δ|>" << kPixelThreshold
              << "）= " << rep.changed << " / " << rep.opaque << " = "
              << changed * 100.0 << "%（阈值 " << max_changed_ratio * 100.0 << "%）";
    if (changed > max_changed_ratio) {
      ok = false;
    }
  }

  if (!ok) {
    LOG(ERROR) << "GOLD COMPARE FAILED: " << gold_name << " roi_diff=" << max_diff
               << "（阈值 " << max_diff_allowed << "）changed=" << changed
               << "（阈值 " << max_changed_ratio
               << "）\n  渲染结果: " << rendered
               << "\n  gold:     " << gold
               << "\n  若观感变化是有意的，重跑 generator 并提交新 gold。";
  }
  return ok;
}

}  // namespace

int main() {
  const std::string outdir = jpov::GetOutputDir() + "vegetation_v2_gold/";
  std::system(("mkdir -p " + outdir).c_str());

  // cards 只信 ROI（几何锁，噪声 0）；leaf 以显著像素占比为主（cutout 会整块翻面）；
  // glb 两者都看（烘焙确定，但 cutout 状态仍可能翻）。
  const bool cards_ok = CheckOne(vegv2::goldtest::kCardsGold, /*mode=*/0,
                                 kCardsMaxRoiDiff, kCardsRoi, /*max_changed_ratio=*/-1.0,
                                 outdir);
  const bool leaf_ok = CheckOne(vegv2::goldtest::kLeafGold, /*mode=*/1,
                                /*max_diff_allowed=*/1e9, /*roi=*/4,
                                kLeafMaxChangedRatio, outdir);
  const bool glb_ok = CheckOne(vegv2::goldtest::kGlbGold, /*mode=*/2,
                               kGlbMaxRoiDiff, kGlbRoi, kGlbMaxChangedRatio, outdir);
  if (!cards_ok || !leaf_ok || !glb_ok) {
    return 1;
  }
  LOG(INFO) << "GOLD COMPARE PASSED（cards + leaf + glb）";
  return 0;
}
