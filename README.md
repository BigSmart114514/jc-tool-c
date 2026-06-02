# 这是远控工具

## 历史

本项目原来是用于研究机惨。
于是，我拿出了我最擅长的python，然后失败了。由于python的语言特性，它不够底层，也存在性能问题。
于是，我使用AI进行，使用C++进行优化。
然后，我发现这个东西其实成为了一个非常好用的远程桌面。

## 发癫

- Easytier 万岁

> 它不仅可以通过dll的方式加载，它还有免费公用服务器

- Windows Terminal 万岁

> 这个终端制作真是微软良心之作。

- 开源社区万岁

## 功能：

- 可搭配p2p网络远程控制

- 基础键盘操作

- 采用H.264视频流传输（主要在转向D3D11的时候把H265舍弃了）。

- SFTP文件上传 / 下载

- ssh终端服务器（使用conPTY）。

- 目前功能较为基础，如果要进行个性化，欢迎在源码上进行修改。



## 编译：

- 下载qtermwidget 2.4.0 ，命名为 **qtermwidget-2.4.0** 放在 **..** 下

- 你可以选择自己fork一份，然后用github actions编译 **Build EasyTier FFI for Windows**, 产物为 **easytier_ffi.dll** 和 **easytier_ffi.lib**。

- 从 ![Easytier](https://github.com/EasyTier/EasyTier/releases) 下载 **easytier-windows-x86_64-v2.6.4.zip** （别忘了点 **Show all Assets** ）。把里面的 **Packet.dll** 和 **Wintun.dll** 提取出来。

- 把 **Easytier** 有关文件整合一下，放到 **../easytier** 里。

```text
easytier
├─bin
│      easytier_ffi.dll
│      Packet.dll
│      wintun.dll
│
└─lib
        easytier_ffi.lib
```
- 下载 ![Windows Terminal](https://github.com/microsoft/terminal/releases) 的 **Microsoft.Windows.Console.ConPTY** 解压以后放在 **../Microsoft.Windows.Console.ConPTY** 下。

- 下载 libssh 解压以后放在 **../libssh** 下。

- 自己改一下的build.bat

- 使用MSVC编译器 （反正我用2026的，但是github actions是用2022的，所以两个都可以）
