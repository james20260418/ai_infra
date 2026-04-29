# GLFW 3.3.10 静态库（MinGW 交叉编译版）
#
# 编译命令：
#   cmake -S /tmp/glfw-3.3.10 -B /tmp/glfw-build-mingw \
#     -DCMAKE_SYSTEM_NAME=Windows \
#     -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc \
#     -DBUILD_SHARED_LIBS=OFF \
#     -DGLFW_BUILD_EXAMPLES=OFF \
#     -DGLFW_BUILD_TESTS=OFF \
#     -DGLFW_BUILD_DOCS=OFF \
#     -DCMAKE_BUILD_TYPE=Release
#   cmake --build /tmp/glfw-build-mingw

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "glfw",
    srcs = ["libglfw3.a"],
    hdrs = glob(["include/GLFW/*.h"]),
    includes = ["include"],
    linkstatic = 1,
    linkopts = [
        "-lgdi32",
        "-luser32",
        "-lkernel32",
        "-lws2_32",
        "-lopengl32",
    ],
)
