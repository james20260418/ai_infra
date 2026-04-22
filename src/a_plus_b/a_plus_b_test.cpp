#include "src/a_plus_b/a_plus_b.h"
#include <cassert>
#include <iostream>

int main() {
    // Test template function
    assert(ai_infra::add(1, 2) == 3);
    assert(ai_infra::add(1.5, 2.5) == 4.0);
    
    // Test int specialization
    assert(ai_infra::add_int(10, 20) == 30);
    assert(ai_infra::add_int(-5, 5) == 0);
    
    // Test float function
    assert(ai_infra::add_float(1.1f, 2.2f) == 3.3f);
    
    // Test double function
    assert(ai_infra::add_double(100.5, 200.5) == 301.0);
    
    std::cout << "All a_plus_b tests passed!" << std::endl;
    return 0;
}