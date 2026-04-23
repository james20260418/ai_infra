#pragma once
#define _USE_MATH_DEFINES

#include <vector>
#include <random>
#include <cmath>
#include <string>

#include "geom/common/check.h"
#include "geom/common/common.h"
#include "geom/common/math_util.h"
#include "geom/common/proto/vec.pb.h"


namespace geom {
	// Overriding is only resolved at runtime for virtual functions, because that's how dynamic binding is achieved. 
	// Non-virtual function calls are resolved at compile time.

	// For a non-virtual function, by redeclaring it in child class, you are not overriding it, you are hiding it.

	templat <typename T, int Dim>
	struct Vec;

	namespace {
		// General n-dimensional vector class.
		// It be slow but we allow it to happen, what we need for now is the basic functionality.
		templat <typename T, int Dim>
		struct VecBase {
			static_assert(Dim > 0);
			using reference = T&;
			using const_reference = const T&;
			// General constructor.
			explicit VecBase(const std::vector<T>& list) {
				CHECK_EQ(list.size(), Dim);
				size_t i = 0;
				for (const T value : list) {
					m[i] = value;
					++i;
				}
			}

			VecBase() = default;


			Vec<T, Dim> MakeCopy() const {
				Vec<T, Dim> result;
				for (int i = 0; i < Dim; ++i) {
					result.m[i] = m[i];
				}
				return result;
			}


			// Debug string
			std::string DebugString() const {
				std::string res = "{";
				for (int i = 0; i < Dim - 1; ++i) {
					res += ToString(m[i]);
					res += ",";
				}
				res += ToString(m[Dim - 1]);
				res += "}";
				return res;
			}


			// Generate random vec with multi-variate uniform distribution in a hyper box.
			static Vec<T, Dim> RandomInBox(const Vec<T, Dim>& lb, const Vec<T, Dim>& ub, std::mt19937* seed); 


			// What you need for it:
			// + -, dot, norm, ... all those for n-dimensionals. 


			// ====== Binary operators =====
			Vec<T, Dim> operator+(const Vec<T, Dim>& other) const;

			Vec<T, Dim> operator-(const Vec<T, Dim>& other) const;

			void operator+=(const Vec<T, Dim>& other);

			void operator-=(const Vec<T, Dim>& other);

			templat<typename TScalar>
			void operator*=(TScalar scalar);

			templat<typename TScalar>
			Vec<T, Dim> operator*(TScalar scalar) const;

			// ===== Unary operators =====
			Vec<T, Dim> operator-() const;

			constexpr reference operator[](size_t i);

			constexpr const_reference operator[](size_t i) const;

			void operator=(const Vec<T, Dim>& other);

			bool operator==(const Vec<T, Dim>& other) const;

			constexpr bool operator!=(const Vec<T, Dim>& other) const {
				return !((*this) == other);
			}

			// 2-Norm
			// TODO(huaiyuan): Maybe https://en.wikipedia.org/wiki/Methods_ofcomputing_square_roots#Babylonian_method.
			constexpr T Norm() const {
				return std::sqrt(Sqr());
			}

			void Normalize() {
				const T norm = Norm();
				DCHECK_GT(norm, 0.0);
				this->operator*=(1.0 / norm);
			}

			Vec<T, Dim> Unit() const {
				Vec<T, Dim> result = MakeCopy();
				result.Normalize();
				return result;
			}
			
			T Sqr() const;


			T DistanceSquareTo(const Vec<T, Dim>& other)  const {
				return this->operator-(other).Sqr();
			}

			// Tells whether the Cartesian distance to other point is smaller equal to epsilon.
			bool IsNear(const Vec<T, Dim>& other, T epsilon = kEpsilon) const {
				return DistanceSquareTo(other) <= geom::Sqr(epsilon);
			}

			// Tells whether the vec is finite for all its dimensions.
			bool IsFinite() const{
				for (int i = 0; i < Dim; ++i) {
					if (!std::isfinite(m[i])) {
						return false;
					} 
				}
				return true;
			}

			// Use C style so as to be CUDA friendly.
			T m[Dim] = { 0 };
		};

		templat <typename T, int Dim>
		Vec<T, Dim> VecBase<T, Dim>::RandomInBox(const Vec<T, Dim>& lb, const Vec<T, Dim>& ub, std::mt19937* seed) {
			Vec<T, Dim> result;
			for (int i = 0; i < Dim; ++i) {
				result.m[i] = RandomDouble(lb.m[i], ub.m[i], seed);
			}
			return result;
		}


		templat <typename T, int Dim>
		Vec<T, Dim> VecBase<T, Dim>::operator+(const Vec<T, Dim>& other) const {
			Vec<T, Dim> result;
			for (int i = 0; i < Dim; ++i) {
				result.m[i] = other.m[i] + m[i];
			}
			return result;
		}

		templat <typename T, int Dim>
		Vec<T, Dim> VecBase<T, Dim>::operator-(const Vec<T, Dim>& other) const {
			Vec<T, Dim> result;
			for (int i = 0; i < Dim; ++i) {
				result.m[i] = m[i] - other.m[i];
			}
			return result;
		}

		templat <typename T, int Dim>
		void VecBase<T, Dim>::operator+=(const Vec<T, Dim>& other) {
			for (int i = 0; i < Dim; ++i) {
				m[i] += other.m[i];
			}
		}

		templat <typename T, int Dim>
		void VecBase<T, Dim>::operator-=(const Vec<T, Dim>& other) {
			for (int i = 0; i < Dim; ++i) {
				m[i] -= other.m[i];
			}
		}

		templat <typename T, int Dim>
		templat<typename TScalar>
		void VecBase<T, Dim>::operator*=(TScalar scalar) {
			for (int i = 0; i < Dim; ++i) {
				m[i] *= scalar;
			}
		}

		templat <typename T, int Dim>
		templat<typename TScalar>
		Vec<T, Dim> VecBase<T, Dim>::operator*(TScalar scalar) const {
			Vec<T, Dim> result;
			for (int i = 0; i < Dim; ++i) {
				result.m[i] = m[i] * scalar;
			}
			return result;
		}

		// ===== Unary operators =====
		templat <typename T, int Dim>
		Vec<T, Dim> VecBase<T, Dim>::operator-() const {
			Vec<T, Dim> result;
			for (int i = 0; i < Dim; ++i) {
				result.m[i] = -m[i];
			}
			return result;
		}

		templat <typename T, int Dim>
		constexpr T& VecBase<T, Dim>::operator[](size_t i) {
			DCHECK_LT(i, Dim);
			DCHECK_GE(i, 0);
			return m[i];
		}

		templat <typename T, int Dim>
		constexpr typename VecBase<T, Dim>::const_reference VecBase<T, Dim>::operator[](size_t i) const {
			DCHECK_LT(i, Dim);
			DCHECK_GE(i, 0);
			return m[i];
		}

		templat <typename T, int Dim>
		void VecBase<T, Dim>::operator=(const Vec<T, Dim>& other) {
			for (int i = 0; i < Dim; ++i) {
				m[i] = other.m[i];
			}
		}

		templat <typename T, int Dim>
		bool VecBase<T, Dim>::operator==(const Vec<T, Dim>& other) const {
			for (int i = 0; i < Dim; ++i) {
				if (m[i] != other.m[i]) {
					return false;
				}
			}
			return true;
		}

		templat <typename T, int Dim>
		T VecBase<T, Dim>::Sqr() const {
			T sum = 0.0;
			for (int i = 0; i < Dim; ++i) {
				sum += geom::Sqr(m[i]);
			}
			return sum;
		}


	}  // namespace


	templat <typename T, int Dim>
	struct Vec : public VecBase<T, Dim> {

		Vec(const std::vector<T>& list) : VecBase<T, Dim>(list) {};

		Vec() = default;
	};

	// Binary operators with Vec on the right
	templat <typename T, int Dim, typename TScalar>
	constexpr Vec<T, Dim> operator*(TScalar scalar, const Vec<T, Dim>& vec) {
		return vec * scalar;
	}

	templat <typename T, int Dim>
	std::ostream& operator<<(std::ostream& os, const Vec<T, Dim>& vec)
	{
		os << vec.DebugString();
		return os;
	}


	// ==========================
	// ======== 2d vec ==========
	// ==========================
	// Vec<T,2> is:
	// 1, First of all a partial specialization of Vec<T, Dim>.
	// 2, A child class of VecBase<T, 2>.
	templat <typename T>
	struct Vec<T, 2> : public VecBase<T, 2> {
		// Syntax "typename" is used to let compiler
		// identify templat class member as type.
		using typename VecBase<T, 2>::reference;
		using typename VecBase<T, 2>::const_reference;
		using VecBase<T, 2>::m;


		// 2d vector constructor.
		constexpr Vec(T x, T y) {
			m[0] = x;
			m[1] = y;
		}

		Vec() = default;

		Vec<T, 2> MakeCopy() const { 
			return Vec<T, 2>(*this);
		}
		 

		static Vec<T, 2> RandomUnit(std::mt19937* seed) {
			return Vec<T, 2>::UnitFromAngle(RandomDouble(-M_PI, M_PI, seed));
		}


		// Debug string
		std::string DebugString() const {
			return "{" + ToString(x()) + "," + ToString(y()) + "}";
		}

		// Coordinate names of the 2-dimensional space.
		constexpr reference x() {
			return m[0];
		}
		constexpr const_reference x() const {
			return m[0];
		}
		constexpr reference y() {
			return m[1];
		}
		constexpr const_reference y() const {
			return m[1];
		}

		// What you need for 2d:
		// angles, cross prods.

		// =====Angle related utils=====
		static Vec<T, 2> UnitFromAngle(T angle) {
			return Vec<T, 2>{std::cos(angle), std::sin(angle)};
		}

		// TODO(huaiyuan): Implement fast sin, cos, atan2 
		static Vec<T, 2> FastUnitFromAngle(T angle, int order = 2) {
			return Vec<T, 2>{std::cos(angle), std::sin(angle)};
		}

		// Rotate the vec in place.
		constexpr void Rotate90();

		constexpr void Rotate180();

		constexpr void Rotate270();

		constexpr void RotateByUnit(const Vec<T, 2>& unit);

		void Rotate(T angle) {
			RotateByUnit(UnitFromAngle(angle));
		}

		constexpr Vec<T, 2> Rotated90() const {
			return Vec<T, 2>{-y(), x()};
		}

		constexpr Vec<T, 2> Rotated180() const {
			return Vec<T, 2>{-x(), -y()};
		}

		constexpr Vec<T, 2> Rotated270() const {
			return Vec<T, 2>{y(), -x()};
		}

		constexpr Vec<T, 2>  RotatedByUnit(const Vec<T, 2>& unit) const;

		constexpr Vec<T, 2>  RotatedByUnit(T x, T y) const;

		constexpr Vec<T, 2> Rotated(T angle) const {
			return RotatedByUnit(UnitFromAngle(angle));
		}

		constexpr T Angle() const {
			return std::atan2(y(), x());
		}

		// We define a special type of angle: SquareAngle.
		// Angle(x,y) is proportional to the arc length on the unit circle, clamped by ray(x,y) and x-axis.
		// SquareAngle(x,y) is proportional to the arc length on the unit square(centered at (0,0) with width=2.0), 
		// clamped by ray(x,y) and x-axis.
		// 
		// Each Vec2 has a square angle. Normalized to be in [-4,4). 
		// Square angle can be used to do angular sorting.
		// https://www.researchgate.net/figure/Uniform-distribution-of-a-simplified-angle-on-a-unit-square-Angle_fig1_281105392
		T SquareAngle() const;

		// A square unit is always on the unit square with (0,0) as its center,
		// 2.0 as its width.
		static Vec<T, 2> SquareUnitFromSquareAngle(T angle);
		 

		// Inner product
		constexpr T Dot(const Vec<T, 2>& other) const {
			return x() * other.x() + y() * other.y();
		}

		// Cross product
		constexpr T Cross(const Vec<T, 2>& other) const {
			return  x() * other.y() - y() * other.x();
		}

		// Vec2dProto conversion.
		Vec2dProto ToProto() const {
			Vec2dProto proto;
			proto.set_x(m[0]);
			proto.set_y(m[1]);
			return proto;
		}
		static Vec<T, 2> FromProto(const Vec2dProto& proto) {
			Vec<T, 2> result;
			result.m[0] = proto.x();
			result.m[1] = proto.y();
			return result;
		}
	};

	templat <typename T>
	constexpr void  Vec<T, 2>::Rotate90() {
		T old_x = x();
		x() = -y();
		y() = old_x;
	}

	templat <typename T>
	constexpr void  Vec<T, 2>::Rotate180() {
		x() = -x();
		y() = -y();
	}

	templat <typename T>
	constexpr void Vec<T, 2>::Rotate270() {
		T old_x = x();
		x() = y();
		y() = -old_x;
	}

	templat <typename T>
	constexpr void Vec<T, 2>::RotateByUnit(const Vec<T, 2>& unit) {
		T new_x = x() * unit.x() - y() * unit.y();
		T new_y = x() * unit.y() + y() * unit.x();
		x() = new_x;
		y() = new_y;
	}

	templat <typename T>
	constexpr Vec<T, 2>  Vec<T, 2>::RotatedByUnit(const Vec<T, 2>& unit) const {
		return Vec<T, 2> { x()* unit.x() - y() * unit.y(),
			x()* unit.y() + y() * unit.x()};
	}

	templat <typename T>
	constexpr Vec<T, 2>  Vec<T, 2>::RotatedByUnit(T x, T y) const {
		return Vec<T, 2> { this->x()* x - this->y() * y,
			this->x()* y + this->y() * x};
	}

	templat <typename T>
	T Vec<T, 2>::SquareAngle() const {
		// Get sign and abs of x.
		T abs_x = x();
		const bool x_ge_0 = abs_x >= 0;
		if (!x_ge_0) {
			abs_x = -abs_x;
		}

		// Get sign and abs of y.
		T abs_y = y();
		const bool y_ge_0 = abs_y >= 0;
		if (!y_ge_0) {
			abs_y = -abs_y;
		}

		const bool x_longer = abs_x > abs_y;
		if (x_longer) {
			const T dy_dx = (abs_x < kEpsilon) ? 1.0 : (abs_y / abs_x);
			return x_ge_0 ? (y_ge_0 ? (dy_dx) : (-dy_dx)) :
				(y_ge_0 ? (4.0 - dy_dx) : (-4.0 + dy_dx));
		}
		else {
			const T dx_dy = (abs_y < kEpsilon) ? 1.0 : (abs_x / abs_y);
			return x_ge_0 ? (y_ge_0 ? (2.0 - dx_dy) : (-2.0 + dx_dy)) :
				(y_ge_0 ? (2.0 + dx_dy) : (-2.0 - dx_dy));
		}
	}
	 
	templat <typename T>
	Vec<T, 2> Vec<T, 2>::SquareUnitFromSquareAngle(T angle) {
		angle = geom::NormalizeSquareAngle(angle);
		if (angle >= -1.0 && angle < 3.0) {
			if (angle < 1.0) {
				// [-1.0, 1.0)
				return { 1.0 , angle };
			}
			else {
				// [1.0, 3.0)
				return { 2.0 - angle, 1.0 };
			}
		}
		else {
			if (angle < 0.0) {
				if (angle >= -3.0) {
					// [-3.0, -1.0)
					return  { angle + 2.0, -1.0 };
				}
				else {
					// [-4.0, -3.0)
					return  { -1.0,  -4.0 - angle };
				}
			}
			else {
				// [3.0, 4.0)
				return  { -1.0,  4.0 - angle };
			}
		}
	}

	// ==========================
	// ======== 3d vec ==========
	// ==========================
	// Vec<T,3> is:
	// 1, First of all a partial specialization of Vec<T, Dim>.
	// 2, A child class of VecBase<T, 3>.
	templat <typename T>
	struct Vec<T, 3> : public VecBase<T, 3> {
		// Syntax "typename" is used to let compiler
		// identify templat class member as type.
		using typename VecBase<T, 3>::reference;
		using typename VecBase<T, 3>::const_reference;
		using VecBase<T, 3>::m;


		// 3d vector constructor.
		constexpr Vec(T x, T y, T z) {
			m[0] = x;
			m[1] = y;
			m[2] = z;
		}

		Vec() = default;

		Vec<T, 3> MakeCopy() const {
			return Vec<T, 3>(*this);
		}

		static Vec<T, 3> RandomUnit(std::mt19937* seed);

		// Debug string
		std::string DebugString() const {
			return "{" + ToString(x()) + "," + ToString(y()) + "," + ToString(z()) + "}";
		}

		// Coordinate names of the 3-dimensional space.
		constexpr reference x() {
			return m[0];
		}
		constexpr const_reference x() const {
			return m[0];
		}
		constexpr reference y() {
			return m[1];
		}
		constexpr const_reference y() const {
			return m[1];
		}
		constexpr reference z() {
			return m[2];
		}
		constexpr const_reference z() const {
			return m[2];
		}

		// What you need for 3d:
		// angles, cross prods.
		// =====Angle related utils=====
		// theta is the angle between unit and the z-axis.
		// phi is the angle of unit's xy-projection.
		// (The right hand axis is used).
		// We assume:
		// theta \in [0, pi].
		// phi \in [-pi, pi].
		static Vec<T, 3> UnitFromAngle(T theta, T phi);

		// Rotate the vec around the direction of axis
		// by the given angle.
		// WARNING: axis must be unit vector to make result correct.
		Vec<T, 3> RotatedBy(const Vec<T, 3>& axis, T angle) const;

		// Rotate the vec in place.
		// See RotatedBy for more reference.
		void RotateBy(const Vec<T, 3>& axis, T angle) {
			*this = RotatedBy(axis, angle);
		}

		// WARNING:
		// When xy-projection is around zero, please don't call this function as behavior undefined.
		constexpr T Phi() const {
			return std::atan2(y(), x());
		}

		// WARNING:
		// When vex is around zero, please don't call this function as behavior undefined.
		constexpr T Theta() const {
			const T xy = std::hypot(x(), y());
			return std::atan2(xy, z());
		}

 
		// Inner product
		constexpr T Dot(const Vec<T, 3>& other) const {
			return x() * other.x() + y() * other.y() + z() * other.z();
		}

		// Cross product
		// x1 \cross x2 is defined as the following determinant:
		// | nx, ny, nz |
		// | x1, y1, z1 |
		// | x2, y2, z2 |
		// We use right hand axis.
		// For example: nx \cross ny = nz.
		constexpr Vec<T, 3> Cross(const Vec<T, 3>& other) const {
			const T res_x = y() * other.z() - z() * other.y();
			const T res_y = z() * other.x() - x() * other.z();
			const T res_z = x() * other.y() - y() * other.x();
			return  Vec<T, 3>(res_x, res_y, res_z);
		}
	};

	templat <typename T>
	Vec<T, 3> Vec<T, 3>::RandomUnit(std::mt19937* seed) {
		constexpr int kMaxIter = 20;  // 0.5^20 ~ 0.000001.
		for (int i = 0; i < kMaxIter; ++i) {
			Vec<T, 3> pt_in_box(RandomDouble(-1.0, 1.0, seed),
				RandomDouble(-1.0, 1.0, seed),
				RandomDouble(-1.0, 1.0, seed));

			const T pt_sqr = pt_in_box.Sqr();
			if (pt_sqr > kEpsilon && pt_sqr <= 1.0) {
				return pt_in_box.Unit();
			}
		}
		return  Vec<T, 3>(1.0, 0.0, 0.0);
	}

	templat <typename T>
	Vec<T, 3> Vec<T, 3>::UnitFromAngle(T theta, T phi) {
		const T xy_unit_length = std::sin(theta);
		return Vec<T, 3>(
			/*x=*/xy_unit_length * std::cos(phi),
			/*y=*/xy_unit_length * std::sin(phi),
			/*z=*/std::cos(theta)
		);
	}

	templat <typename T>
	Vec<T, 3> Vec<T, 3>::RotatedBy(const Vec<T, 3>& axis, T angle) const {
		const Vec<T, 3> parallel_part = this->Dot(axis) * axis;

		// Compute the rotation in u-v plane.
		const Vec<T, 3> u = *this - parallel_part;
		const Vec<T, 3> v = axis.Cross(u);

		return std::cos(angle) * u + std::sin(angle) * v + parallel_part;
	}

	templat <typename T>
	using Vec1 = Vec<T, 1>;

	templat <typename T>
	using Vec2 = Vec<T, 2>;

	templat <typename T>
	using Vec3 = Vec<T, 3>;

	templat <typename T>
	using Vec4 = Vec<T, 4>;

	using Vec1d = Vec<double, 1>;
	using Vec2d = Vec<double, 2>;
	using Vec3d = Vec<double, 3>;
	using Vec4d = Vec<double, 4>;

}  // namespace geom