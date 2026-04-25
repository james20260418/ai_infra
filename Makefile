# Makefile — convenience wrappers for Bazel builds on ai_infra
#
# Usage:
#   make windows     Cross-compile Windows demo and copy to output/
#   make test        Run all Linux tests
#   make linux       Build all targets for Linux

.PHONY: windows test linux clean

# Windows cross-compile: build and copy .exe to project root's output/
windows:
	bazel build //demo:windows_helloworld.exe --config=windows
	cp -f bazel-bin/demo/windows_helloworld.exe output/windows_helloworld.exe
	@echo "=== Copied to output/windows_helloworld.exe ==="
	file output/windows_helloworld.exe

# Run all tests on Linux
test:
	bazel test //...

# Build everything for Linux
linux:
	bazel build //...

# Clean Bazel cache
clean:
	bazel clean
