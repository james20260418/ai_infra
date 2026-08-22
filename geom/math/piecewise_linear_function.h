// PiecewiseLinearFunction — 分段线性函数查询
//
// 用一组单调递增的 x 断点 + 对应 y 值，构建一条分段线性函数。
// 典型用途：用经验照度锚点做查表插值（如太阳直射光强随仰角变化），
// 替代纯解析拟合（Beer-Lambert），更接地气、更好调、物理上界的处理更干净。
//
// 语义：
//   - x 必须严格递增（构造时 CHECK），断点至少 1 个。x 固定为 double。
//   - y 为模板参数 T，支持 double / float / 自定义标量类型等任意数值类型。
//   - operator()(x) 在相邻断点间做线性插值；
//   - x 小于最小断点 / 大于最大断点时，返回对应端点 y（夹断到区间，不外推）。
//
// 复杂度：构造 O(1)；单次查询 O(log n)（std::lower_bound 二分）。
//
// 实现位置：geom/math/ 下，命名空间 geom::math。

#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

#include "geom/common/check.h"

namespace geom {
namespace math {

template <typename T>
class PiecewiseLinearFunction {
 public:
  // 构造：x 与 y 长度一致且至少为 1；x 必须严格递增。
  // Pre-condition: xs.size() == ys.size() 且 >= 1；xs 严格递增。
  PiecewiseLinearFunction(const std::vector<double>& xs,
                          const std::vector<T>& ys) {
    CHECK_EQ(xs.size(), ys.size()) << "x 与 y 长度不一致";
    CHECK_GE(xs.size(), static_cast<size_t>(1)) << "断点至少 1 个";
    for (size_t i = 1; i < xs.size(); ++i) {
      CHECK_GT(xs[i], xs[i - 1]) << "x 必须严格递增，冲突于 [" << i - 1 << ", "
                                 << i << "]: " << xs[i - 1] << " -> " << xs[i];
    }
    xs_ = xs;
    ys_ = ys;
  }

  // 查询 x 处对应的 y。
  // x 落在断点区间内 -> 线性插值；落在两端之外 -> 返回对应端点 y。
  T operator()(double x) const {
    if (x <= xs_.front()) {
      return ys_.front();
    }
    if (x >= xs_.back()) {
      return ys_.back();
    }
    // 找第一个 x 不小于给定值的断点，二分 O(log n)。
    const size_t ub =
        static_cast<size_t>(std::lower_bound(xs_.begin(), xs_.end(), x) -
                            xs_.begin());
    // lower_bound 返回 [1, n-1]（已由上方越界提前 return 保证 ub >= 1），
    // 但为防御未来改动破坏不变量，访问前仍 clamp 一下。
    CHECK_GE(ub, static_cast<size_t>(1));
    CHECK_LT(ub, xs_.size());
    const size_t idx = ub;
    const double x0 = xs_[idx - 1];
    const double x1 = xs_[idx];
    const T y0 = ys_[idx - 1];
    const T y1 = ys_[idx];
    const double t = (x - x0) / (x1 - x0);
    return y0 + (y1 - y0) * t;
  }

  const std::vector<double>& xs() const { return xs_; }
  const std::vector<T>& ys() const { return ys_; }

 private:
  std::vector<double> xs_;
  std::vector<T> ys_;
};

}  // namespace math
}  // namespace geom
