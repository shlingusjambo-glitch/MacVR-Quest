cmake_minimum_required(VERSION 3.22.1)
project(vr4mac LANGUAGES CXX)
find_package(OpenXR REQUIRED CONFIG)
add_library(vr4mac SHARED main.cpp)
target_compile_definitions(vr4mac PRIVATE XR_USE_PLATFORM_ANDROID XR_USE_GRAPHICS_API_OPENGL_ES)
target_link_libraries(vr4mac PRIVATE OpenXR::openxr_loader android log EGL GLESv3)

target_include_directories(vr4mac PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../common")
