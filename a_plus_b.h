#ifndef AI_INFRA_A_PLUS_B_H_
#define AI_INFRA_A_PLUS_B_H_

namespace demo {

// Computes the sum of two integers.
// @param a First integer.
// @param b Second integer.
// @return The sum of a and b.
int APlusB(int a, int b);

// Template version of APlusB that works with any numeric type.
// @param a First value.
// @param b Second value.
// @return The sum of a and b.
template <typename T>
T APlusB(T a, T b) {
  return a + b;
}

}  // namespace demo

#endif  // AI_INFRA_A_PLUS_B_H_
