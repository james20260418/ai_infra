#pragma once

#define _USE_MATH_DEFINES

#include <algorithm>

namespace geom {
	constexpr double kTwoPi = 2.0 * M_PI;
	constexpr double kSquareAngleHalfPi = 2.0;
	constexpr double kSquareAnglePi = 4.0;
	constexpr double kSquareAngleTwoPi = 8.0;

	template <typename T>
	constexpr T Sqr(T x) {
		return x * x;
	}

	template <typename T>
	constexpr T Cube(T x) {
		return x * x * x;
	}

	template <typename T, typename R>
	constexpr T Lerp(const T& start, const T& end, R ratio) {
		return start + (end - start) * ratio;
	}

	// Normalize angle to [0.0, 2.0* pi).
	template <typename T>
	T WrapAngle(T angle){
		T residue = fmod(angle, kTwoPi);
		if (residue < 0.0) {
			residue += kTwoPi;
		}
		return residue;
	}

	// Normalize angle to [-pi, pi).
	template <typename T>
	T NormalizeAngle(T angle) {
		T result = WrapAngle<T>(angle);
		if (result >= M_PI) {
			result -= kTwoPi;
		}
		return result;
	}

	// Normalize square angle to [0.0, 8.0).
	template <typename T>
	T WrapSquareAngle(T angle) {
		T residue = fmod(angle, kSquareAngleTwoPi);
		if (residue < 0.0) {
			residue += kSquareAngleTwoPi;
		}
		return residue;
	}

	// Normalize square angle to [-4.0, 4.0).
	template <typename T>
	T NormalizeSquareAngle(T angle) {
		T result = WrapSquareAngle<T>(angle);
		if (result >= kSquareAnglePi) {
			result -= kSquareAngleTwoPi;
		}
		return result;
	}

	// Pre-condition:
	// max(abs(x), abx(y)) > 0.0
	// Fast version of atan2. 
	// Max error is about 0.012 deg. 
	template <typename T>
	T FastAtan2(T y, T x) {
		const T abs_x = std::abs(x);
		const T abs_y = std::abs(y);
		T min_val, max_val;
		const bool x_lt_y = abs_x < abs_y;
		if (x_lt_y) {
			min_val = abs_x;
			max_val = abs_y;
		}
		else {
			min_val = abs_y;
			max_val = abs_x;
		}
		if (max_val == T(0.0)) return 0.0;
		const T a = min_val / max_val;
		const T s = a * a;
		T r =
			((T(-0.0464964749) * s + T(0.15931422)) * s - T(0.327622764)) * s * a + a;
		if (x_lt_y) r = T(M_PI_2) - r;
		if (x < 0) r = T(M_PI) - r;
		if (y < 0) r = -r;
		return r;
	}

	template <typename T, typename R>
	constexpr T LerpAngle(const T& start, const T& end, R ratio) {
		T angle_span = NormalizeAngle(end - start);
		return start + angle_span * ratio;
	}
}  // namespace geom
