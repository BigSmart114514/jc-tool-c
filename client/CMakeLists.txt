# Qt 配置
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network)

# 源文件
set(CLIENT_SOURCES
    main.cpp
    control_panel.cpp
    desktop_window.cpp
    file_window.cpp
    hevc_decoder.cpp
    display_buffer.cpp
    ${CMAKE_SOURCE_DIR}/common/transport_tcp.cpp
    ${CMAKE_SOURCE_DIR}/common/transport_p2p.cpp
)

set(CLIENT_HEADERS
    control_panel.h
    desktop_window.h
    file_window.h
    hevc_decoder.h
    display_buffer.h
)

# 创建可执行文件
add_executable(client WIN32 ${CLIENT_SOURCES} ${CLIENT_HEADERS})

target_include_directories(client PRIVATE ${CMAKE_SOURCE_DIR})

target_link_libraries(client
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
    ${COMMON_LIBS}
)

# 输出目录
set_target_properties(client PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/Release
    RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/Debug
)

# 复制 DLL
if(WIN32)
    # FFmpeg 和 P2P DLL
    file(GLOB FFMPEG_DLLS "${FFMPEG_DIR}/bin/*.dll")
    file(GLOB P2P_DLLS "${P2P_DIR}/bin/*.dll")
    
    add_custom_command(TARGET client POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${FFMPEG_DLLS} ${P2P_DLLS}
        $<TARGET_FILE_DIR:client>
        COMMENT "Copying FFmpeg and P2P DLLs..."
    )
    
    # Qt DLL
    get_target_property(QT_QMAKE_EXECUTABLE Qt6::qmake IMPORTED_LOCATION)
    get_filename_component(QT_BIN_DIR "${QT_QMAKE_EXECUTABLE}" DIRECTORY)
    
    set(QT_DLLS
        "${QT_BIN_DIR}/Qt6Core.dll"
        "${QT_BIN_DIR}/Qt6Gui.dll"
        "${QT_BIN_DIR}/Qt6Widgets.dll"
        "${QT_BIN_DIR}/Qt6Network.dll"
    )
    
    add_custom_command(TARGET client POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${QT_DLLS}
        $<TARGET_FILE_DIR:client>
        COMMENT "Copying Qt DLLs..."
    )
    
    # Qt 平台插件
    add_custom_command(TARGET client POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
        $<TARGET_FILE_DIR:client>/platforms
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${QT_BIN_DIR}/../plugins/platforms/qwindows.dll"
        $<TARGET_FILE_DIR:client>/platforms/
        COMMENT "Copying Qt platform plugin..."
    )
endif()