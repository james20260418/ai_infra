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
