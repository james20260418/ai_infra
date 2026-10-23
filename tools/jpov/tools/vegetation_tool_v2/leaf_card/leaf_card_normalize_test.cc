// leaf_card_normalize 的单测：合成图（已知根点在哪儿）→ 检查检测/旋转/裁剪是否对。
//
// 合成叶子：一个椭圆叶片 + 一条细叶柄。叶柄那端在主轴上是「细」的一端，
// 所以根部检测有唯一正确答案，断言可以写死。

#include "tools/jpov/tools/vegetation_tool_v2/leaf_card/leaf_card_normalize.h"

#include <cmath>
#include <string>

#include "gtest/gtest.h"
#include "tools/jpov/tools/vegetation_tool_v2/leaf_card/leaf_card_spec.h"

namespace jpov {
namespace vegetation {
namespace {

// 画一张「黑底 + 椭圆叶片 + 细叶柄」的图。叶柄从 (stem_x, stem_y0) 竖直向下一段。
LeafCardImage MakeSyntheticLeaf(int width, int height, double blade_cx, double blade_cy,
                                double blade_rx, double blade_ry, double stem_x,
                                double stem_y0, double stem_y1) {
  LeafCardImage image;
  image.width = width;
  image.height = height;
  image.rgba.assign(static_cast<std::size_t>(width) * height * 4, 0);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const double dx = (x - blade_cx) / blade_rx;
      const double dy = (y - blade_cy) / blade_ry;
      const bool in_blade = dx * dx + dy * dy <= 1.0;
      const bool in_stem = std::abs(x - stem_x) <= 2.5 && y >= stem_y0 && y <= stem_y1;
      if (!in_blade && !in_stem) {
        continue;  // 背景保持纯黑
      }
      const std::size_t index = (static_cast<std::size_t>(y) * width + x) * 4;
      // 内容亮度必须 >= kLeafContentMinChannel（生图合同）。
      image.rgba[index] = 90;
      image.rgba[index + 1] = 140;
      image.rgba[index + 2] = 60;
      image.rgba[index + 3] = 255;
    }
  }
  return image;
}

double AngleDiff(double a, double b) {
  double diff = std::fmod(a - b + 540.0, 360.0) - 180.0;
  return std::fabs(diff);
}

// 叶柄竖直朝上（图像坐标 -y），目标 bottom_center 也是朝上 ⇒ 不该旋转。
TEST(LeafCardNormalizeTest, StemAlreadyUpForBottomCenter) {
  const LeafCardImage input = MakeSyntheticLeaf(/*width=*/200, /*height=*/240,
                                                /*blade_cx=*/100, /*blade_cy=*/80,
                                                /*blade_rx=*/50, /*blade_ry=*/60,
                                                /*stem_x=*/100, /*stem_y0=*/140,
                                                /*stem_y1=*/220);
  LeafCardNormalizeOptions options;
  options.anchor_mode = "bottom_center";
  LeafCardImage output;
  LeafCardNormalizeReport report;
  std::string error;
  ASSERT_TRUE(NormalizeLeafCard(input, options, &output, &report, &error)) << error;

  EXPECT_LT(AngleDiff(report.rotation_deg, 0.0), 5.0) << "rotation=" << report.rotation_deg;
  EXPECT_LT(std::fabs(report.root_offset_u), 0.10) << "根点应该落在底部中间";
  EXPECT_GT(report.height, 0);
  EXPECT_GT(report.aspect, 0.0);
}

// 同一张图换 bottom_left：叶柄要转到右上（+45°），根点落在左下角。
TEST(LeafCardNormalizeTest, RotatesStemToCornerForBottomLeft) {
  const LeafCardImage input = MakeSyntheticLeaf(/*width=*/200, /*height=*/240,
                                                /*blade_cx=*/100, /*blade_cy=*/80,
                                                /*blade_rx=*/50, /*blade_ry=*/60,
                                                /*stem_x=*/100, /*stem_y0=*/140,
                                                /*stem_y1=*/220);
  LeafCardNormalizeOptions options;
  options.anchor_mode = "bottom_left";
  LeafCardImage output;
  LeafCardNormalizeReport report;
  std::string error;
  ASSERT_TRUE(NormalizeLeafCard(input, options, &output, &report, &error)) << error;

  EXPECT_LT(AngleDiff(report.rotation_deg, 45.0), 5.0) << "rotation=" << report.rotation_deg;
  // 根部要落在左下角（容差给 12%：叶柄有粗细、裁剪按 alpha 像素走）。
  EXPECT_LT(std::fabs(report.root_offset_u), 0.12) << "du=" << report.root_offset_u;
  EXPECT_LT(std::fabs(report.root_offset_v), 0.12) << "dv=" << report.root_offset_v;

  // 叶柄转到右上后：根点（叶柄尖）应当落在左下角那一小块里；
  // 左上角那一块应当没有内容（叶子整体朝右上铺开）。
  // 注意不能断言「右上角有内容」—— 叶片是椭圆的，AABB 的四角本来就空。
  const int w = output.width;
  const int h = output.height;
  int near_root = 0;
  for (int y = h - 12; y < h; ++y) {
    for (int x = 0; x < 12; ++x) {
      near_root += output.rgba[(static_cast<std::size_t>(y) * w + x) * 4 + 3] > 0 ? 1 : 0;
    }
  }
  EXPECT_GT(near_root, 0) << "根点（叶柄尖）应当落在左下角";
  int near_top_left = 0;
  for (int y = 0; y < 12; ++y) {
    for (int x = 0; x < 12; ++x) {
      near_top_left += output.rgba[(static_cast<std::size_t>(y) * w + x) * 4 + 3] > 0 ? 1 : 0;
    }
  }
  EXPECT_EQ(near_top_left, 0) << "左上角应当没有内容";
}

// bottom_right 与 bottom_left 的旋转方向应当相反。
TEST(LeafCardNormalizeTest, MirrorsRotationForBottomRight) {
  const LeafCardImage input = MakeSyntheticLeaf(200, 240, 100, 80, 50, 60, 100, 140, 220);
  LeafCardNormalizeOptions left_options;
  left_options.anchor_mode = "bottom_left";
  LeafCardNormalizeOptions right_options;
  right_options.anchor_mode = "bottom_right";
  LeafCardImage out_left;
  LeafCardImage out_right;
  LeafCardNormalizeReport left_report;
  LeafCardNormalizeReport right_report;
  std::string error;
  ASSERT_TRUE(NormalizeLeafCard(input, left_options, &out_left, &left_report, &error)) << error;
  ASSERT_TRUE(NormalizeLeafCard(input, right_options, &out_right, &right_report, &error)) << error;
  EXPECT_LT(AngleDiff(left_report.rotation_deg, 45.0), 5.0);
  EXPECT_LT(AngleDiff(right_report.rotation_deg, -45.0), 5.0);
}

// 负向：全黑图必须报错（并且提示去看生图合同），不能返回一张空图。
TEST(LeafCardNormalizeTest, RejectsEmptyImage) {
  LeafCardImage input;
  input.width = 64;
  input.height = 64;
  input.rgba.assign(static_cast<std::size_t>(64) * 64 * 4, 0);
  LeafCardNormalizeOptions options;
  LeafCardImage output;
  LeafCardNormalizeReport report;
  std::string error;
  EXPECT_FALSE(NormalizeLeafCard(input, options, &output, &report, &error));
  EXPECT_NE(error.find("没有识别到叶子内容"), std::string::npos) << error;
}

// 负向：anchor_mode 拼错必须报错。
TEST(LeafCardNormalizeTest, RejectsUnknownAnchorMode) {
  const LeafCardImage input = MakeSyntheticLeaf(200, 240, 100, 80, 50, 60, 100, 140, 220);
  LeafCardNormalizeOptions options;
  options.anchor_mode = "center";
  LeafCardImage output;
  LeafCardNormalizeReport report;
  std::string error;
  EXPECT_FALSE(NormalizeLeafCard(input, options, &output, &report, &error));
  EXPECT_NE(error.find("anchor_mode"), std::string::npos) << error;
}

// 生图合同体检：背景灰度落在 [背景上限, 内容下限] 之间时要报出比例。
TEST(LeafCardNormalizeTest, ReportsAmbiguousPixels) {
  LeafCardImage input = MakeSyntheticLeaf(200, 240, 100, 80, 50, 60, 100, 140, 220);
  // 把 200 个背景像素抬到灰区（32），模拟「背景不是纯黑」。
  for (int i = 0; i < 200; ++i) {
    input.rgba[static_cast<std::size_t>(i) * 4] = 32;
    input.rgba[static_cast<std::size_t>(i) * 4 + 1] = 32;
    input.rgba[static_cast<std::size_t>(i) * 4 + 2] = 32;
  }
  LeafCardNormalizeOptions options;
  LeafCardImage output;
  LeafCardNormalizeReport report;
  std::string error;
  ASSERT_TRUE(NormalizeLeafCard(input, options, &output, &report, &error)) << error;
  EXPECT_GT(report.ambiguous_fraction, 0.0);
}

}  // namespace
}  // namespace vegetation
}  // namespace jpov
