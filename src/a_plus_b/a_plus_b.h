#ifndef AI_INFRA_A_PLUS_B_H
#define AI_INFRA_A_PLUS_B_H

namespace ai_infra {

/**
 * @brief Adds two integers
 * 
 * This is a simple template function that adds two integers.
 * It serves as a basic example for the project structure.
 * 
 * @param a First integer
 * @param b Second integer
 * @return Sum of a and b
 */
template<typename T>
T add(T a, T b) {
    return a + b;
}

/**
 * @brief Adds two integers (int specialization)
 * 
 * @param a First integer
 * @param b Second integer
 * @return Sum of a and b as int
 */
int add_int(int a, int b);

/**
 * @brief Adds two floating-point numbers
 * 
 * @param a First float
 * @param b Second float
 * @return Sum of a and b as float
 */
float add_float(float a, float b);

/**
 * @brief Adds two double-precision numbers
 * 
 * @param a First double
 * @param b Second double
 * @return Sum of a and b as double
 */
double add_double(double a, double b);

} // namespace ai_infra

#endif // AI_INFRA_A_PLUS_B_H