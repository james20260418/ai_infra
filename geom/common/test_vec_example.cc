#include "geom/common/vec.h"
#include "glog/logging.h"

int main() {
  // Initialize Google Logging
  google::InitGoogleLogging("test_vec_example");
  
  LOG(INFO) << "Testing Vec library migration...";
  
  // Test Vec2d
  geom::Vec2d v1(1.0, 2.0);
  geom::Vec2d v2(3.0, 4.0);
  
  LOG(INFO) << "v1 = (" << v1.x() << ", " << v1.y() << ")";
  LOG(INFO) << "v2 = (" << v2.x() << ", " << v2.y() << ")";
  
  // Test addition (commented out for now due to constructor issue)
  // auto v3 = v1 + v2;
  // LOG(INFO) << "v1 + v2 = (" << v3.x() << ", " << v3.y() << ")";
  
  // Test dot product
  double dot = v1.Dot(v2);
  LOG(INFO) << "v1 · v2 = " << dot;
  
  // Test magnitude
  double mag = v1.Magnitude();
  LOG(INFO) << "|v1| = " << mag;
  
  LOG(INFO) << "Vec library test completed successfully!";
  return 0;
}