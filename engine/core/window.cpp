#include "engine/core/window.h"
#include <GLFW/glfw3.h>
#include <cstdio>

namespace eng {

static GLFWwindow* W(const Window& w) { return static_cast<GLFWwindow*>(w.handle); }

bool Window::open(int width, int height, const char* title) {
  if (!glfwInit()) { std::fprintf(stderr, "glfwInit failed\n"); return false; }
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
  glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
  glfwWindowHint(GLFW_SAMPLES, 4);
  GLFWwindow* w = glfwCreateWindow(width, height, title, nullptr, nullptr);
  if (!w) { std::fprintf(stderr, "glfwCreateWindow failed\n"); glfwTerminate(); return false; }
  handle = w;
  glfwSetWindowUserPointer(w, this);
  glfwMakeContextCurrent(w);
  glfwSwapInterval(1);
  glfwSetScrollCallback(w, [](GLFWwindow* g, double, double dy) {
    static_cast<Window*>(glfwGetWindowUserPointer(g))->scroll += dy;
  });
  return true;
}
void Window::close() { if (handle) { glfwDestroyWindow(W(*this)); glfwTerminate(); handle = nullptr; } }
bool Window::isOpen() const { return handle && !glfwWindowShouldClose(W(*this)); }
void Window::swapBuffers() { glfwSwapBuffers(W(*this)); }
void Window::pollEvents() { glfwPollEvents(); }
double Window::time() const { return glfwGetTime(); }
void Window::framebufferSize(int& w, int& h) const { glfwGetFramebufferSize(W(*this), &w, &h); }
bool Window::mouseButton(int n) const { return glfwGetMouseButton(W(*this), n) == GLFW_PRESS; }
void Window::mousePos(double& x, double& y) const { glfwGetCursorPos(W(*this), &x, &y); }
bool Window::key(int k) const { return glfwGetKey(W(*this), k) == GLFW_PRESS; }

} // namespace eng
