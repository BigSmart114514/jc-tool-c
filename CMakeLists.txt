cmake_minimum_required(VERSION 3.16)
project(RemoteControlApp VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Qt 配置
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network)

# FFmpeg (minimal build)
set(FFMPEG_DIR "${CMAKE_SOURCE_DIR}/../ffmpeg-minimal")
if(NOT EXISTS ${FFMPEG_DIR})
    message(FATAL_ERROR "FFmpeg not found at ${FFMPEG_DIR}")
endif()
include_directories(${FFMPEG_DIR}/include)
link_directories(${FFMPEG_DIR}/lib)

# easytier
set(EASYTIER_DIR "${CMAKE_SOURCE_DIR}/../easytier")
if(NOT EXISTS ${EASYTIER_DIR})
    message(FATAL_ERROR "Easytier library not found at ${EASYTIER_DIR}")
endif()
include_directories(${EASYTIER_DIR}/include)
link_directories(${EASYTIER_DIR}/lib)

# libssh
set(LIBSSH_DIR "${CMAKE_SOURCE_DIR}/../libssh")
if(NOT EXISTS ${LIBSSH_DIR})
    message(FATAL_ERROR "libssh not found at ${LIBSSH_DIR}")
endif()
include_directories(${LIBSSH_DIR}/include)
link_directories(${LIBSSH_DIR}/lib)

add_definitions(-D_CRT_SECURE_NO_WARNINGS -DP2P_CLIENT_STATIC -DWITH_SERVER)

if(MSVC)
    add_compile_options(/utf-8)
endif()

# 公共库列表
set(COMMON_LIBS
    ws2_32 crypt32 avcodec avutil swscale easytier_ffi
)

# 收集所有的源文件 (包含Client和Server的所有功能)
set(APP_SOURCES
    app/main.cpp
    client/control_panel.cpp
    client/desktop_window.cpp
    client/sftp_window.cpp
    client/ssh_terminal.cpp
    client/terminal_widget.cpp
    client/connection_dialog.cpp
    client/hevc_decoder.cpp
    client/display_buffer.cpp
    server/desktop_service.cpp
    server/file_service.cpp
    server/hevc_encoder.cpp
    server/screen_capture.cpp
    server/ssh_server.cpp
    common/transport_tcp.cpp
    common/easytier_manager.cpp
    common/ssh_session.cpp
)

set(APP_HEADERS
    client/control_panel.h
    client/desktop_window.h
    client/sftp_window.h
    client/ssh_terminal.h
    client/terminal_widget.h
    client/connection_dialog.h
    client/hevc_decoder.h
    client/display_buffer.h
    server/desktop_service.h
    server/file_service.h
    server/hevc_encoder.h
    server/screen_capture.h
    server/ssh_server.h
    common/easytier_manager.h
    common/ssh_session.h
)

# 创建单个可执行文件 (使用 WIN32 隐藏控制台，只显示 Qt 界面)
#add_executable(RemoteControl WIN32 ${APP_SOURCES} ${APP_HEADERS})
add_executable(RemoteControl ${APP_SOURCES} ${APP_HEADERS})
set_property(TARGET RemoteControl APPEND_STRING PROPERTY LINK_FLAGS " /MANIFESTUAC:\"level='requireAdministrator' uiAccess='false'\"")

target_include_directories(RemoteControl PRIVATE ${CMAKE_SOURCE_DIR})

target_link_libraries(RemoteControl
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network
    ${COMMON_LIBS}
    gdi32 user32 winmm d3d11 dxgi wtsapi32
    ssh
)

# 输出目录
set_target_properties(RemoteControl PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/Release
    RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/Debug
)

# 复制 DLL
if(WIN32)
    # 可靠地获取 Qt bin 目录
    find_program(QT_QMAKE_EXECUTABLE qmake HINTS ${Qt6_DIR} ENV Qt6_DIR REQUIRED)
    get_filename_component(QT_BIN_DIR "${QT_QMAKE_EXECUTABLE}" DIRECTORY)
    message(STATUS "Qt bin directory: ${QT_BIN_DIR}")

    file(GLOB EASYTIER_DLLS "${EASYTIER_DIR}/bin/*.dll")
    file(GLOB LIBSSH_DLLS "${LIBSSH_DIR}/bin/*.dll")

    # Qt 核心 DLL（根据实际需要添加，可用 windeployqt --dry-run 查看）
    set(QT_DLLS
        "${QT_BIN_DIR}/Qt6Core.dll"
        "${QT_BIN_DIR}/Qt6Gui.dll"
        "${QT_BIN_DIR}/Qt6Widgets.dll"
        "${QT_BIN_DIR}/Qt6Network.dll"
    )

    set(FFMPEG_DLLS
        "${FFMPEG_DIR}/bin/avcodec-61.dll"
        "${FFMPEG_DIR}/bin/avutil-59.dll"
        "${FFMPEG_DIR}/bin/swscale-8.dll"
        "${FFMPEG_DIR}/bin/swresample-5.dll"
    )

    # 如果使用其他模块，请补充对应的 DLL

    add_custom_command(TARGET RemoteControl POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:RemoteControl>/platforms
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FFMPEG_DLLS} ${EASYTIER_DLLS} ${LIBSSH_DLLS} ${QT_DLLS} $<TARGET_FILE_DIR:RemoteControl>
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${QT_BIN_DIR}/../plugins/platforms/qwindows.dll" $<TARGET_FILE_DIR:RemoteControl>/platforms/
        # 根据需要添加其他平台插件
        # COMMAND ${CMAKE_COMMAND} -E copy_if_different "${QT_BIN_DIR}/../plugins/platforms/qdirect2d.dll" $<TARGET_FILE_DIR:RemoteControl>/platforms/
        COMMENT "Copying runtime DLLs and Qt platform plugin..."
    )
endif()
