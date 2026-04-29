// GLFW 点击响应演示
//
// 创建一个可见窗口，响应鼠标左键点击（按下并释放），
// 每次点击在终端打印一个字符 '.'，按 Esc 或点击关闭按钮退出。
//
// 可执行文件复制到 WSL2 中运行时，窗口会弹出到 Windows 桌面。

#include <GLFW/glfw3.h>
#include <cstdio>

// Pre-condition: callback is never null (GLFW owns the registration)
static void OnMouseClick(GLFWwindow* window, int button, int action, int mods) {
  (void)mods;
  if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_RELEASE) {
    fprintf(stdout, ".");
    fflush(stdout);
  }
}

int main() {
  if (!glfwInit()) {
    fprintf(stderr, "FAILED: glfwInit()\n");
    return 1;
  }

  GLFWwindow* window = glfwCreateWindow(800, 600, "JPOV Click Demo", nullptr, nullptr);
  if (!window) {
    fprintf(stderr, "FAILED: glfwCreateWindow()\n");
    glfwTerminate();
    return 1;
  }

  glfwMakeContextCurrent(window);
  glfwSetMouseButtonCallback(window, OnMouseClick);

  fprintf(stdout, "Click in the window (or press Esc to exit)\n");
  fflush(stdout);

  // 主循环 — 每帧清除背景并轮询事件
  while (!glfwWindowShouldClose(window)) {
    // 清成深蓝色背景
    glClearColor(0.1f, 0.1f, 0.3f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glfwSwapBuffers(window);
    glfwPollEvents();

    // 按 Esc 退出
    if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
      glfwSetWindowShouldClose(window, GLFW_TRUE);
    }
  }

  glfwDestroyWindow(window);
  glfwTerminate();
  fprintf(stdout, "\nDone.\n");
  return 0;
}
