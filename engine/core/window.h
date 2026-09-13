// Desktop window + input (GLFW). On Android this is replaced by a Surface from Flutter.
#pragma once

namespace eng {

struct Window {
  bool open(int width, int height, const char* title);
  void close();
  bool isOpen() const;
  void swapBuffers();
  void pollEvents();
  double time() const;             // seconds since start
  void framebufferSize(int& w, int& h) const;

  bool mouseButton(int n) const;
  void mousePos(double& x, double& y) const;
  double scroll = 0;               // accumulated scroll; caller resets
  bool key(int glfwKey) const;

  void* handle = nullptr;          // GLFWwindow*
};

} // namespace eng
