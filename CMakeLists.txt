cmake_minimum_required(VERSION 3.28)
project(mesin LANGUAGES CXX C)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
add_compile_options(-Wall -Wextra -Werror -ffp-contract=off)

# ---- pustaka engine ("mesin") ----
add_library(mesin STATIC
  mesin/rhi/rhi_gl.cpp
  mesin/inti/jendela.cpp
)
target_include_directories(mesin PUBLIC ${CMAKE_SOURCE_DIR})

if(APPLE)
  find_package(glfw3 3.3 REQUIRED)
  find_library(OPENGL_FW OpenGL REQUIRED)
  target_link_libraries(mesin PUBLIC glfw ${OPENGL_FW})
  target_compile_definitions(mesin PUBLIC MESIN_GL_DESKTOP=1 GL_SILENCE_DEPRECATION=1)
elseif(ANDROID)
  target_link_libraries(mesin PUBLIC GLESv3 EGL log android)
  target_compile_definitions(mesin PUBLIC MESIN_GL_ES=1)
endif()

# ---- contoh ----
add_executable(kubus contoh/kubus/main.cpp)
target_link_libraries(kubus PRIVATE mesin)
