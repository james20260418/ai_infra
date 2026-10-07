// SizeLimitedPiecewiseLinearFunction — 定长分段线性函数（段数极少时用）
//
// 与 PiecewiseLinearFunction 的区别：
//   - 容量**编译期固定** NumSample（≥ 2），栈上数组、无堆分配；
//   - 求值**不用二分**，用 for 循环线性扫描（段少时更快，且便于双指针遍历）；
//   - 提供 UpProp / DownProp —— 把当前折线当一条**绷紧的线**，用一根「立柱」把它顶起 / 顶下；
//   - 提供带 `int index_hint` 的查询，供**双指针遍历**从一个已知区间起步，避免每次从头扫。
//
// 语义：
//   - x 严格递增；size ∈ [0, NumSample]。
//   - 求值：区间内**线性插值**；两端之外**线性外推**（不外推钳制，见 Evaluate）。
//
// 实现位置：geom/math/ 下，命名空间 geom::math。

#pragma once

#include "geom/common/check.h"

namespace geom {
namespace math {

template <int NumSample>
class SizeLimitedPiecewiseLinearFunction {
  static_assert(NumSample >= 2, "NumSample 至少为 2（至少一条线段）");

 public:
  SizeLimitedPiecewiseLinearFunction() = default;

  int size() const { return size_; }
  bool empty() const { return size_ == 0; }
  // 采样 i 的 x / y（0 <= i < size）。
  double x(int i) const {
    CHECK_GE(i, 0);
    CHECK_LT(i, size_);
    return xs_[i];
  }
  double y(int i) const {
    CHECK_GE(i, 0);
    CHECK_LT(i, size_);
    return ys_[i];
  }

  // 追加一个采样 x（必须严格大于已有最后一个 x）。
  // Pre-condition: size_ < NumSample；size_ == 0 或 x > 最后一个 x。
  void AddSample(double x, double y) {
    CHECK_LT(size_, NumSample) << "容量已满（NumSample=" << NumSample << "）";
    CHECK(size_ == 0 || x > xs_[size_ - 1])
        << "x 必须严格递增：末采样 x=" << (size_ ? xs_[size_ - 1] : 0.0)
        << " 新 x=" << x;
    xs_[size_] = x;
    ys_[size_] = y;
    ++size_;
  }

  void Clear() { size_ = 0; }

  // 求值（线性扫描，从第 0 段起）。x 越界时按首/末段线性外推。
  // Pre-condition: size_ >= 2（否则无区间可查，CHECK 崩溃）。
  double Evaluate(double x) const {
    CHECK_GE(size_, 2) << "至少 2 个采样才有线段";
    const int i = LocateIntervalForward(/*start=*/0, x);
    return InterpOrExtrap(i, x);
  }
  double operator()(double x) const { return Evaluate(x); }

  // 带区间提示的求值：从 *index_hint 所指区间起，用 for 循环向前找 x 所在区间，
  // 找到后写回 *index_hint。用于**双指针遍历**（查询的 x 单调递增时，均摊 O(1)）。
  // *index_hint 为 in/out；进入时越界会被 clamp 到合法区间。
  // 采样外插：按首/末段线性外推（简单）。
  // Pre-condition: size_ >= 2
  double Evaluate(double x, int* index_hint /*inout*/) const {
    CHECK_GE(size_, 2) << "至少 2 个采样才有线段";
    CHECK(index_hint != nullptr);
    int i = *index_hint;
    // clamp 到合法区间下标范围 [0, size_-2]。
    if (i < 0) {
      i = 0;
    } else if (i > size_ - 2) {
      i = size_ - 2;
    }
    i = LocateIntervalForward(i, x);
    // 兜底向后扫（允许 x 非单调，代价小；单调场景不会触发）。
    while (i > 0 && x < xs_[i]) {
      --i;
    }
    *index_hint = i;   // 保证为合法区间下标
    return InterpOrExtrap(i, x);
  }

  // 「向上顶」：想象当前折线是一条绷紧的线，(x0, y0) 是一根立柱把它顶起。
  //   - x0 不在 [xs_[0], xs_[size_-1]] 内 → 无变化；
  //   - x0 落在某线段 (x1, x2) 内：若 y0 > 该段在 x0 处的线性值 → 顶起（插入 x0, y0）；
  //     否则无变化；
  //   - x0 恰与某断点重合：若 y0 > 该断点 y → 抬高该断点，否则无变化。
  // 返回是否发生了变化。容量已满（需插入但无空间）→ 返回 false（不改变）。
  // Pre-condition: size_ >= 2 才有线段（< 2 时直接返回 false，不算错误）。
  bool UpProp(double x0, double y0) {
    if (size_ < 2 || x0 < xs_[0] || x0 > xs_[size_ - 1]) {
      return false;
    }
    int i = LocateIntervalForward(/*start=*/0, x0);
    if (x0 == xs_[i]) {
      if (y0 > ys_[i]) {
        ys_[i] = y0;
        return true;
      }
      return false;
    }
    if (x0 == xs_[i + 1]) {
      if (y0 > ys_[i + 1]) {
        ys_[i + 1] = y0;
        return true;
      }
      return false;
    }
    const double t = (x0 - xs_[i]) / (xs_[i + 1] - xs_[i]);
    const double y_line = ys_[i] + (ys_[i + 1] - ys_[i]) * t;
    if (y0 <= y_line) {
      return false;   // 没顶起来
    }
    return InsertAt(i + 1, x0, y0);
  }

  // 「向下顶」：与 UpProp 对称（立柱把线向下压）。
  bool DownProp(double x0, double y0) {
    if (size_ < 2 || x0 < xs_[0] || x0 > xs_[size_ - 1]) {
      return false;
    }
    int i = LocateIntervalForward(/*start=*/0, x0);
    if (x0 == xs_[i]) {
      if (y0 < ys_[i]) {
        ys_[i] = y0;
        return true;
      }
      return false;
    }
    if (x0 == xs_[i + 1]) {
      if (y0 < ys_[i + 1]) {
        ys_[i + 1] = y0;
        return true;
      }
      return false;
    }
    const double t = (x0 - xs_[i]) / (xs_[i + 1] - xs_[i]);
    const double y_line = ys_[i] + (ys_[i + 1] - ys_[i]) * t;
    if (y0 >= y_line) {
      return false;   // 没压下去
    }
    return InsertAt(i + 1, x0, y0);
  }

 private:
  // 从区间 start 起向前扫，返回使 x 落在 [xs_[i], xs_[i+1]] 的区间下标 i。
  // 返回必为合法区间下标 [0, size_-2]（x 在两端之外时分别返回 0 / size_-2）。
  int LocateIntervalForward(int start, double x) const {
    int i = start;
    for (; i < size_ - 2 && x > xs_[i + 1]; ++i) {
    }
    return i;
  }

  // 在区间 i（[xs_[i], xs_[i+1]]）上求值：线性插值，越界则线性外推。
  double InterpOrExtrap(int i, double x) const {
    const double dx = xs_[i + 1] - xs_[i];
    const double t = (x - xs_[i]) / dx;
    return ys_[i] + (ys_[i + 1] - ys_[i]) * t;
  }

  // 把 (x, y) 插到下标 pos（>= 1）处，右移其后的采样。容量满 → 返回 false。
  bool InsertAt(int pos, double x, double y) {
    if (size_ >= NumSample) {
      return false;
    }
    for (int k = size_; k > pos; --k) {
      xs_[k] = xs_[k - 1];
      ys_[k] = ys_[k - 1];
    }
    xs_[pos] = x;
    ys_[pos] = y;
    ++size_;
    return true;
  }

  int size_ = 0;
  double xs_[NumSample];
  double ys_[NumSample];
};

}  // namespace math
}  // namespace geom
