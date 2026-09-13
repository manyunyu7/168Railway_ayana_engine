# mesin — engine 3D sendiri untuk PPKA

C++20, tanpa engine pihak ketiga. Pustaka luar hanya GLFW (jendela/input, desktop) dan GPU API (OpenGL).
Aset & data peta diambil dari `../ppka-wannabe-2` (tidak disalin).

```bash
cmake --preset mac-debug && cmake --build --preset mac-debug
./build/mac-debug/kubus                 # contoh 1: kubus berputar (drag = putar, scroll = zoom)
MESIN_TANGKAP=/tmp/a.ppm ./build/mac-debug/kubus   # tangkap frame ke-30 lalu keluar
```

Tata letak: `mesin/matek` (math), `mesin/rhi` (lapisan GPU), `mesin/inti` (jendela, kamera), `contoh/`.
