@echo off
setlocal
chcp 65001 >nul

echo ========================================
echo   Remote Control Build (Qt6)
echo ========================================
echo.

:: 设置 Qt 路径
set QT_DIR=%QT6_DIR%
if not exist "%QT_DIR%" (
    echo Error: Qt not found at %QT_DIR%
    pause
    exit /b 1
)

set CMAKE_PREFIX_PATH=%QT_DIR%
echo Qt Path: %QT_DIR%
echo.

:: 检查 FFmpeg
set FFMPEG_DIR=%~dp0..\ffmpeg
if not exist "%FFMPEG_DIR%" (
    echo Warning: FFmpeg not found at %FFMPEG_DIR%
    echo Please ensure FFmpeg is installed correctly
)

:: 检查 P2P
set P2P_DIR=%~dp0..\p2p-client-windows
if not exist "%P2P_DIR%" (
    echo Warning: P2P library not found at %P2P_DIR%
    echo Please ensure P2P library is installed correctly
)

:: 检查 CMake
where cmake >nul 2>&1
if errorlevel 1 (
    echo Error: CMake not found! Please install CMake and add to PATH
    pause
    exit /b 1
)

:: 清理旧构建
if exist build (
    echo Cleaning old build...
    rmdir /s /q build
)

:: 创建构建目录
mkdir build
cd build

:: 配置项目
echo [1/2] Configuring...
cmake -G "Visual Studio 18 2026" -A x64 ..
if errorlevel 1 (
    echo.
    echo ========================================
    echo Configuration FAILED!
    echo ========================================
    echo.
    echo Please check:
    echo 1. Qt path is correct
    echo 2. FFmpeg is at: %FFMPEG_DIR%
    echo 3. P2P library is at: %P2P_DIR%
    echo.
    cd ..
    pause
    exit /b 1
)

:: 编译
echo.
echo [2/2] Building...
cmake --build . --config Release -j 8
if errorlevel 1 (
    echo.
    echo ========================================
    echo Build FAILED!
    echo ========================================
    cd ..
    pause
    exit /b 1
)

cd ..

echo.
echo ========================================
echo   Build Complete!
echo ========================================
echo.
echo Output file is in: build\Release\
@REM echo   - client.exe
@REM echo   - server.exe
@REM echo.
@REM echo Run client: cd build\Release ^&^& client.exe
@REM echo Run server: cd build\Release ^&^& server.exe
echo.

pause
endlocal