# Engine test suite (ctest). Each test is a small executable using tests/check.h (no framework).
# Exit codes: 0 pass, 1 fail, 77 skipped (SKIP_RETURN_CODE). GPU tests carry the label `gpu` and are
# excluded by the `mac-debug` preset (`ctest --preset mac-debug-gpu` runs them alone).

function(engine_test name)
  add_executable(${name} ${name}.cpp ${ARGN})
  target_link_libraries(${name} PRIVATE engine)
  add_test(NAME ${name} COMMAND ${name} WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  set_tests_properties(${name} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
endfunction()

engine_test(test_math)
engine_test(test_json)
engine_test(test_gltf_emod)
engine_test(test_track)
engine_test(test_compass ${CMAKE_SOURCE_DIR}/examples/ppka/compass.cpp)
engine_test(test_profile)
engine_test(test_bridge)
set_tests_properties(test_bridge PROPERTIES LABELS "node")

# Golden image (needs a window/GPU): runs the viewer with ENG_CAPTURE and compares.
engine_test(test_golden)
set_tests_properties(test_golden PROPERTIES LABELS "gpu" DEPENDS viewer)
target_compile_definitions(test_golden PRIVATE ENG_BINARY_DIR="${CMAKE_BINARY_DIR}")
