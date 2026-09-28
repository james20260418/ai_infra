// geom/common/grid_test.cc — Grid 坐标↔索引换算单测（GTest）
#include "geom/common/grid.h"

#include <gtest/gtest.h>

namespace {

	using geom::Grid1d;
	using geom::Grid2d;
	using geom::Grid3d;

	TEST(GridTest, IsGridIndiceInside) {
		EXPECT_TRUE(Grid1d::IsGridIndiceInside(Grid1d::Indice{ 0 }));
		EXPECT_TRUE(Grid2d::IsGridIndiceInside(Grid2d::Indice{ 0, 0 }));
		EXPECT_TRUE(Grid3d::IsGridIndiceInside(Grid3d::Indice{ 0, 0, 0 }));
	}

	// 坐标 → 格索引：向下取整（负数亦然），越界夹断到索引边界。
	TEST(GridTest, ValueToGridIndex) {
		EXPECT_EQ(Grid1d::ValueToGridIndex(3.5), 3);
		EXPECT_EQ(Grid1d::ValueToGridIndex(-3.5), -4);
		EXPECT_EQ(Grid1d::ValueToGridIndex(1e99), Grid1d::kMaxGridIndex);
		EXPECT_EQ(Grid1d::ValueToGridIndex(-1e99), Grid1d::kMinGridIndex);
	}

	TEST(GridTest, IsValueInside) {
		EXPECT_TRUE(Grid1d::IsValueInside(1e6));
		EXPECT_TRUE(Grid1d::IsValueInside(-1e6));
		EXPECT_FALSE(Grid1d::IsValueInside(1e66));
		EXPECT_FALSE(Grid1d::IsValueInside(-1e66));
	}

	TEST(GridTest, GridIndexToStartValue) {
		EXPECT_EQ(Grid1d::GridIndexToStartValue(-3), -3.0);
		EXPECT_EQ(Grid1d::GridIndexToStartValue(3), 3.0);
	}

}  // namespace
