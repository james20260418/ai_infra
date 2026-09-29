// JPOV 穿衣工具 — 坐标填值输入解析单测
//
// 覆盖 ParseAxisValue 的合法/非法边界。这是"回车/焦点丧失提交"语义的唯一行为要点。

#include "tools/jpov/clothing/clothing_axis_input.h"

#include <gtest/gtest.h>

namespace jpov {
namespace clothing {
namespace {

TEST(ParseAxisValueTest, AcceptsPlainNumbers) {
    float v = 0.0f;
    EXPECT_TRUE(ParseAxisValue("0", &v));
    EXPECT_FLOAT_EQ(v, 0.0f);

    EXPECT_TRUE(ParseAxisValue("1.5", &v));
    EXPECT_FLOAT_EQ(v, 1.5f);

    EXPECT_TRUE(ParseAxisValue("-3", &v));
    EXPECT_FLOAT_EQ(v, -3.0f);

    EXPECT_TRUE(ParseAxisValue("+2.25", &v));
    EXPECT_FLOAT_EQ(v, 2.25f);
}

TEST(ParseAxisValueTest, AcceptsScientificAndLeadingDot) {
    float v = 0.0f;
    EXPECT_TRUE(ParseAxisValue("1e3", &v));
    EXPECT_FLOAT_EQ(v, 1000.0f);

    EXPECT_TRUE(ParseAxisValue(".5", &v));
    EXPECT_FLOAT_EQ(v, 0.5f);
}

TEST(ParseAxisValueTest, AcceptsSurroundingWhitespace) {
    float v = 0.0f;
    EXPECT_TRUE(ParseAxisValue("  -1.25\t", &v));
    EXPECT_FLOAT_EQ(v, -1.25f);
}

TEST(ParseAxisValueTest, RejectsEmptyAndWhitespaceOnly) {
    float v = 7.0f;
    EXPECT_FALSE(ParseAxisValue("", &v));
    EXPECT_FALSE(ParseAxisValue("   ", &v));
    EXPECT_FALSE(ParseAxisValue("\t\t", &v));
    // 非法时不改写 out。
    EXPECT_FLOAT_EQ(v, 7.0f);
}

TEST(ParseAxisValueTest, RejectsNonNumeric) {
    float v = 7.0f;
    EXPECT_FALSE(ParseAxisValue("abc", &v));
    EXPECT_FALSE(ParseAxisValue("x", &v));
    EXPECT_FLOAT_EQ(v, 7.0f);
}

TEST(ParseAxisValueTest, RejectsTrailingGarbage) {
    float v = 7.0f;
    EXPECT_FALSE(ParseAxisValue("12abc", &v));
    EXPECT_FALSE(ParseAxisValue("1.5 2.5", &v));
    EXPECT_FALSE(ParseAxisValue("1,5", &v));
    EXPECT_FLOAT_EQ(v, 7.0f);
}

TEST(ParseAxisValueTest, RejectsNonFinite) {
    float v = 7.0f;
    EXPECT_FALSE(ParseAxisValue("inf", &v));
    EXPECT_FALSE(ParseAxisValue("-inf", &v));
    EXPECT_FALSE(ParseAxisValue("nan", &v));
    EXPECT_FLOAT_EQ(v, 7.0f);
}

TEST(ParseAxisValueTest, RejectsNull) {
    float v = 0.0f;
    EXPECT_FALSE(ParseAxisValue(nullptr, &v));
    EXPECT_FALSE(ParseAxisValue("1.0", nullptr));
}

// 精度：解析出的值与字面量在 float 精度内一致（不是 0/垃圾）。
TEST(ParseAxisValueTest, ParsesNegativeFraction) {
    float v = 0.0f;
    ASSERT_TRUE(ParseAxisValue("-0.35", &v));
    EXPECT_NEAR(v, -0.35f, 1e-6f);
}

}  // namespace
}  // namespace clothing
}  // namespace jpov
