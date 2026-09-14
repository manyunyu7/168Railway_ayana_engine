add_executable(imgconv main.cpp)
target_link_libraries(imgconv PRIVATE engine)
target_compile_options(imgconv PRIVATE -Wno-error -O2 -fno-sanitize=undefined)
