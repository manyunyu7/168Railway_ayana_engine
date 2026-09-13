#include "mesin/inti/jendela.h"
#include <GLFW/glfw3.h>
#include <cstdio>

namespace mesin {

static GLFWwindow* W(const Jendela& j) { return static_cast<GLFWwindow*>(j.pegangan); }

bool Jendela::buka(int lebar, int tinggi, const char* judul) {
  if (!glfwInit()) { std::fprintf(stderr, "glfwInit gagal\n"); return false; }
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
  glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
  glfwWindowHint(GLFW_SAMPLES, 4);
  GLFWwindow* w = glfwCreateWindow(lebar, tinggi, judul, nullptr, nullptr);
  if (!w) { std::fprintf(stderr, "glfwCreateWindow gagal\n"); glfwTerminate(); return false; }
  pegangan = w;
  glfwSetWindowUserPointer(w, this);
  glfwMakeContextCurrent(w);
  glfwSwapInterval(1);
  glfwSetScrollCallback(w, [](GLFWwindow* g, double, double dy) {
    static_cast<Jendela*>(glfwGetWindowUserPointer(g))->gulir += dy;
  });
  return true;
}
void Jendela::tutup() { if (pegangan) { glfwDestroyWindow(W(*this)); glfwTerminate(); pegangan = nullptr; } }
bool Jendela::masihBuka() const { return pegangan && !glfwWindowShouldClose(W(*this)); }
void Jendela::tukarBuffer() { glfwSwapBuffers(W(*this)); }
void Jendela::ambilPeristiwa() { glfwPollEvents(); }
double Jendela::waktu() const { return glfwGetTime(); }
void Jendela::ukuranFramebuffer(int& w, int& h) const { glfwGetFramebufferSize(W(*this), &w, &h); }
bool Jendela::tombolMouse(int n) const { return glfwGetMouseButton(W(*this), n) == GLFW_PRESS; }
void Jendela::posisiMouse(double& x, double& y) const { glfwGetCursorPos(W(*this), &x, &y); }
bool Jendela::tombol(int k) const { return glfwGetKey(W(*this), k) == GLFW_PRESS; }

} // namespace mesin
