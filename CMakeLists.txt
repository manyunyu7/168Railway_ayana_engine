cmake_minimum_required(VERSION 3.28)
project(engine LANGUAGES CXX C)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
add_compile_options(-Wall -Wextra -Werror -ffp-contract=off)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
enable_testing()

# ---- engine library ----
file(GLOB ENGINE_WORLD_SRC CONFIGURE_DEPENDS engine/world/*.cpp)
add_library(engine STATIC
  ${ENGINE_WORLD_SRC}
  engine/rhi/rhi_gl.cpp
  engine/core/window.cpp
  engine/core/json.cpp
  engine/asset/gltf.cpp
  engine/asset/emod.cpp
  engine/render/model_renderer.cpp
  engine/render/text.cpp
  engine/render/sky.cpp
)
if(NOT EMSCRIPTEN)
  target_sources(engine PRIVATE engine/sim/sim_process.cpp)   # fork/pipes: native only
endif()
# Repo root at compile time: SimProcess finds bridge/sim-bridge.ts and ../ppka-wannabe-2 from it.
target_compile_definitions(engine PUBLIC ENG_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(engine PUBLIC ${CMAKE_SOURCE_DIR})

if(APPLE)
  find_package(glfw3 3.3 REQUIRED)
  find_library(OPENGL_FW OpenGL REQUIRED)
  target_link_libraries(engine PUBLIC glfw ${OPENGL_FW})
  target_compile_definitions(engine PUBLIC ENG_GL_DESKTOP=1 GL_SILENCE_DEPRECATION=1)
elseif(EMSCRIPTEN)
  # WebGL2 via Emscripten's GLES3 emulation; GLFW comes from the Emscripten port
  target_compile_definitions(engine PUBLIC ENG_GL_ES=1)
  target_link_options(engine PUBLIC -sUSE_GLFW=3 -sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2
                      -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=256MB -sSTACK_SIZE=1MB)
elseif(ANDROID)
  target_link_libraries(engine PUBLIC GLESv3 EGL log android)
  target_compile_definitions(engine PUBLIC ENG_GL_ES=1)
endif()

# ---- examples ----
add_executable(cube examples/cube/main.cpp)
target_link_libraries(cube PRIVATE engine)
add_executable(viewer examples/viewer/main.cpp)
target_link_libraries(viewer PRIVATE engine)
if(EMSCRIPTEN)
  # web build: viewer only for now; assets are preloaded into the virtual FS
  set_target_properties(viewer PROPERTIES SUFFIX ".html")
  target_link_options(viewer PRIVATE "SHELL:--preload-file ${CMAKE_SOURCE_DIR}/assets/font.efnt@/assets/font.efnt"
                      "SHELL:--preload-file ${CMAKE_SOURCE_DIR}/build/assets/web@/models" -sEXIT_RUNTIME=0)
  return()
endif()
add_executable(simtest examples/simtest/main.cpp)
target_link_libraries(simtest PRIVATE engine)

# ---- tools (offline; may use third-party decoders) ----
add_executable(convert tools/convert/main.cpp)
target_link_libraries(convert PRIVATE engine)
target_compile_options(convert PRIVATE -Wno-error -O2 -fno-sanitize=undefined)
add_executable(fontgen tools/fontgen/main.cpp)
target_include_directories(fontgen PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_options(fontgen PRIVATE -Wno-error -O2 -fno-sanitize=undefined)

# ---- per-directory targets (each dir may carry its own CMakeLists.txt) ----
file(GLOB SUB_CMAKE CONFIGURE_DEPENDS examples/*/CMakeLists.txt tools/*/CMakeLists.txt)
foreach(f ${SUB_CMAKE})
  get_filename_component(d ${f} DIRECTORY)
  add_subdirectory(${d})
endforeach()

# ---- tests (ctest; see tests/CMakeLists.txt) ----
add_subdirectory(tests)
