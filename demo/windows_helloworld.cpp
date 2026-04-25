/// @brief A simple Windows console application demonstrating
///        cross-compilation with MinGW and glog logging.
///
/// Build with:
///   bazel build //demo:windows_helloworld.exe --config=windows
///
/// The resulting .exe can be run on Windows (via Wine or directly).

#include <iostream>

#ifdef _WIN32
// Must define this before including glog headers to avoid
// conflict between Windows ERROR macro and glog::ERROR enum
#ifndef GLOG_NO_ABBREVIATED_SEVERITIES
#define GLOG_NO_ABBREVIATED_SEVERITIES
#endif
#endif

#include <glog/logging.h>

#ifdef __MINGW32__
#define PLATFORM_STRING "MinGW-w64"
#elif defined(_MSC_VER)
#define PLATFORM_STRING "MSVC"
#else
#define PLATFORM_STRING "Native GCC/Clang"
#endif

int main(int argc, char* argv[]) {
  // Initialize Google's logging library.
  google::InitGoogleLogging(argv[0]);

  // Force logging to stderr (equivalent to --logtostderr=1)
  FLAGS_logtostderr = true;
  FLAGS_minloglevel = 0;  // Show all log levels

  LOG(INFO) << "Windows Hello World!";
  LOG(INFO) << "Platform: " << PLATFORM_STRING;

#ifdef _WIN32
  LOG(INFO) << "Running on Windows!";
#else
  LOG(INFO) << "Cross-compiled from Linux for Windows target";
#endif

  LOG(INFO) << "Architecture: x86_64";
  LOG(WARNING) << "This binary was cross-compiled using Bazel + MinGW";
  LOG(ERROR) << "This is an example error message (not a real error)";

  std::cout << std::endl;
  std::cout << "Hello, World from Windows!" << std::endl;
  std::cout << "Platform: " << PLATFORM_STRING << std::endl;
  std::cout << "Built with: Bazel + MinGW" << std::endl;

  LOG(INFO) << "Exiting successfully.";

  google::ShutdownGoogleLogging();

  std::cout << std::endl;
  std::cout << "Press any key to exit..." << std::endl;
#ifdef _WIN32
  system("pause > nul");
#else
  std::cin.get();
#endif

  return 0;
}
