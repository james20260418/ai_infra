// GLFW 集成测试
//
// 验证 GLFW 能在 Linux 下正常编译链接，创建窗口并轮询事件。
// 输出 GLFW 版本信息到日志。

#include <GLFW/glfw3.h>
#include <cstdio>

int main() {
    // 初始化 GLFW
    if (!glfwInit()) {
        fprintf(stderr, "FAILED: glfwInit()\n");
        return 1;
    }

    // 输出 GLFW 版本
    int major, minor, rev;
    glfwGetVersion(&major, &minor, &rev);
    fprintf(stdout, "OK: GLFW %d.%d.%d initialized\n", major, minor, rev);

    // 创建隐藏窗口（不显示，仅验证创建逻辑）
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    GLFWwindow* window = glfwCreateWindow(640, 480, "GLFW Test", nullptr, nullptr);
    if (!window) {
        fprintf(stderr, "FAILED: glfwCreateWindow()\n");
        glfwTerminate();
        return 1;
    }
    fprintf(stdout, "OK: Window created (640x480, hidden)\n");

    // 验证 glfwWindowShouldClose 返回 false（刚创建的窗口）
    if (glfwWindowShouldClose(window)) {
        fprintf(stderr, "FAILED: glfwWindowShouldClose() returned true on fresh window\n");
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }
    fprintf(stdout, "OK: glfwWindowShouldClose() returns false\n");

    // 验证窗口尺寸
    int w, h;
    glfwGetWindowSize(window, &w, &h);
    fprintf(stdout, "OK: Window size = %dx%d\n", w, h);

    // 轮询事件（无头模式下不阻塞）
    glfwPollEvents();

    fprintf(stdout, "OK: glfwPollEvents() completed\n");

    // 清理
    glfwDestroyWindow(window);
    glfwTerminate();
    fprintf(stdout, "OK: All GLFW tests passed\n");
    return 0;
}
