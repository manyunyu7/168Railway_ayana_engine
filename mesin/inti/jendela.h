// Jendela + input desktop (GLFW). Di Android diganti oleh Surface dari Flutter.
#pragma once
#include <functional>

namespace mesin {

struct Jendela {
  bool buka(int lebar, int tinggi, const char* judul);
  void tutup();
  bool masihBuka() const;
  void tukarBuffer();
  void ambilPeristiwa();
  double waktu() const;            // detik sejak mulai
  void ukuranFramebuffer(int& w, int& h) const;

  // input sederhana (mouse)
  bool tombolMouse(int n) const;
  void posisiMouse(double& x, double& y) const;
  double gulir = 0;                // akumulasi scroll, di-reset oleh pemakai
  bool tombol(int kunciGlfw) const;

  void* pegangan = nullptr;        // GLFWwindow*
};

} // namespace mesin
