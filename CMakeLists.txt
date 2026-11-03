cmake_minimum_required(VERSION 3.21)
project(ThunderheadLightningRod VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_compile_definitions(_WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)

add_subdirectory(src/Thunderhead)
add_subdirectory(src/LightningRod)
