cmake_minimum_required(VERSION 3.20)
project(BatteryMonitor LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 强制静态 C++ 运行库 & 字符集设置
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_definitions(-DUNICODE -D_UNICODE)
add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")

# 1. 自动寻找头文件
file(GLOB_RECURSE WEBVIEW2_HEADERS "${CMAKE_SOURCE_DIR}/*WebView2.h")
if(NOT WEBVIEW2_HEADERS)
    message(FATAL_ERROR "❌ 找不到 WebView2.h")
endif()
list(GET WEBVIEW2_HEADERS 0 FIRST_HEADER)
get_filename_component(INCLUDE_DIR "${FIRST_HEADER}" DIRECTORY)

# 👈 全局引入这两个路径，以后头文件里直接写 #include "IDevice.h" 即可
include_directories("${INCLUDE_DIR}")
include_directories("${CMAKE_SOURCE_DIR}/src/drivers") 

# 2. 寻找 x64 静态库
file(GLOB_RECURSE WEBVIEW2_LIBS 
    "${CMAKE_SOURCE_DIR}/packages/*x64*/*WebView2LoaderStatic.lib"
    "${CMAKE_SOURCE_DIR}/libs/*WebView2LoaderStatic.lib"
)
if(NOT WEBVIEW2_LIBS)
    message(FATAL_ERROR "❌ 找不到 x64 位的 WebView2LoaderStatic.lib！")
endif()
list(GET WEBVIEW2_LIBS 0 FIRST_LIB)

# 3. 编译源码
file(GLOB_RECURSE SOURCE_FILES "${CMAKE_SOURCE_DIR}/src/*.cpp")
add_executable(BatteryMonitor WIN32 ${SOURCE_FILES})

# 4. 强制静态链接宏与属性
target_compile_definitions(BatteryMonitor PRIVATE WEBVIEW2_STATIC)
set_target_properties(BatteryMonitor PROPERTIES VS_GLOBAL_WebView2LoaderPreference "Static")

# 5. 链接所有系统库与 WebView2 静态库
target_link_libraries(BatteryMonitor PRIVATE
    "${FIRST_LIB}"
    user32 shell32 ole32 oleaut32 uuid dwmapi 
    hid setupapi shlwapi gdiplus version
)

# 6. 编译后自动拷贝 UI 文件夹
add_custom_command(TARGET BatteryMonitor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/ui" "$<TARGET_FILE_DIR:BatteryMonitor>/ui"
    COMMENT "自动拷贝 ui 文件夹到输出目录"
)

# 7. 如果有 icon.ico，自动拷贝
if(EXISTS "${CMAKE_SOURCE_DIR}/icon.ico")
    add_custom_command(TARGET BatteryMonitor POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/icon.ico" "$<TARGET_FILE_DIR:BatteryMonitor>"
    )
endif()