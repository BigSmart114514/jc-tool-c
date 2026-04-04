cmake_minimum_required(VERSION 3.16)
project(RemoteControl VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# FFmpeg
set(FFMPEG_DIR "${CMAKE_SOURCE_DIR}/../ffmpeg")
if(NOT EXISTS ${FFMPEG_DIR})
    message(FATAL_ERROR "FFmpeg not found at ${FFMPEG_DIR}")
endif()

include_directories(${FFMPEG_DIR}/include)
link_directories(${FFMPEG_DIR}/lib)

# P2P
set(P2P_DIR "${CMAKE_SOURCE_DIR}/../p2p-client-windows")
if(NOT EXISTS ${P2P_DIR})
    message(FATAL_ERROR "P2P library not found at ${P2P_DIR}")
endif()

include_directories(${P2P_DIR}/include)
link_directories(${P2P_DIR}/lib)

add_definitions(-D_CRT_SECURE_NO_WARNINGS -DP2P_CLIENT_STATIC)

# 添加 UTF-8 编码支持
if(MSVC)
    add_compile_options(/utf-8)
endif()

# 公共库列表（不包括 Qt）
set(COMMON_LIBS
    ws2_32
    crypt32
    avcodec
    avutil
    swscale
    p2p-client
    datachannel
    juice
    libcrypto
    libssl
    usrsctp
)

# 添加子项目
add_subdirectory(server)
add_subdirectory(client)