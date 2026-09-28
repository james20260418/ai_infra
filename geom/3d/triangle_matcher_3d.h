// geom/3d/triangle_matcher_3d.h — 「点到最近三角形」查询容器（体素栅格加速）
//
// 问题：给定空间一点，快速找到离它最近的三角形（三角形来自一个几万面的网格，
// 如人体 glb）。朴素 brute force 是 O(三角形数)；本容器降到 O(1) 查表 + 一个很小的常数。
//
// 做法（体素栅格 + 两级筛选）：
//   1) 用 grid 把世界坐标体素化：体素边长 = grid_size（调用方给，典型 0.01 = 1cm，
//      与 JPOV 的米制一致）。只有"被写过的体素"占内存（稀疏哈希表 GridMap）。
//   2) 建"全量"表（FindAllRecentTriangles 用）：对每个三角形 t，把它写进所有**中心距
//      t ≤ local_distance + 体素半径**的体素。体素半径 = 0.5*sqrt(3)*grid_size，即
//      体素中心到体素内任意点的最大距离。
//      ⇒ 召回保证：任何点 p 满足 dist(p, t) ≤ local_distance，则 t 一定在 p 所在体素的
//        列表里。证明：设 p 所在体素中心为 c，则
//          dist(c, t) ≤ dist(c, p) + dist(p, t) ≤ 体素半径 + local_distance，故 t 被写入。
//      （注意是 **"+ 体素半径"**：这里必须以体素中心为基准向外**放宽**，否则
//        dist(c, t) 可能落在 (local_distance − 体素半径, local_distance + 体素半径]
//        区间而被漏掉——即以体素中心为基准时，正确的写入半径是 R + r，不是 R − r。）
//   3) 建"淘汰"表（FindNearestTriangles 用）：对每个体素，先算其中心 c 到桶内三角形的
//      最近距离 d0(c)，再把"中心距它 > d0(c) + 2*体素半径"的三角形剔掉。
//      ⇒ 最近邻保证：对体素内任意点 p，"离 p 最近的三角形"一定留在桶里。证明：设 T*
//        为 p 的最近三角形、d* = dist(p, T*)、T0 为离 c 最近的三角形、d0 = dist(c, T0)：
//          dist(c, T*) ≤ dist(c, p) + dist(p, T*) ≤ 体素半径 + d*
//          d* ≤ dist(p, T0) ≤ dist(p, c) + dist(c, T0) ≤ 体素半径 + d0
//        ⇒ dist(c, T*) ≤ d0 + 2*体素半径，故 T* 不会被剔掉。
//      淘汰在**构造时离线预计算**，故查询本身只是 O(1) 的哈希查找。
//
// 于是上层（衣物排斥顶点）只需：FindNearestTriangles → 遍历候选找真正最近的那个 →
// 判里外。至于距离 > local_distance 的点（衣物外侧较远处、或深陷体内超过 local_distance
// 的点），本容器**不保证**给出结果——按需求者判断，靠衣物整体的拉扯解决，不做 brute force 兜底。
//
// 返回 const std::vector<int>& —— 指向容器内部缓存的三角形下标表，直接遍历访问快。
//   - 生命周期：结果在**修改本容器前**一直有效；本容器构造后不再改动，故可放心持有引用。
//   - 未命中（该点所在体素没有条目）返回一个稳定的空表引用。
//   - 非线程安全（共享内部表引用）；如需并发另加接口。
//
// 单测策略：与 brute force（遍历全部三角形取最小值）对照，见 triangle_matcher_3d_test.cc。

#pragma once

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

#include "geom/3d/triangle_3.h"
#include "geom/common/check.h"
#include "geom/common/grid.h"
#include "geom/common/grid_map.h"
#include "geom/common/vec.h"

namespace geom {

	template <typename T>
	class TriangleMatcher3d {
	public:
		// Pre-condition（不满足即 LOG(FATAL)）：
		//   - local_distance > 0：查询半径（单位与 triangles 一致）；
		//   - grid_size > 0：体素边长（同上）；
		//   - triangles 非空：对着空气建匹配器是编程错误。
		//
		// triangles 按**值**保存（本容器持有其副本）；查询返回的下标即该副本的下标。
		//
		// 坐标范围：三角形与查询点须落在体素栅格可表示范围内（|坐标 / grid_size| < 2^31）；
		//   更远会被 grid 夹断到同一极端体素、使结果失真（本工具的使用范围——人体 glb，米制
		//   ——与之相差多个数量级，不会碰到）。查询点须有限（NaN/Inf 会触发 grid 的 CHECK）。
		//
		// 内存：同时保留“全量表”（all_recent，仅供 FindAllRecentTriangles）与“淘汰表”（nearest）
		//   两套体素桶。若上层只用 FindNearestTriangles，可省掉全量表（需另一套“只存每体素
		//   最近距离”的中间结构，暂未实现）。
		TriangleMatcher3d(T local_distance, T grid_size, std::vector<Triangle3<T>> triangles);

		// 返回 point 所在**体素**桶内的三角形下标（= 该体素中心到三角形距离 ≤ local_distance +
		// 体素半径）。它是「距 point ≤ local_distance 的三角形」的**超集**（保召回，可能含略远者）。
		const std::vector<int>& FindAllRecentTriangles(const Vec3<T>& point) const;

		// 返回"可能是 point 最近邻"的三角形下标（已按体素离线淘汰，见文件头）。O(1)。
		// 保证：point 的**真正最近邻**若其所在体素有非空条目，必在其中。
		const std::vector<int>& FindNearestTriangles(const Vec3<T>& point) const;

		T local_distance() const {
			return local_distance_;
		}

		T grid_size() const {
			return grid_size_;
		}

		const std::vector<Triangle3<T>>& triangles() const {
			return triangles_;
		}

	private:
		using Grid3T = Grid<3, int, T>;
		using VoxelIndex = typename Grid3T::Indice;
		using Buckets = GridMap<std::vector<int>, Grid3T>;

		// 世界坐标 → 体素索引（按 1/grid_size 缩放后向下取整）。
		VoxelIndex VoxelIndexOf(const Vec3<T>& point) const;

		// 体素索引 → 体素中心的世界坐标。
		Vec3<T> VoxelCenterOf(const VoxelIndex& index) const;

		void BuildAllRecentBuckets();
		void BuildNearestBuckets();

		T local_distance_;
		T grid_size_;
		T voxel_radius_;  // 0.5*sqrt(3)*grid_size：体素中心到体素内任意点的最大距离
		std::vector<Triangle3<T>> triangles_;
		Buckets all_recent_buckets_;
		Buckets nearest_buckets_;
	};

	// ============== Implementation ===============

	template <typename T>
	TriangleMatcher3d<T>::TriangleMatcher3d(T local_distance, T grid_size,
		std::vector<Triangle3<T>> triangles)
		: local_distance_(local_distance), grid_size_(grid_size),
		triangles_(std::move(triangles)) {
		CHECK_GT(local_distance_, T(0)) << "TriangleMatcher3d: local_distance 必须 > 0。";
		CHECK_GT(grid_size_, T(0)) << "TriangleMatcher3d: grid_size 必须 > 0。";
		CHECK(!triangles_.empty()) << "TriangleMatcher3d: triangles 不能为空。";

		// 体素中心到体素内任意点的最大距离 = 半体对角线（grid_size 见文件头）。
		voxel_radius_ = static_cast<T>(std::sqrt(3.0) / 2.0) * grid_size_;

		BuildAllRecentBuckets();
		BuildNearestBuckets();
	}

	template <typename T>
	typename TriangleMatcher3d<T>::VoxelIndex TriangleMatcher3d<T>::VoxelIndexOf(
		const Vec3<T>& point) const {
		VoxelIndex index;
		index[0] = Grid3T::ValueToGridIndex(point.x() / grid_size_);
		index[1] = Grid3T::ValueToGridIndex(point.y() / grid_size_);
		index[2] = Grid3T::ValueToGridIndex(point.z() / grid_size_);
		return index;
	}

	template <typename T>
	Vec3<T> TriangleMatcher3d<T>::VoxelCenterOf(const VoxelIndex& index) const {
		const T half = T(0.5);
		return Vec3<T>((static_cast<T>(index[0]) + half) * grid_size_,
			(static_cast<T>(index[1]) + half) * grid_size_,
			(static_cast<T>(index[2]) + half) * grid_size_);
	}

	template <typename T>
	void TriangleMatcher3d<T>::BuildAllRecentBuckets() {
		// 写入判据：体素中心到三角形距离 ≤ local_distance + 体素半径（见文件头证明）。
		const T insert_radius = local_distance_ + voxel_radius_;

		for (int tri = 0; tri < static_cast<int>(triangles_.size()); ++tri) {
			const Triangle3<T>& triangle = triangles_[tri];
			const Vec3<T>& a = triangle.a();
			const Vec3<T>& b = triangle.b();
			const Vec3<T>& c = triangle.c();

			// 三角形轴对齐包围盒，向外放宽 insert_radius（覆盖所有可能被写入的体素中心）。
			const Vec3<T> box_min(std::min({ a.x(), b.x(), c.x() }) - insert_radius,
				std::min({ a.y(), b.y(), c.y() }) - insert_radius,
				std::min({ a.z(), b.z(), c.z() }) - insert_radius);
			const Vec3<T> box_max(std::max({ a.x(), b.x(), c.x() }) + insert_radius,
				std::max({ a.y(), b.y(), c.y() }) + insert_radius,
				std::max({ a.z(), b.z(), c.z() }) + insert_radius);

			const VoxelIndex lo = VoxelIndexOf(box_min);
			const VoxelIndex hi = VoxelIndexOf(box_max);
			const T insert_radius_sqr = insert_radius * insert_radius;

			for (int ix = lo[0]; ix <= hi[0]; ++ix) {
				for (int iy = lo[1]; iy <= hi[1]; ++iy) {
					for (int iz = lo[2]; iz <= hi[2]; ++iz) {
						const VoxelIndex voxel = { ix, iy, iz };
						const Vec3<T> center = VoxelCenterOf(voxel);
						if (triangle.DistanceSquareTo(center) <= insert_radius_sqr) {
							all_recent_buckets_[voxel].push_back(tri);
						}
					}
				}
			}
		}
	}

	template <typename T>
	void TriangleMatcher3d<T>::BuildNearestBuckets() {
		for (const auto& entry : all_recent_buckets_) {
			const VoxelIndex& voxel = entry.first;
			const std::vector<int>& candidates = entry.second;
			const Vec3<T> center = VoxelCenterOf(voxel);

			// 中心到桶内最近三角形的距离 d0（先用平方距离找最小）。
			T nearest_sqr = std::numeric_limits<T>::max();
			for (const int tri : candidates) {
				nearest_sqr = std::min(nearest_sqr, triangles_[tri].DistanceSquareTo(center));
			}

			// 淘汰距离 = d0 + 2*体素半径（见文件头证明）；用平方比较以免开方。
			const T cull = std::sqrt(nearest_sqr) + T(2) * voxel_radius_;
			const T cull_sqr = cull * cull;

			std::vector<int> kept;
			for (const int tri : candidates) {
				if (triangles_[tri].DistanceSquareTo(center) <= cull_sqr) {
					kept.push_back(tri);
				}
			}
			nearest_buckets_[voxel] = std::move(kept);
		}
	}

	template <typename T>
	const std::vector<int>& TriangleMatcher3d<T>::FindAllRecentTriangles(
		const Vec3<T>& point) const {
		const VoxelIndex voxel = VoxelIndexOf(point);
		const typename Buckets::const_iterator it = all_recent_buckets_.find(voxel);
		if (it == all_recent_buckets_.end()) {
			static const std::vector<int> kEmpty;
			return kEmpty;
		}
		return it->second;
	}

	template <typename T>
	const std::vector<int>& TriangleMatcher3d<T>::FindNearestTriangles(
		const Vec3<T>& point) const {
		const VoxelIndex voxel = VoxelIndexOf(point);
		const typename Buckets::const_iterator it = nearest_buckets_.find(voxel);
		if (it == nearest_buckets_.end()) {
			static const std::vector<int> kEmpty;
			return kEmpty;
		}
		return it->second;
	}

}  // namespace geom
