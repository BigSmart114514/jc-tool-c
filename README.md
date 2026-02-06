# 这是远控工具

## 历史

本项目原来是用于研究机惨。那么众所周知，机惨工具要操作方便。但是，目前主流的机惨工具以命令行为主，不好进行直接操控。
于是，我拿出了我最擅长的python，然后失败了。由于python的高级性，所以它不够底层，也存在性能问题。
为了适应机房的神秘环境，我与AI进行合作，拿出了第二擅长的语言C++，进行极致优化。
然后，我发现我写出了一个性能极好的远程桌面

## 功能：

- 屏幕监控

- 鼠标操作

- 基础键盘操作键盘

- 采用高效H.265视频流编码，能够在1-3Mb（125-375KB）的带宽下实现1920x1080 30帧。

- 目前功能较为基础，如果要进行个性化，欢迎在源码上进行修改。



## 怎么使用：

- 下载ffmpeg（https://github.com/BtbN/FFmpeg-Builds/releases）Windows版

- 自己改一下下面的build.bat

- 使用MSVC编译器 （反正我用2022的）

```batch
@echo off
setlocal

:: 设置FFmpeg路径 - 请根据实际安装位置修改
set FFMPEG_DIR=C:\test...

:: 设置Visual Studio环境 - 根据你的VS版本调整路径
call "C:\Visual Studio\Community\VC\Auxiliary\Build\vcvars64.bat"

echo ========================================
echo    HEVC远程桌面编译脚本
echo ========================================
echo.
echo FFmpeg路径: %FFMPEG_DIR%
echo.

:: 检查FFmpeg目录是否存在
if not exist "%FFMPEG_DIR%\include" (
    echo 错误: 未找到FFmpeg include目录
    echo 请确认 %FFMPEG_DIR%\include 存在
    exit /b 1
)

if not exist "%FFMPEG_DIR%\lib" (
    echo 错误: 未找到FFmpeg lib目录
    echo 请确认 %FFMPEG_DIR%\lib 存在
    exit /b 1
)

:: 编译服务器
echo 正在编译服务器...
cl /nologo /EHsc /O2 /MD ^
   /I"%FFMPEG_DIR%\include" ^
   /D_CRT_SECURE_NO_WARNINGS ^
   /wd4819 ^
   server.cpp ^
   /link /LIBPATH:"%FFMPEG_DIR%\lib" ^
   avcodec.lib avutil.lib swscale.lib ^
   ws2_32.lib gdi32.lib user32.lib ^
   /OUT:server.exe

if %ERRORLEVEL% neq 0 (
    echo.
    echo 服务器编译失败!
    pause
    exit /b 1
)
echo 服务器编译成功!
echo.

:: 编译客户端
echo 正在编译客户端...
cl /nologo /EHsc /O2 /MD ^
   /I"%FFMPEG_DIR%\include" ^
   /D_CRT_SECURE_NO_WARNINGS ^
   /wd4819 ^
   client.cpp ^
   /link /LIBPATH:"%FFMPEG_DIR%\lib" ^
   avcodec.lib avutil.lib swscale.lib ^
   ws2_32.lib gdi32.lib user32.lib dwmapi.lib ^
   /OUT:client.exe

if %ERRORLEVEL% neq 0 (
    echo.
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
    echo DLL文件已复制
) else (
    echo 警告: 未找到 %FFMPEG_DIR%\bin 目录
    echo 请手动复制FFmpeg DLL文件到可执行文件目录
)

echo.
echo ========================================
echo    编译完成!
echo ========================================
echo.

:: 清理临时文件
del /Q *.obj >nul 2>&1

pause
endlocal
```
- 把编译好的server.exe和server.exe放进对方电脑里

- 自己改ip和端口号！

## 注意：

此软件仅供学习使用，严禁用于非法用途！！！！！！
