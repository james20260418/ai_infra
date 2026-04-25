#pragma once

#include "geom/common/check.h"

namespace geom {

template<typename T>
struct AABox2 {
 public:
  AABox2(T x_min, T x_max, T y_min, T y_max)
      : x_min_(x_min), x_max_(x_max), y_min_(y_min), y_max_(y_max) {
    DCHECK_GE(x_max_, x_min_);
    DCHECK_GE(y_max_, y_min_);
  }

  AABox2() = default;

  inline T x_min() const { return x_min_; }
  inline T x_max() const { return x_max_; }
  inline T XSpan() const { return x_max_ - x_min_; }

  inline T y_min() const { return y_min_; }
  inline T y_max() const { return y_max_; }
  inline T YSpan() const { return y_max_ - y_min_; }

  T x_min_ = 0.0;
  T x_max_ = 0.0;
  T y_min_ = 0.0;
  T y_max_ = 0.0;
};

using AABox2d = AABox2<double>;

}  // namespace geom
