// MeasureOverlayExtent 单测（**纯 CPU，零 GL**）。
//
// 为什么单独测这个"数行宽"的小函数：Draw() 的布局是「量内容 → 定黑底尺寸 →
// 生成顶点」，如果测量逻辑写错（比如把"空内容就不画"的判断插进了遍历分支里），
// 绝大多数输入仍然出图，只有少数形状（首行为空、后面有字）整块不画 —— GPU 测试
// 用的 DebugString() 永远是"首行非空"，抓不到。这个纯函数把它逼出来。

#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "tools/jpov/src/debug_overlay/debug_overlay_renderer.h"

namespace {

using jpov::MeasureOverlayExtent;

TEST(OverlayExtent, EmptyInputsCarryNoDrawableChar) {
    EXPECT_TRUE(MeasureOverlayExtent("").IsEmpty());
    EXPECT_TRUE(MeasureOverlayExtent("\n").IsEmpty());
    EXPECT_TRUE(MeasureOverlayExtent("\n\n\n").IsEmpty());
    // 空串也算 1 行（版面语义：至少一行，只是宽度 0）。
    EXPECT_EQ(MeasureOverlayExtent("").line_count, 1);
    EXPECT_EQ(MeasureOverlayExtent("\n\n\n").line_count, 4);
}

// 🔴 这条就是当年（本轮）真踩到的形状：首行为空 + 后面有字，必须**照画**。
TEST(OverlayExtent, LeadingBlankLineStillHasContent) {
    const jpov::OverlayExtent e = MeasureOverlayExtent("\nabc");
    EXPECT_FALSE(e.IsEmpty()) << "首行为空但第二行有字，不该被判成空内容";
    EXPECT_EQ(e.max_cols, 3);
    EXPECT_EQ(e.line_count, 2);
}

TEST(OverlayExtent, MaxColsIsTheWidestLine) {
    const jpov::OverlayExtent e = MeasureOverlayExtent("a\nbbbb\ncc");
    EXPECT_EQ(e.max_cols, 4);
    EXPECT_EQ(e.line_count, 3);
}

TEST(OverlayExtent, TrailingNewlineAddsAnEmptyLine) {
    const jpov::OverlayExtent e = MeasureOverlayExtent("abc\n");
    EXPECT_EQ(e.max_cols, 3);
    EXPECT_EQ(e.line_count, 2);
}

// 非 ASCII（UTF-8 多字节）按**字节**数格子 —— 与 Draw() 的前进量一致（每个字节一格）。
TEST(OverlayExtent, NonAsciiCountsPerByte) {
    const std::string utf8 = "\xe4\xb8\xad";   // "中"，3 字节
    const jpov::OverlayExtent e = MeasureOverlayExtent(utf8);
    EXPECT_EQ(e.max_cols, 3);
    EXPECT_FALSE(e.IsEmpty());
}

TEST(OverlayExtent, SpacesCountAsCells) {
    // 5 空格 + "vendor"(6) + 1 空格 + "Mesa"(4) = 16
    const jpov::OverlayExtent e = MeasureOverlayExtent("     vendor Mesa");
    EXPECT_EQ(e.max_cols, 16);
}

}  // namespace
