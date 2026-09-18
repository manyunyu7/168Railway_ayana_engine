# Gradle's entry into the engine: the repo root builds target `ayana` (libayana.so) when ANDROID is set,
# which AGP then packs into the APK. Nothing Android-specific lives here on purpose — the engine's own
# CMakeLists.txt is the single description of what the library contains.
cmake_minimum_required(VERSION 3.22)
project(ayana_plugin LANGUAGES CXX C)
get_filename_component(AYANA_ENGINE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../.. ABSOLUTE)
add_subdirectory(${AYANA_ENGINE_ROOT} ${CMAKE_CURRENT_BINARY_DIR}/engine)
