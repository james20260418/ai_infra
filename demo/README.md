# Windows Demo — Cross-compilation Guide

This directory contains a Windows Hello World demo that demonstrates
how to cross-compile C++ code from Linux to Windows using MinGW.

## Prerequisites

Install the MinGW cross-compiler on your development machine:

```bash
# Ubuntu/Debian
sudo apt-get install g++-mingw-w64-x86-64

# macOS (with Homebrew)
brew install mingw-w64
```

_Tip: GitHub Actions runners also support MinGW — just add the install
command in your workflow._

## Build

```bash
# Cross-compile for Windows x86_64
bazel build //demo:windows_helloworld.exe --config=windows

# The output is a Windows PE executable
file bazel-bin/demo/windows_helloworld.exe
# → PE32+ executable (console) x86-64, for MS Windows
```

## Run

The `.exe` file cannot run directly on Linux. Use one of these methods:

### On actual Windows
Copy the `.exe` to a Windows machine and run it:

```bash
cp bazel-bin/demo/windows_helloworld.exe output/
# → then copy output/windows_helloworld.exe to Windows
```

### Using Wine (on Linux)
```bash
sudo apt-get install wine
wine bazel-bin/demo/windows_helloworld.exe
```

## Output

The program displays platform info and log messages via glog:

```
Windows Hello World!
Platform: MinGW-w64
Architecture: x86_64
Built with: Bazel + MinGW
```

## How It Works

The project uses Bazel's platform & toolchain system for cross-compilation:

- **Platform**: `//platforms:windows_x86_64` defines the Windows target
- **Toolchain**: `//toolchains:mingw_windows_toolchain` wraps the MinGW
  cross-compiler (`x86_64-w64-mingw32-g++`)
- **Config**: `--config=windows` in `.bazelrc` activates both

The `cc_binary` target is named with `.exe` suffix so Bazel produces a
Windows PE executable. Static libraries (glog) are selected per-platform
using `select()` in their BUILD files.
