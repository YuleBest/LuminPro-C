# 从源代码编译

从 [源代码](https://github.com/YuleBest/LuminPro-C/blob/main/source/service.c) 进行编译是一个简单的过程，这里提供在 Android 上编译的教程。

1. 下载安装 [Termux](https://github.com/termux/termux-app)。

2. 执行以下代码以完成环境配置：

```bash
pkg update
pkg upgrade
pkg install clang
```

3. 下载 [源代码](https://github.com/YuleBest/LuminPro-C/blob/main/source/service.c) ，放到 `/data/user/0/com.termux/files/home/`

4. 使用以下代码进行编译，你会得到一个 `service` 无后缀文件：

```bash
clang -o service service.c
```

5. 使用得到的 `service` 替换掉 [Release](https://github.com/YuleBest/LuminPro-C/release/tag/latest) 的压缩包内的 `service`。