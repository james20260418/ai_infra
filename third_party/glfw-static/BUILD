# GLFW 3.3.10 静态库
#
# 从 https://github.com/glfw/glfw/releases/download/3.3.10/glfw-3.3.10.zip 源码编译
# 编译命令：
#   cmake -S /tmp/glfw-3.3.10 -B /tmp/glfw-build-linux \
#     -DBUILD_SHARED_LIBS=OFF \
#     -DGLFW_BUILD_EXAMPLES=OFF \
#     -DGLFW_BUILD_TESTS=OFF \
#     -DGLFW_BUILD_DOCS=OFF \
#     -DCMAKE_BUILD_TYPE=Release \
#     -DCMAKE_POSITION_INDEPENDENT_CODE=ON
#   cmake --build /tmp/glfw-build-linux

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "glfw",
    srcs = ["libglfw3.a"],
    hdrs = glob(["include/GLFW/*.h"]),
    includes = ["include"],
    linkstatic = 1,
    linkopts = [
        "-lX11",
        "-lXrandr",
        "-lXinerama",
        "-lXcursor",
        "-lXi",
        "-lXrender",
        "-ldl",
        "-lpthread",
    ],
)
