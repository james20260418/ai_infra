// geom/common/grid_map.h — 以格索引为键的稀疏栅格（哈希表）
//
// 与 grid.h 配套：grid.h 定义「坐标 ↔ 格索引」的换算，本文件提供一种**稀疏**存储
// ——把格索引（Grid::Indice，N 维整数数组）当键、任意类型当值的哈希表。只有被显式
// 写入的格才占内存，适合「大部分格为空」的场景（稠密场景请直接用数组）。
//
// 用法：
//   geom::GridMap2d<int> occ;              // 2D、值为 int 的稀疏栅格
//   occ[{0, 3}] = 1;                       // 写入格 (0,3)
//   occ.erase({0, 3});                     // 删除格 (0,3)
//   if (occ.find({1, 2}) != occ.end()) { ... }
//
// 说明：
//   - Indice 的哈希/相等比较按 GridType::kDim 逐维进行（见 GridIndiceHash/Equal）。
//   - 值为 std::optional<T> 的等价替代：需要区分「格存在但值为空」时，用 T = std::optional<...>。
//
// 参考：
//   https://en.cppreference.com/w/cpp/container/unordered_map
//   https://ianyepan.github.io/posts/cpp-custom-hash/

#pragma once

#include <cstddef>
#include <functional>
#include <unordered_map>

#include "geom/common/grid.h"

namespace geom {

	// 格索引的哈希函数（逐维取 std::hash 后异或合并）。
	template <typename GridType>
	struct GridIndiceHash {
		size_t operator()(const typename GridType::Indice& indice) const {
			size_t result = 0;
			for (int i = 0; i < GridType::kDim; ++i) {
				result ^= std::hash<typename GridType::IntT>{}(indice[i]);
			}
			return result;
		}
	};

	// 格索引的相等比较（逐维比较）。
	template <typename GridType>
	struct GridIndiceEqual {
		bool operator()(const typename GridType::Indice& indice_1,
			const typename GridType::Indice& indice_2) const {
			for (int i = 0; i < GridType::kDim; ++i) {
				if (indice_1[i] != indice_2[i]) {
					return false;
				}
			}
			return true;
		}
	};

	// 稀疏栅格：键 = GridType::Indice，值 = ElementType。
	template <typename ElementType, typename GridType>
	using GridMap = std::unordered_map<
		/*Key=*/typename GridType::Indice,
		/*T=*/ElementType,
		/*Hash=*/GridIndiceHash<GridType>,
		/*KeyEqual=*/GridIndiceEqual<GridType>>;

	// 2D 便捷别名。
	template <typename ElementType>
	using GridMap2d = GridMap<ElementType, Grid2d>;

	// ========== Specializations ========
	// 例如：若对 2 维 int 型稀疏栅格有更快的实现，可在此特化。

}  // namespace geom
