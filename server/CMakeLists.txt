# 源文件
set(SERVER_SOURCES
    main.cpp
    desktop_service.cpp
    file_service.cpp
    hevc_encoder.cpp
    screen_capture.cpp
    ${CMAKE_SOURCE_DIR}/common/transport_tcp.cpp
    ${CMAKE_SOURCE_DIR}/common/transport_p2p.cpp
)

set(SERVER_HEADERS
    desktop_service.h
    file_service.h
    hevc_encoder.h
    screen_capture.h
)

# 创建可执行文件
add_executable(server ${SERVER_SOURCES} ${SERVER_HEADERS})

target_include_directories(server PRIVATE ${CMAKE_SOURCE_DIR})

target_link_libraries(server
    ${COMMON_LIBS}
    gdi32
    user32
    winmm
    d3d11
    dxgi
)

# 输出目录
set_target_properties(server PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/Release
    RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/Debug
)

# 复制 DLL
if(WIN32)
    file(GLOB FFMPEG_DLLS "${FFMPEG_DIR}/bin/*.dll")
    file(GLOB P2P_DLLS "${P2P_DIR}/bin/*.dll")
    
    add_custom_command(TARGET server POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${FFMPEG_DLLS} ${P2P_DLLS}
        $<TARGET_FILE_DIR:server>
        COMMENT "Copying DLLs to server..."
    )
endif()