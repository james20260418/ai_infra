/// @brief A simple Windows console application demonstrating
///        cross-compilation with MinGW.
///
/// Build with:
///   bazel build //demo:windows_helloworld --config=windows
///
/// The resulting .exe can be run on Windows (via Wine or directly).

#include <iostream>
#include <cstdio>

#ifdef __MINGW32__
#define PLATFORM_STRING "MinGW-w64"
#elif defined(_MSC_VER)
#define PLATFORM_STRING "MSVC"
#else
#define PLATFORM_STRING "Native GCC/Clang"
#endif

int main(int argc, char* argv[]) {
  std::cout << "================================" << std::endl;
  std::cout << "  Windows Hello World!" << std::endl;
  std::cout << "================================" << std::endl;
  std::cout << "Platform: " << PLATFORM_STRING << std::endl;
  std::cout << "Architecture: x86_64" << std::endl;
  std::cout << "Built with: Bazel + MinGW" << std::endl;
  std::cout << "================================" << std::endl;

#ifdef _WIN32
  std::cout << "Target: Windows" << std::endl;
#else
  std::cout << "Cross-compiled from Linux" << std::endl;
#endif

  return 0;
}
