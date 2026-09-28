// geom/common/grid.h — N 维等距栅格（等距网格的坐标↔索引换算）
//
// 用「正整数格的等距栅格」把连续坐标 (ValueType) 与整数格索引 (IntType) 互相换算：
//   - 格宽恒为 1.0，格 k 覆盖 [k, k+1)，原点与 0.0 对齐；
//   - 坐标 → 所属格索引（向下取整；越界坐标先夹断到网格范围内）；
//   - 格索引 → 该格左/下边界坐标（越界索引先夹断到网格范围内）。
//
// 本文件只负责**换算规则**，不含任何存储：稀疏数据的存放见 grid_map.h
// （以格索引为键的哈希表），稠密数据可由调用方自行按索引展平存数组。
//
// 设计取舍：
//   - Dim / IntType / ValueType 都是模板参数，由使用方按场景取（如 Grid4d）。
//   - IntType 必须是有符号整型（索引可为负，见文件尾部示意图）；ValueType 必须是
//     浮点型，且其取值范围必须能覆盖整型索引范围（static_assert 保证，避免夹断失真）。
//   - 索引留出 ±2 的余量（kMaxGridIndex/kMinGridIndex），使「格索引 + 1」等边界运算
//     不会溢出，同时 ValueType 的夹断端点有定义。
//
// Pre-condition（模板约束，见 struct 内 static_assert）：
//   1. Dim > 0；
//   2. ValueType 为浮点类型；
//   3. IntType 为有符号整型。

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <type_traits>

#include "geom/common/check.h"

namespace geom {

	// 一个 N 维等距栅格（2D 为例，kMaxValue = 3.0、kMinValue = -3.0、
	// kMaxGridIndex = 2、kMinGridIndex = -3）：
	//
	//                      ^ y
	//    __________________|_________________o (x=3, y=3)
	//    |     |     |     |     |     |     |
	//    |-3,2 |-2,2 |-1,2 | 0,2 | 1,2 | 2,2 |
	//    |_____|_____|_____|_____|_____|_____|
	//    |     |     |     |     |     |     |
	//    |-3,1 |-2,1 |-1,1 | 0,1 | 1,1 | 2,1 |
	//    |_____|_____|_____|_____|_____|_____|
	//    |     |     |     |     |     |     |
	//    |-3,0 |-2,0 |-1,0 | 0,0 | 1,0 | 2,0 |
	//  __|_____|_____|_____|_____|_____|_____|__>  x
	//    |     |     |     |     |     |     |
	//    |-3,-1|-2,-1|-1,-1| 0,-1| 1,-1| 2,-1|
	//    |_____|_____|_____|_____|_____|_____|
	//    |     |     |     |     |     |     |
	//    |-3,-2|-2,-2|-1,-2| 0,-2| 1,-2| 2,-2|
	//    |_____|_____|_____|_____|_____|_____|
	//    |     |     |     |     |     |     |
	//    |-3,-3|-2,-3|-1,-3| 0,-3| 1,-3| 2,-3|
	//    |_____|_____|_____|_____|_____|_____o (x=3, y=-3)
	//                      |
	// 此处把原点设在 0.0、格宽设为 1.0。
	template <int Dim, typename IntType = int, typename ValueType = double>
	struct Grid {
		// 校验模板类型合法（见文件头 Pre-condition）。
		static_assert(Dim > 0 && std::is_integral<IntType>::value &&
			std::is_signed<IntType>::value && std::is_floating_point<ValueType>::value,
			"Invalid template type.");

		// 对外暴露模板参数。
		static constexpr int kDim = Dim;
		using IntT = IntType;
		using ValueT = ValueType;

		// 格索引的取值范围（两端各留 2 的余量防溢出）。
		static constexpr IntType kMaxGridIndex = std::numeric_limits<IntType>::max() - 2;
		static constexpr IntType kMinGridIndex = std::numeric_limits<IntType>::min() + 2;

		// 浮点类型必须能覆盖整型索引范围（否则夹断会失真）。
		static_assert(std::numeric_limits<ValueType>::max() > (kMaxGridIndex + 1) &&
			std::numeric_limits<ValueType>::lowest() < kMinGridIndex,
			"Floating type range too small.");

		// 格坐标的取值范围。
		static constexpr ValueType kMaxValue = static_cast<ValueType>(kMaxGridIndex + 1);
		static constexpr ValueType kMinValue = static_cast<ValueType>(kMinGridIndex);

		// 格索引（每维一个）。
		using Indice = std::array<IntType, Dim>;

		// 给定坐标，判断是否落在栅格坐标范围内。（1D 版本）
		static bool IsValueInside(ValueType value);

		// 给定格索引，判断是否落在栅格索引范围内。（1D 版本）
		static bool IsGridIndexInside(IntType grid_index);

		// 给定格索引，判断是否落在栅格索引范围内。
		static bool IsGridIndiceInside(const Indice& indice);

		// 给定坐标，求它所在格的索引。（越界坐标用夹断值）
		// （1D 版本）
		static IntType ValueToGridIndex(ValueType value);

		// 给定格索引，求该格左/下边界的坐标。（越界索引用夹断值）
		// （1D 版本）
		static ValueType GridIndexToStartValue(IntType grid_index);
	};

	using Grid1d = Grid<1, int, double>;
	using Grid2d = Grid<2, int, double>;
	using Grid3d = Grid<3, int, double>;

	// ============== Implementation ===============
	template <int Dim, typename IntType, typename ValueType>
	bool Grid<Dim, IntType, ValueType>::IsGridIndiceInside(const Indice& indice) {
		for (int i = 0; i < Dim; ++i) {
			if (!IsGridIndexInside(indice[i])) {
				return false;
			}
		}
		return true;
	}

	template <int Dim, typename IntType, typename ValueType>
	bool Grid<Dim, IntType, ValueType>::IsGridIndexInside(IntType grid_index) {
		return kMinGridIndex <= grid_index && grid_index <= kMaxGridIndex;
	}

	template <int Dim, typename IntType, typename ValueType>
	bool Grid<Dim, IntType, ValueType>::IsValueInside(ValueType value) {
		return kMinValue <= value && value <= kMaxValue;
	}

	template <int Dim, typename IntType, typename ValueType>
	IntType Grid<Dim, IntType, ValueType>::ValueToGridIndex(ValueType value) {
		CHECK(!std::isnan(value));
		value = std::clamp(value, kMinValue, kMaxValue);
		IntType index = static_cast<IntType>(value);
		if (value < 0.0) {
			--index;
		}
		return std::clamp(index, kMinGridIndex, kMaxGridIndex);
	}

	template <int Dim, typename IntType, typename ValueType>
	ValueType Grid<Dim, IntType, ValueType>::GridIndexToStartValue(IntType grid_index) {
		grid_index = std::clamp(grid_index, kMinGridIndex, kMaxGridIndex);
		return static_cast<ValueType>(grid_index);
	}

}  // namespace geom
