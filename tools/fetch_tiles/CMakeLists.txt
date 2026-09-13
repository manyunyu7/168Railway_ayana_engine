add_executable(fetch_tiles main.cpp)
target_link_libraries(fetch_tiles PRIVATE engine)
target_compile_options(fetch_tiles PRIVATE -Wno-error -O2 -fno-sanitize=undefined)
