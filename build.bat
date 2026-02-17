@echo off
setlocal
chcp 65001 >nul

set FFMPEG_DIR=../ffmpeg
set P2P_DIR=../p2p-client-windows

call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" 2>nul
if errorlevel 1 (
    call "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat" 2>nul
)

echo ========================================
echo   Remote Control Build
echo ========================================
echo.

set COMMON_CPP=common\transport_tcp.cpp common\transport_p2p.cpp
set SERVER_CPP=server\main.cpp server\screen_capture.cpp server\hevc_encoder.cpp server\desktop_service.cpp server\file_service.cpp
set CLIENT_CPP=client\main.cpp client\control_panel.cpp client\hevc_decoder.cpp client\display_buffer.cpp client\desktop_window.cpp client\file_window.cpp

set INCLUDES=/I"%FFMPEG_DIR%\include" /I"%P2P_DIR%\include" /I"."
set DEFINES=/D_CRT_SECURE_NO_WARNINGS /DP2P_CLIENT_STATIC
set FLAGS=/nologo /EHsc /O2 /MD /std:c++17 /utf-8 /wd4819

set LIBS_COMMON=ws2_32.lib crypt32.lib
set LIBS_FFMPEG=avcodec.lib avutil.lib swscale.lib
set LIBS_P2P=p2p-client.lib datachannel.lib juice.lib libcrypto.lib libssl.lib usrsctp.lib
set LIBS_SERVER=gdi32.lib user32.lib winmm.lib d3d11.lib dxgi.lib
set LIBS_CLIENT=gdi32.lib user32.lib dwmapi.lib comctl32.lib shell32.lib shlwapi.lib comdlg32.lib

echo [1/2] Building Server...
cl %FLAGS% %INCLUDES% %DEFINES% ^
   %COMMON_CPP% %SERVER_CPP% ^
   /Fe:server.exe ^
   /link /LIBPATH:"%FFMPEG_DIR%\lib" /LIBPATH:"%P2P_DIR%\lib" ^
   %LIBS_FFMPEG% %LIBS_P2P% %LIBS_COMMON% %LIBS_SERVER%

if %ERRORLEVEL% neq 0 (
    echo Server FAILED!
    pause
    exit /b 1
)
echo Server OK!

echo.
echo [2/2] Building Client...
cl %FLAGS% %INCLUDES% %DEFINES% ^
   %COMMON_CPP% %CLIENT_CPP% ^
   /Fe:client.exe ^
   /link /LIBPATH:"%FFMPEG_DIR%\lib" /LIBPATH:"%P2P_DIR%\lib" ^
   %LIBS_FFMPEG% %LIBS_P2P% %LIBS_COMMON% %LIBS_CLIENT%

if %ERRORLEVEL% neq 0 (
    echo Client FAILED!
    pause
    exit /b 1
)
echo Client OK!

echo.
echo Copying DLLs...
if exist "%FFMPEG_DIR%\bin" copy /Y "%FFMPEG_DIR%\bin\*.dll" . >nul 2>&1
if exist "%P2P_DIR%\bin" copy /Y "%P2P_DIR%\bin\*.dll" . >nul 2>&1

del /Q *.obj >nul 2>&1

echo.
echo ========================================
echo   Build Complete!
echo ========================================
echo.
pause
endlocal