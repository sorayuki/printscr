cmake_minimum_required(VERSION 3.20)
project(printscr)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(DEPS_DIR "${CMAKE_SOURCE_DIR}/deps")

include_directories(${DEPS_DIR}/include)

add_executable(printscr main.cpp ScreenCapture.cpp SystemInfo.cpp PreviewModule.cpp)

# Link Libraries
target_link_libraries(printscr PRIVATE
    d3d11
    dxgi
    dwmapi
    windowsapp
    
    $<$<CONFIG:Debug>:${DEPS_DIR}/debug/lib/libEGL.lib>
    $<$<CONFIG:Debug>:${DEPS_DIR}/debug/lib/libGLESv2.lib>
    $<$<CONFIG:Debug>:${DEPS_DIR}/debug/lib/zlibd.lib>
    $<$<NOT:$<CONFIG:Debug>>:${DEPS_DIR}/lib/libEGL.lib>
    $<$<NOT:$<CONFIG:Debug>>:${DEPS_DIR}/lib/libGLESv2.lib>
    $<$<NOT:$<CONFIG:Debug>>:${DEPS_DIR}/lib/zlib.lib>
)

# Copy DLLs
add_custom_command(TARGET printscr POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    $<$<CONFIG:Debug>:${DEPS_DIR}/debug/bin/libEGL.dll>
    $<$<CONFIG:Debug>:${DEPS_DIR}/debug/bin/libGLESv2.dll>
    $<$<CONFIG:Debug>:${DEPS_DIR}/debug/bin/zlibd1.dll>
    $<$<NOT:$<CONFIG:Debug>>:${DEPS_DIR}/bin/libEGL.dll>
    $<$<NOT:$<CONFIG:Debug>>:${DEPS_DIR}/bin/libGLESv2.dll>
    $<$<NOT:$<CONFIG:Debug>>:${DEPS_DIR}/bin/zlib1.dll>
    $<TARGET_FILE_DIR:printscr>
)
