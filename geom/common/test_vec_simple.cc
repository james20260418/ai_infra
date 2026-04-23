#include "geom/common/vec.h"
#include "glog/logging.h"
#include <iostream>

int main() {
  // Initialize Google Logging
  google::InitGoogleLogging("test_vec_simple");
  
  std::cout << "Testing Vec library migration (simple test)..." << std::endl;
  
  try {
    // Test Vec2d construction
    geom::Vec2d v1(1.0, 2.0);
    std::cout << "Successfully created Vec2d(1.0, 2.0)" << std::endl;
    std::cout << "v1.x() = " << v1.x() << std::endl;
    std::cout << "v1.y() = " << v1.y() << std::endl;
    
    // Test Vec2d with two doubles
    geom::Vec2d v2(3.0, 4.0);
    std::cout << "Successfully created Vec2d(3.0, 4.0)" << std::endl;
    
    // Test accessors
    std::cout << "v2[0] = " << v2[0] << std::endl;
    std::cout << "v2[1] = " << v2[1] << std::endl;
    
    std::cout << "Vec library basic test completed successfully!" << std::endl;
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << std::endl;
    return 1;
  }
}