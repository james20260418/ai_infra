#pragma once

#include "geom/common/vec.h"

namespace geom {
	template <typename T>
	struct Segment2 {
	public:
		Segment2() = default; 

		Segment2(const Vec2<T>& start, const Vec2<T>& end) :
			start_(start), unit_(end - start) {
			length_ = unit_.Norm();

			DCHECK_GE(length_, 0.0);
			DCHECK(!ValueNear(length_, 0.0)) << "Segment2: When constructing using 2 points, they must be disjoint.";

			unit_ *= 1.0 / length_;
		}

		Segment2(const Vec2<T>& start, const Vec2<T>& unit, T length) :
			start_(start), unit_(unit), length_(length) {
			DCHECK_GE(length_, 0.0);
		}


		T ProductOntoUnit(const Vec2<T>& point) const;

		T ProjectOntoUnit(const Vec2<T>& point) const;

		T length() const {
			return length_;
		}

		void set_length(T length) {
			DCHECK_GE(length_, 0.0);
			length_ = length;
		}

		bool IsPoint() const {
			return ValueNear(length_, 0.0);
		}

		const Vec2<T>& start() const {
			return start_;
		}

		Vec2<T> end() const {
			return start_ + unit_ * length_;
		}

		const Vec2<T>& unit() const {
			return unit_;
		}

		// TODO(huaiyuan): Shift, Rotate, Scale.
		void ShiftBy(const Vec2<T>& offset) {
			start_ += offset;
		}

		void RotateByUnit(const Vec2d& center, const Vec2d& unit) {
			start_ = center + (start_ - center).RotatedByUnit(unit);
			unit_.RotateByUnit(unit);
		}

		bool IsPointOn(const Vec2<T>& point, T epsilon = kEpsilon) const {
			const T proj = ProjectOntoUnit(point);
			if (proj < -epsilon || proj > length_ + epsilon) {
				return false;
			}
			return ValueNear(ProductOntoUnit(point), 0.0, epsilon);
		}

		std::string DebugString() const {
			const char* type_name = typeid(T).name();
			return StrFormat("Segment2<%s>{%s,%s,%f}",
				type_name, 
				start_.DebugString().c_str(),
				unit_.DebugString().c_str(),
				length_);
		}

		T DistanceSquareTo(const Vec2<T>& point) const;

		T DistanceTo(const Vec2<T>& point) const {
			return std::sqrt(DistanceSquareTo(point));
		}

	private:
		T length_ = 0.0;
		Vec2<T> start_;
		Vec2<T> unit_;
	};


	template <typename T>
	T Segment2<T>::ProductOntoUnit(const Vec2<T>& point) const {
		return unit_.Cross(point - start_);
	}

	template <typename T>
	T Segment2<T>::ProjectOntoUnit(const Vec2<T>& point) const {
		return unit_.Dot(point - start_);
	}

	template <typename T>
	T Segment2<T>::DistanceSquareTo(const Vec2<T>& point) const {
		const T s = ProjectOntoUnit(point);
		if (s < 0.0) {
			return (point - start()).Sqr();
		} else if (s > length_) {
			return (point - end()).Sqr();
		} else {
			T l = ProductOntoUnit(point);
			return Sqr(l);
		}
	}


	using Segment2d = Segment2<double>;
}  // namespace geom