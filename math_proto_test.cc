#include "math.pb.h"
#include "glog/logging.h"
#include "gtest/gtest.h"

namespace demo {
namespace {

class MathProtoTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Initialize Google Logging if not already initialized
    static bool initialized = false;
    if (!initialized) {
      google::InitGoogleLogging("math_proto_test");
      initialized = true;
    }
  }
};

// Test basic protobuf functionality
TEST_F(MathProtoTest, BasicMessage) {
  LOG(INFO) << "Testing protobuf integration";
  
  AddRequest request;
  request.set_a(10);
  request.set_b(20);
  
  EXPECT_EQ(request.a(), 10);
  EXPECT_EQ(request.b(), 20);
  
  AddResponse response;
  response.set_result(request.a() + request.b());
  
  EXPECT_EQ(response.result(), 30);
  LOG(INFO) << "Protobuf test passed: " << request.a() << " + " << request.b() 
            << " = " << response.result();
}

// Test serialization/deserialization
TEST_F(MathProtoTest, Serialization) {
  AddRequest request;
  request.set_a(100);
  request.set_b(200);
  
  // Serialize
  std::string serialized = request.SerializeAsString();
  EXPECT_FALSE(serialized.empty());
  
  // Deserialize
  AddRequest request2;
  EXPECT_TRUE(request2.ParseFromString(serialized));
  EXPECT_EQ(request2.a(), 100);
  EXPECT_EQ(request2.b(), 200);
  
  LOG(INFO) << "Serialization test passed, serialized size: " << serialized.size();
}

}  // namespace
}  // namespace demo
