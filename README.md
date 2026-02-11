# 这是远控工具

## 历史

本项目原来是用于研究机惨。那么众所周知，机惨工具要操作方便。但是，目前主流的机惨工具以命令行为主，不好进行直接操控。
于是，我拿出了我最擅长的python，然后失败了。由于python的高级性，所以它不够底层，也存在性能问题。
为了适应机房的神秘环境，我与AI进行合作，拿出了第二擅长的语言C++，进行极致优化。
然后，我发现我写出了一个性能极好的远程桌面

## 功能：

- 可搭配p2p网络远程控制（需要我另一个仓库的信令服务器）,也可以通过服务器中继

- 屏幕监控

- 鼠标操作

- 基础键盘操作键盘

- 采用高效H.265视频流编码，能够在1-3Mb（125-375KB）的带宽下实现1920x1080 30帧。

- 目前功能较为基础，如果要进行个性化，欢迎在源码上进行修改。



## 怎么使用：

- 下载ffmpeg（https://github.com/BtbN/FFmpeg-Builds/releases）Windows版

- 下载我的https://github.com/BigSmart114514/p2p 可以用github actions，反正我是这么编译的。

- 自己改一下下面的build.bat

- 使用MSVC编译器 （反正我用2022的）

```batch
@echo off
setlocal

:: 设置FFmpeg路径 - 请根据实际安装位置修改
set FFMPEG_DIR=\path\to\ffmpeg

:: 设置Visual Studio环境 - 根据你的VS版本调整路径
call "\path\to\vs\vcvars64.bat"

:: 设置P2P库路径
set P2P_DIR=\sb\114514


echo ========================================
echo    P2P HEVC 远程桌面编译脚本
echo ========================================
echo.
echo FFmpeg路径: %FFMPEG_DIR%
echo P2P库路径: %P2P_DIR%
echo.

:: 检查目录
if not exist "%FFMPEG_DIR%\include" (
    echo 错误: 未找到FFmpeg include目录
    exit /b 1
)

if not exist "%P2P_DIR%\include" (
    echo 错误: 未找到P2P库 include目录
    exit /b 1
)

:: 编译服务端
echo 正在编译P2P服务端...
cl /nologo /EHsc /O2 /MD /std:c++17 /utf-8 ^
   /I"%FFMPEG_DIR%\include" ^
   /I"%P2P_DIR%\include" ^
   /D_CRT_SECURE_NO_WARNINGS ^
   /DP2P_CLIENT_STATIC ^
   /wd4819 ^
   server.cpp ^
   /link /LIBPATH:"%FFMPEG_DIR%\lib" /LIBPATH:"%P2P_DIR%\lib" ^
   avcodec.lib avutil.lib swscale.lib ^
   p2p-client.lib datachannel.lib juice.lib libcrypto.lib libssl.lib usrsctp.lib ^
   ws2_32.lib gdi32.lib user32.lib winmm.lib d3d11.lib dxgi.lib crypt32.lib ^
   /OUT:server.exe

if %ERRORLEVEL% neq 0 (
    echo 服务端编译失败!
    pause
    exit /b 1
)
echo 服务端编译成功!
echo.

:: 编译客户端
echo 正在编译P2P客户端...
cl /nologo /EHsc /O2 /MD /std:c++17 ^
   /I"%FFMPEG_DIR%\include" ^
   /I"%P2P_DIR%\include" ^
   /D_CRT_SECURE_NO_WARNINGS ^
   /DP2P_CLIENT_STATIC ^
   /wd4819 ^
   client.cpp ^
   /link /LIBPATH:"%FFMPEG_DIR%\lib" /LIBPATH:"%P2P_DIR%\lib" ^
   avcodec.lib avutil.lib swscale.lib ^
   p2p-client.lib datachannel.lib juice.lib libcrypto.lib libssl.lib usrsctp.lib ^
   ws2_32.lib gdi32.lib user32.lib dwmapi.lib crypt32.lib ^
   /OUT:client.exe

if %ERRORLEVEL% neq 0 (
    echo 客户端编译失败!
    pause
    exit /b 1
)
echo 客户端编译成功!
echo.

:: 复制DLL文件
echo 正在复制DLL文件...
if exist "%FFMPEG_DIR%\bin" (
    copy /Y "%FFMPEG_DIR%\bin\avcodec*.dll" . >nul 2>&1
    copy /Y "%FFMPEG_DIR%\bin\avutil*.dll" . >nul 2>&1
    copy /Y "%FFMPEG_DIR%\bin\swscale*.dll" . >nul 2>&1
    copy /Y "%FFMPEG_DIR%\bin\swresample*.dll" . >nul 2>&1
)

if exist "%P2P_DIR%\bin" (
    copy /Y "%P2P_DIR%\bin\p2p-client.dll" . >nul 2>&1
    copy /Y "%P2P_DIR%\bin\datachannel.dll" . >nul 2>&1
    copy /Y "%P2P_DIR%\bin\juice.dll" . >nul 2>&1
    copy /Y "%P2P_DIR%\bin\legacy.dll" . >nul 2>&1
    copy /Y "%P2P_DIR%\bin\libcrypto-3-x64.dll" . >nul 2>&1
    copy /Y "%P2P_DIR%\bin\libssl-3-x64.dll" . >nul 2>&1
)

echo DLL文件已复制
echo.

:: 清理
del /Q *.obj >nul 2>&1

echo ========================================
echo    编译完成!
echo ========================================
echo.
echo 使用方法:
echo   1. 启动信令服务器
echo   2. 运行 p2p_server.exe (记下 Peer ID)
echo   3. 运行 p2p_client.exe (输入服务端 Peer ID)
echo.

pause
endlocal
```
- 把编译好的server.exe放进对方电脑里


## 注意：

此软件仅供学习使用，严禁用于非法用途！！！！！！
