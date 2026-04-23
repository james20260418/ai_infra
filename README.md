# AI Infrastructure Project

A Bazel-based C++ project for learning and practicing AI infrastructure development.

## Project Structure

```
ai_infra/
├── BUILD              # Bazel build configuration
├── WORKSPACE          # Bazel workspace definition
├── .bazelrc           # Bazel compiler flags and settings
├── a_plus_b.h         # a+b function header
├── a_plus_b.cc        # a+b function implementation
├── a_plus_b_test.cc   # a+b tests with GoogleTest
├── math.proto         # Protobuf message definitions
├── math_proto_test.cc # Protobuf tests
└── third_party/       # External dependencies
    ├── glog-static/   # Glog static library
    └── protobuf-3.14.0-rc-3/  # Protobuf source
```

## Build & Test

```bash
# Build a_plus_b library
bazel build //:a_plus_b

# Run tests
bazel test //:a_plus_b_test
bazel test //:math_proto_test
```

## Features

- C++20 with Bazel build system
- GoogleTest for unit testing
- Glog for logging
- Protobuf for serialization

## a_plus_b Library

### Overview
The `a_plus_b` library provides a simple yet powerful addition function that supports multiple numeric types through function overloading.

### Design Philosophy
1. **Simplicity**: Single-purpose function with clear semantics
2. **Type Safety**: Compile-time type checking through function overloading
3. **Extensibility**: Easy to add support for new numeric types
4. **Testability**: Comprehensive unit test coverage for all supported types

### API Reference

#### Header: `a_plus_b.h`
```cpp
// Integer addition
int APlusB(int a, int b);

// Float addition
float APlusB(float a, float b);

// Double addition
double APlusB(double a, double b);
```

#### Implementation: `a_plus_b.cc`
- **Integer version**: Direct arithmetic addition
- **Float version**: Single-precision floating-point addition
- **Double version**: Double-precision floating-point addition

### Usage Example
```cpp
#include "a_plus_b.h"

int main() {
    // Integer addition
    int int_result = APlusB(3, 4);  // Returns 7
    
    // Float addition
    float float_result = APlusB(3.14f, 2.71f);  // Returns 5.85f
    
    // Double addition
    double double_result = APlusB(3.1415926535, 2.7182818284);  // Returns 5.8598744819
    
    return 0;
}
```

### Testing Strategy

#### Test File: `a_plus_b_test.cc`
The library includes comprehensive unit tests using GoogleTest framework:

1. **IntegerAddition Test**:
   - Tests basic integer arithmetic
   - Edge cases: zero, negative numbers, large values

2. **FloatAddition Test**:
   - Tests single-precision floating-point addition
   - Uses `EXPECT_FLOAT_EQ` for approximate equality comparison
   - Tests with fractional values

3. **DoubleAddition Test**:
   - Tests double-precision floating-point addition
   - Uses `EXPECT_DOUBLE_EQ` for high-precision comparison
   - Tests with high-precision constants

#### Running Tests
```bash
# Run all a_plus_b tests
bazel test //:a_plus_b_test

# Run specific test
bazel test //:a_plus_b_test --test_filter=IntegerAddition
```

### Build Integration

The library is integrated into the Bazel build system:

```python
# BUILD file configuration
cc_library(
    name = "a_plus_b",
    srcs = ["a_plus_b.cc"],
    hdrs = ["a_plus_b.h"],
    visibility = ["//visibility:public"],
)

cc_test(
    name = "a_plus_b_test",
    srcs = ["a_plus_b_test.cc"],
    deps = [
        ":a_plus_b",
        "@com_google_googletest//:gtest_main",
    ],
)
```

### Design Decisions

1. **Function Overloading vs Templates**:
   - Chose function overloading for explicit type signatures
   - Provides clearer API documentation
   - Easier to debug and understand

2. **Separate Implementation Files**:
   - Header (`a_plus_b.h`) declares the interface
   - Source (`a_plus_b.cc`) implements the logic
   - Follows C++ best practices for separation of concerns

3. **Testing Framework**:
   - GoogleTest for industry-standard unit testing
   - Separate test cases for each numeric type
   - Appropriate equality comparisons for each type

### Future Extensions

Potential enhancements for the library:
1. Support for complex numbers
2. Vectorized versions using SIMD instructions
3. Template version for generic numeric types
4. Error handling for overflow/underflow conditions

### Dependencies
- **Build System**: Bazel 5.4.0+
- **Testing**: GoogleTest
- **Compiler**: C++20 compatible compiler (GCC/Clang)

### Contributing

When adding support for new numeric types:
1. Add function declaration to `a_plus_b.h`
2. Implement the function in `a_plus_b.cc`
3. Add corresponding test cases to `a_plus_b_test.cc`
4. Update this documentation
5. Ensure all tests pass before submitting a PR
