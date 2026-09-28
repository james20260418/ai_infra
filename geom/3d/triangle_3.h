// geom/3d/triangle_3.h — 空间三角形（3D，非退化）
//
// Triangle3<T> 表示一个**非退化**的空间三角形：三个不共线（面积不为 0）的顶点
// (a, b, c)。顶点按 a→b→c 逆时针给出时，法向 normal = (b-a) × (c-a) 指向三角形
// 的"正面"（右手系，与 geom::Vec 一致）。
//
// 设计取舍：
//   - **拒绝退化**：三点共线 / 两两重合 / 面积趋近 0 时，三角形没有唯一的平面与
//     法向，"最近点 / 里外"等语义含糊，且求最近点的公式会出现 0/0。与其构造一个
//     坏对象、把问题推迟到调用处，不如在构造处直接拒绝——因此本类**没有公开构造
//     函数**，只提供静态工厂 Create()，退化时返回 std::nullopt，把"失败"显式摆在
//     类型系统里（调用者必须处理，忘不了）。
//   - 退化判据用**无量纲细长比**（2*面积 ≤ kDegenerateAreaRatio * 最长边²，等价于
//     三角形的最小高 ≤ 1e-6 * 最长边），与尺度无关：厘米/米两种单位下行为一致。
//   - 只依赖 geom::Vec，不含任何 GL / 渲染 / 动画概念（与 geom/2d/segment_2 平级）。
//
// Pre-condition（Create 内校验，不满足即 LOG(FATAL)）：
//   - 三个顶点必须有限（IsFinite）；NaN/Inf 是编程错误，不作为"退化"静默吞掉。
//
// 单位：本类不做任何换算，与顶点所在坐标系一致（JPOV 侧为米）。

#pragma once

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>

#include "geom/common/check.h"
#include "geom/common/common.h"
#include "geom/common/vec.h"

namespace geom {

	// 空间三角形（三个非退化顶点 a、b、c）。
	template <typename T>
	class Triangle3 {
	public:
		// 构造工厂：三点构成非退化三角形时返回 Triangle3，否则返回 std::nullopt。
		//
		// Pre-condition：a、b、c 必须有限（否则 LOG(FATAL)）。
		//
		// 退化判据（见 kDegenerateAreaRatio）：2*面积 ≤ kDegenerateAreaRatio * 最长边²。
		// 该式同时覆盖"两点重合""三点重合"（此时最长边为 0，右端为 0，左端也为 0）。
		static std::optional<Triangle3> Create(const Vec3<T>& a, const Vec3<T>& b,
			const Vec3<T>& c);

		const Vec3<T>& a() const {
			return a_;
		}

		const Vec3<T>& b() const {
			return b_;
		}

		const Vec3<T>& c() const {
			return c_;
		}

		// 单位法向 normal = (b-a) × (c-a) 归一化；顶点逆时针 → 指向"正面"。
		const Vec3<T>& normal() const {
			return normal_;
		}

		// 三角形上（含内部 / 边 / 顶点）离 point 最近的点。
		// 用 Ericson《Real-Time Collision Detection》§5.1.5 的分区域求法：先判断最近点
		// 落在哪个顶点/边/面区域，再在该区域内解析求投影。详见 .cc 实现处注释。
		Vec3<T> ClosestPointTo(const Vec3<T>& point) const;

		// point 到三角形的**平方**距离（= 到最近点的平方距离）。
		T DistanceSquareTo(const Vec3<T>& point) const;

		// point 到三角形的距离。
		T DistanceTo(const Vec3<T>& point) const {
			return std::sqrt(DistanceSquareTo(point));
		}

		std::string DebugString() const;

		// 退化判据的（无量纲）细长比阈值：2*面积 ≤ 此值 * 最长边² 即判为退化。
		// 1e-6 ⇒ 三角形最短高 ≤ 1e-6 * 最长边时拒绝（只挡真正的细长退化，不误伤网格里
		// 正常的细三角形）。要更宽松/更严格直接改这里。
		static constexpr T kDegenerateAreaRatio = static_cast<T>(1e-6);

	private:
		// 只能经 Create 构造（见文件头"拒绝退化"）。
		Triangle3(const Vec3<T>& a, const Vec3<T>& b, const Vec3<T>& c, const Vec3<T>& normal)
			: a_(a), b_(b), c_(c), normal_(normal) {}

		Vec3<T> a_;
		Vec3<T> b_;
		Vec3<T> c_;
		Vec3<T> normal_;
	};

	using Triangle3d = Triangle3<double>;
	using Triangle3f = Triangle3<float>;

	// ============== Implementation ===============

	template <typename T>
	std::optional<Triangle3<T>> Triangle3<T>::Create(const Vec3<T>& a, const Vec3<T>& b,
		const Vec3<T>& c) {
		CHECK(a.IsFinite() && b.IsFinite() && c.IsFinite())
			<< "Triangle3::Create: 顶点必须有限（NaN/Inf 属编程错误，不作退化处理）。";

		const Vec3<T> ab = b - a;
		const Vec3<T> ac = c - a;
		const Vec3<T> cross = ab.Cross(ac);
		const T double_area = cross.Norm();

		// 最长边（两两距离的最大值）作为尺度基准。
		const T max_edge_sqr = std::max({ ab.Sqr(), ac.Sqr(), (c - b).Sqr() });

		if (double_area <= kDegenerateAreaRatio * max_edge_sqr) {
			return std::nullopt;  // 退化：共线 / 重合 / 面积趋近 0。
		}

		return Triangle3(a, b, c, cross * (T(1) / double_area));
	}

	template <typename T>
	Vec3<T> Triangle3<T>::ClosestPointTo(const Vec3<T>& p) const {
		// 记顶点为 A=a_、B=b_、C=c_。以下按 P 在 AABB 外接区域的划分选最近特征，
		// 变量 d1..d6 是 P 相对三条边的投影点积（Ericson §5.1.5 的标准记号）。
		const Vec3<T> ab = b_ - a_;
		const Vec3<T> ac = c_ - a_;
		const Vec3<T> ap = p - a_;

		// 顶点 A 区域。
		const T d1 = ab.Dot(ap);
		const T d2 = ac.Dot(ap);
		if (d1 <= T(0) && d2 <= T(0)) {
			return a_;
		}

		// 顶点 B 区域。
		const Vec3<T> bp = p - b_;
		const T d3 = ab.Dot(bp);
		const T d4 = ac.Dot(bp);
		if (d3 >= T(0) && d4 <= d3) {
			return b_;
		}

		// 边 AB 区域（VC 是 P 相对 AB 的重心权重分子，≤0 表示在 AB 外侧）。
		const T vc = d1 * d4 - d3 * d2;
		if (vc <= T(0) && d1 >= T(0) && d3 <= T(0)) {
			const T v = d1 / (d1 - d3);
			return a_ + ab * v;
		}

		// 顶点 C 区域。
		const Vec3<T> cp = p - c_;
		const T d5 = ab.Dot(cp);
		const T d6 = ac.Dot(cp);
		if (d6 >= T(0) && d5 <= d6) {
			return c_;
		}

		// 边 AC 区域。
		const T vb = d5 * d2 - d1 * d6;
		if (vb <= T(0) && d2 >= T(0) && d6 <= T(0)) {
			const T w = d2 / (d2 - d6);
			return a_ + ac * w;
		}

		// 边 BC 区域。
		const T va = d3 * d6 - d5 * d4;
		if (va <= T(0) && (d4 - d3) >= T(0) && (d5 - d6) >= T(0)) {
			const T w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
			return b_ + (c_ - b_) * w;
		}

		// 面内部区域：P 在三角形三边内侧，用重心坐标投影。
		// Create 已保证非退化 ⇒ va + vb + vc > 0（分母不为 0）。
		const T denom = T(1) / (va + vb + vc);
		const T v = vb * denom;
		const T w = vc * denom;
		return a_ + ab * v + ac * w;
	}

	template <typename T>
	T Triangle3<T>::DistanceSquareTo(const Vec3<T>& point) const {
		return (point - ClosestPointTo(point)).Sqr();
	}

	template <typename T>
	std::string Triangle3<T>::DebugString() const {
		return StrFormat("Triangle3{%s,%s,%s}", a_.DebugString().c_str(),
			b_.DebugString().c_str(), c_.DebugString().c_str());
	}

}  // namespace geom
