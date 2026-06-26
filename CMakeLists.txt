cmake_minimum_required(VERSION 3.16)
project(RemoteControlApp VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Qt 配置
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network)

# easytier
set(EASYTIER_DIR "${CMAKE_SOURCE_DIR}/../easytier")
if(NOT EXISTS ${EASYTIER_DIR})
    message(FATAL_ERROR "Easytier library not found at ${EASYTIER_DIR}")
endif()
include_directories(${CMAKE_SOURCE_DIR}/thirdparty/easytier/include)
link_directories(${EASYTIER_DIR}/lib)

# libssh
set(LIBSSH_DIR "${CMAKE_SOURCE_DIR}/../libssh")
if(NOT EXISTS ${LIBSSH_DIR})
    message(FATAL_ERROR "libssh not found at ${LIBSSH_DIR}")
endif()
include_directories(${LIBSSH_DIR}/include)
link_directories(${LIBSSH_DIR}/lib)

# qtermwidget (terminal emulation engine)
add_subdirectory(thirdparty/qtermwidget)
include_directories(${CMAKE_SOURCE_DIR}/thirdparty/qtermwidget)
include_directories(${CMAKE_SOURCE_DIR}/../qtermwidget-2.4.0/lib)

add_definitions(-D_CRT_SECURE_NO_WARNINGS -DP2P_CLIENT_STATIC -DWITH_SERVER)

if(MSVC)
    add_compile_options(/utf-8)
endif()

# 公共库列表
set(COMMON_LIBS
    ws2_32 crypt32 easytier_ffi bcrypt
    mfplat mfuuid mfreadwrite propsys
)

# 收集所有的源文件 (包含Client和Server的所有功能)
set(APP_SOURCES
    app/main.cpp
    client/control_panel.cpp
    client/desktop_window.cpp
    client/sftp_window.cpp
    client/ssh_terminal.cpp
    client/connection_dialog.cpp
    client/service_manager_dialog.cpp
    client/media_decoder.cpp
    server/desktop_service.cpp
    server/file_service.cpp
    server/media_encoder.cpp
    server/screen_capture.cpp
    server/ssh_server.cpp
    common/transport_tcp.cpp
    common/easytier_control.cpp
    common/ssh_session.cpp
)

set(APP_HEADERS
    client/control_panel.h
    client/desktop_window.h
    client/sftp_window.h
    client/ssh_terminal.h
    client/connection_dialog.h
    client/service_manager_dialog.h
    client/media_decoder.h
    server/desktop_service.h
    server/file_service.h
    server/media_encoder.h
    server/screen_capture.h
    server/ssh_server.h
    common/easytier_control.h
    common/ssh_session.h
)

# 创建单个可执行文件 (使用 WIN32 隐藏控制台，只显示 Qt 界面)
#add_executable(RemoteControl WIN32 ${APP_SOURCES} ${APP_HEADERS})
add_executable(RemoteControl ${APP_SOURCES} ${APP_HEADERS})
set_property(TARGET RemoteControl APPEND_STRING PROPERTY LINK_FLAGS " /MANIFESTUAC:\"level='requireAdministrator' uiAccess='false'\"")

target_include_directories(RemoteControl PRIVATE ${CMAKE_SOURCE_DIR})

target_link_libraries(RemoteControl
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network
    qtermwidget
    ${COMMON_LIBS}
    gdi32 user32 winmm d3d11 dxgi d3dcompiler wtsapi32
    ssh
)

# 输出目录
set_target_properties(RemoteControl PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/Release
    RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/Debug
)

# ======================= EasyTierService (常驻 SYSTEM 服务) =======================
# 独立的 Windows Service 程序，以 SYSTEM 权限运行 EasyTier，通过 Named Pipe 与 RemoteControl 通讯
add_executable(EasyTierService
    service/service_main.cpp
    service/easytier_service.cpp
    common/easytier_control.cpp
)
target_include_directories(EasyTierService PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/thirdparty/easytier/include
)
target_link_libraries(EasyTierService
    ws2_32
    crypt32
    easytier_ffi
)
set_target_properties(EasyTierService PROPERTIES
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

    set(CONPTY_DIR "${CMAKE_SOURCE_DIR}/../Microsoft.Windows.Console.ConPTY")
    if(EXISTS "${CONPTY_DIR}/runtimes/win-x64/native/conpty.dll")
        set(CONPTY_DLLS
            "${CONPTY_DIR}/runtimes/win-x64/native/conpty.dll"
            "${CONPTY_DIR}/build/native/runtimes/x64/OpenConsole.exe"
        )
    endif()

    add_custom_command(TARGET RemoteControl POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:RemoteControl>/platforms
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${EASYTIER_DLLS} ${LIBSSH_DLLS} ${QT_DLLS} ${CONPTY_DLLS} $<TARGET_FILE_DIR:RemoteControl>
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${QT_BIN_DIR}/../plugins/platforms/qwindows.dll" $<TARGET_FILE_DIR:RemoteControl>/platforms/
        # 根据需要添加其他平台插件
        # COMMAND ${CMAKE_COMMAND} -E copy_if_different "${QT_BIN_DIR}/../plugins/platforms/qdirect2d.dll" $<TARGET_FILE_DIR:RemoteControl>/platforms/
        COMMENT "Copying runtime DLLs, Qt platform plugin, and ConPTY binaries..."
    )

    # EasyTierService 也需要 easytier DLL
    add_custom_command(TARGET EasyTierService POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${EASYTIER_DLLS} $<TARGET_FILE_DIR:EasyTierService>
        COMMENT "Copying EasyTier DLLs for EasyTierService..."
    )
endif()
