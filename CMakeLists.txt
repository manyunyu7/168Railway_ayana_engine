cmake_minimum_required(VERSION 3.28)
project(engine LANGUAGES CXX C)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
add_compile_options(-Wall -Wextra -Werror -ffp-contract=off)

# ---- engine library ----
add_library(engine STATIC
  engine/rhi/rhi_gl.cpp
  engine/core/window.cpp
)
target_include_directories(engine PUBLIC ${CMAKE_SOURCE_DIR})

if(APPLE)
  find_package(glfw3 3.3 REQUIRED)
  find_library(OPENGL_FW OpenGL REQUIRED)
  target_link_libraries(engine PUBLIC glfw ${OPENGL_FW})
  target_compile_definitions(engine PUBLIC ENG_GL_DESKTOP=1 GL_SILENCE_DEPRECATION=1)
elseif(ANDROID)
  target_link_libraries(engine PUBLIC GLESv3 EGL log android)
  target_compile_definitions(engine PUBLIC ENG_GL_ES=1)
endif()

# ---- examples ----
add_executable(cube examples/cube/main.cpp)
target_link_libraries(cube PRIVATE engine)
