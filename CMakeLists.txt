cmake_minimum_required(VERSION 3.16)
project(onvif_client VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ========================================================================
# Find dependencies (FFmpeg + libcurl)
# ========================================================================
find_package(CURL REQUIRED)
find_package(PkgConfig QUIET)

# FFmpeg：运行时库 + 开发头文件
set(AVFORMAT "/usr/lib/x86_64-linux-gnu/libavformat.so.60")
set(AVCODEC  "/usr/lib/x86_64-linux-gnu/libavcodec.so.60")
set(AVUTIL   "/usr/lib/x86_64-linux-gnu/libavutil.so.58")
set(AVFILTER "/usr/lib/x86_64-linux-gnu/libavfilter.so.9")

# FFmpeg 开发头文件（优先 3rdparty，手动下载）
set(FFMPEG_DEV_INCLUDE_ARCH "/home/javen/code/libonvif_client/3rdparty/ffmpeg-dev/include/x86_64-linux-gnu")
if(NOT EXISTS "${FFMPEG_DEV_INCLUDE_ARCH}/libavformat/avformat.h")
    set(FFMPEG_DEV_INCLUDE_ARCH "/usr/include")
endif()

# ========================================================================
# libonvif.so — ONVIF 核心库（onvif.cpp 编译而来）
# ========================================================================
add_library(onvif SHARED)
target_sources(onvif PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/onvif.cpp
)
target_include_directories(onvif PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    "${FFMPEG_DEV_INCLUDE_ARCH}"
)
target_compile_options(onvif PRIVATE -fPIC -O2)
target_link_libraries(onvif PRIVATE
    CURL::libcurl
    "${AVFORMAT}"
    "${AVCODEC}"
    "${AVUTIL}"
    "${AVFILTER}"
    pthread
    dl
)
set_target_properties(onvif PROPERTIES
    OUTPUT_NAME "onvif"
    VERSION 1.0.0
    SOVERSION 1
)

# ========================================================================
# onvif_demo — 演示程序（链接 libonvif.so）
# ========================================================================
add_executable(onvif_demo
    demo/onvif_demo.cpp
)
target_include_directories(onvif_demo PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    "${FFMPEG_DEV_INCLUDE_ARCH}"
)
target_link_libraries(onvif_demo PRIVATE
    onvif
    CURL::libcurl
    "${AVFORMAT}"
    "${AVCODEC}"
    "${AVUTIL}"
    "${AVFILTER}"
    pthread
    dl
)

# ========================================================================
# Install
# ========================================================================
install(TARGETS onvif onvif_demo RUNTIME DESTINATION bin)
install(TARGETS onvif LIBRARY DESTINATION lib)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/src/onvif.h DESTINATION include)

# 打包 .so 库和头文件到 lib/
install(TARGETS onvif
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
)