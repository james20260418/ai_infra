// geom/common/grid_map_test.cc — GridMap（稀疏哈希栅格）单测（GTest）
//
// 思路：用一个「稠密网格」作参考实现（DenseGrid，按索引展平存数组），对 GridMap 做
// 随机增/删/改的**一致性 fuzz**——每轮随机动作后，两容器的元素集合必须逐项相等
// （键存在性 + 值），且元素个数相等。随机动作含「写/覆盖」与「删除」两种，保证
// GridMap 的插入、覆盖、删除路径都被覆盖。
#include "geom/common/grid_map.h"

#include <array>
#include <cstddef>
#include <optional>
#include <random>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "geom/common/common.h"

namespace {

	using geom::Grid;
	using geom::GridMap;
	using geom::RandomDouble;
	using geom::RandomInt;

	// 稠密参考实现：把 Dim 维索引展平成一维数组（无元素的格为 std::nullopt）。
	template <int Dim>
	class DenseGrid {
	public:
		static_assert(Dim > 0);

		explicit DenseGrid(std::array<int, Dim> shape) : shape_(std::move(shape)) {
			list_size_ = 1;
			for (int i = 0; i < Dim; ++i) {
				CHECK_GT(shape_[i], 0);
				list_size_ *= static_cast<size_t>(shape_[i]);
			}
			list_.resize(list_size_);

			stride_[Dim - 1] = 1;
			for (int i = Dim - 2; i >= 0; --i) {
				stride_[i] = stride_[i + 1] * static_cast<size_t>(shape_[i + 1]);
			}
		}

		void SetValue(const std::array<int, Dim>& indices, std::optional<int> value) {
			list_[FlatIndex(indices)] = value;
		}

		const std::optional<int>& At(const std::array<int, Dim>& indices) const {
			return list_[FlatIndex(indices)];
		}

		const std::vector<std::optional<int>>& list() const { return list_; }

	private:
		size_t FlatIndex(const std::array<int, Dim>& indices) const {
			size_t list_index = 0;
			for (int i = Dim - 1; i >= 0; --i) {
				list_index += static_cast<size_t>(indices[i]) * stride_[i];
			}
			return list_index;
		}

		std::array<int, Dim> shape_;
		std::array<size_t, Dim> stride_;
		size_t list_size_ = 0;
		std::vector<std::optional<int>> list_;
	};

	// 断言 GridMap 与稠密参考逐项一致：元素集合相同且个数相同。
	template <int Dim>
	void CheckIdentical(const DenseGrid<Dim>& dense_grid,
		const GridMap<int, Grid<Dim>>& grid_map) {
		using Indice = typename Grid<Dim>::Indice;

		// GridMap 中每个元素，在稠密网格里必须存在且相等。
		for (const auto& indice_and_value : grid_map) {
			const Indice& indice = indice_and_value.first;
			const std::optional<int> dense_element = dense_grid.At(indice);
			ASSERT_TRUE(dense_element.has_value());
			EXPECT_EQ(*dense_element, indice_and_value.second);
		}

		// 元素个数必须相等。
		size_t num_elements_in_dense_grid = 0;
		for (const std::optional<int>& optional_int : dense_grid.list()) {
			if (optional_int.has_value()) {
				++num_elements_in_dense_grid;
			}
		}
		EXPECT_EQ(num_elements_in_dense_grid, grid_map.size());
	}

	// 一次随机动作：多数时候写/覆盖某个格，少数时候删除某个格。两容器同步操作。
	template <int Dim>
	void RandomAction(int max_index, DenseGrid<Dim>* dense_grid,
		GridMap<int, Grid<Dim>>* grid_map, std::mt19937* seed) {
		typename Grid<Dim>::Indice indice;
		for (int i = 0; i < Dim; ++i) {
			indice[i] = RandomInt(0, max_index, seed);
		}

		const double r = RandomDouble(0.0, 1.0, seed);
		if (r > 0.2) {
			// 写 & 覆盖。
			const int value = RandomInt(0, 100, seed);
			dense_grid->SetValue(indice, value);
			(*grid_map)[indice] = value;
		}
		else {
			// 存在才删除。
			if (grid_map->find(indice) != grid_map->end()) {
				dense_grid->SetValue(indice, std::nullopt);
				grid_map->erase(indice);
			}
		}
	}

	// 单组 fuzz：在 BoxSize^Dim 的格空间里做多轮随机动作，每轮后校验一致性。
	template <int Dim, int BoxSize>
	void CompareWithDenseGridSingleTest() {
		std::array<int, Dim> grid_shape;
		grid_shape.fill(BoxSize);

		GridMap<int, Grid<Dim>> grid_map;
		DenseGrid<Dim> dense_grid(std::move(grid_shape));

		constexpr int kNumCheck = 20;
		constexpr int kNumActionPerCheck = 500;

		std::mt19937 seed(0);
		for (int i = 0; i < kNumCheck; ++i) {
			for (int j = 0; j < kNumActionPerCheck; ++j) {
				RandomAction<Dim>(BoxSize - 1, &dense_grid, &grid_map, &seed);
			}
			CheckIdentical<Dim>(dense_grid, grid_map);
		}
	}

	// 2~6 维各测一组（维度越高格空间越大，为控制耗时逐维缩小 BoxSize）。
	TEST(GridMapTest, MatchesDenseGrid) {
		CompareWithDenseGridSingleTest</*Dim=*/2, /*BoxSize=*/100>();
		CompareWithDenseGridSingleTest</*Dim=*/3, /*BoxSize=*/100>();
		CompareWithDenseGridSingleTest</*Dim=*/4, /*BoxSize=*/30>();
		CompareWithDenseGridSingleTest</*Dim=*/5, /*BoxSize=*/10>();
		CompareWithDenseGridSingleTest</*Dim=*/6, /*BoxSize=*/10>();
	}

}  // namespace
