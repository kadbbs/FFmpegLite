# 快速开始与 CMake 集成

[文档首页](README.md)

## 1. 环境

- CMake 3.20 或更新版本。
- 支持 C++17 的编译器和 C 编译器。
- GNU Make、POSIX shell，以及 FFmpeg configure 所需的基础构建工具。
- 本地 FFmpeg 源码，当前验证版本为 7.1.1。

当前验证平台为 Linux。原生 Windows/MSVC 尚未适配；macOS 未完成验证。
默认关闭汇编优化，因此初次构建不要求 nasm/yasm。

## 2. 准备 FFmpeg

当前工作区已经有 `third_party/ffmpeg/`。该目录不纳入版本控制，新工作副本需要提供源码。
源码目录应保持未配置状态，由 CMake 在独立目录调用 FFmpeg 的构建系统。

```sh
curl -fL https://ffmpeg.org/releases/ffmpeg-7.1.1.tar.xz -o ffmpeg-7.1.1.tar.xz
mkdir -p third_party
tar -xJf ffmpeg-7.1.1.tar.xz -C third_party
mv third_party/ffmpeg-7.1.1 third_party/ffmpeg
```

本次使用的归档 SHA-256：

```text
733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1
```

构建过程中不会自动下载 FFmpeg。已有源码可通过下列参数指定：

```sh
cmake -S . -B build -DFFLITE_FFMPEG_SOURCE_DIR=/absolute/path/to/ffmpeg
```

## 3. 构建与运行

在仓库根目录执行：

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
ctest --test-dir build --output-on-failure
./build/fflite_memory
```

内存示例应输出：

```text
12 frames encoded and decoded entirely in memory
```

读取视频文件：

```sh
./build/fflite_decode /path/to/input.mp4
```

该命令读取并处理帧，输出帧数；它没有图形窗口，也不播放音频。

## 4. 精简 FFmpeg 构建

只需要运行仓库测试时，可以缩小 FFmpeg 组件范围：

```sh
cmake -S . -B build-minimal \
  '-DFFLITE_FFMPEG_CONFIGURE_ARGS=--disable-everything;--enable-encoder=mpeg4,pcm_s16le,aac,movtext;--enable-decoder=mpeg4,pcm_s16le,aac,subrip,movtext;--enable-demuxer=matroska,mov,avi,srt;--enable-muxer=matroska,mp4,avi;--enable-protocol=file;--enable-parser=mpeg4video,aac'
cmake --build build-minimal -j2
ctest --test-dir build-minimal --output-on-failure
```

此配置没有 H.264 解码或编码能力，不能拿它处理常见的 H.264 MP4。
默认完整配置包含内置 H.264 解码器；H.264 软件编码通常还需要配置外部编码器及链接依赖。
`Codec::H264` 只是请求编码标准，并不安装编码器。不可用时构造会抛异常。

附加参数以分号分隔。CMake 会启用 fflite 当前需要的基本音频滤镜。
外部静态依赖需要自行补充相应链接配置。

## 5. 接入自己的工程

假设目录如下：

```text
my-app/
  CMakeLists.txt
  main.cpp
  vendor/
    FFmpegLite/
      CMakeLists.txt
      include/
      third_party/ffmpeg/
```

应用的 `CMakeLists.txt`：

```cmake
cmake_minimum_required(VERSION 3.20)
project(my_app LANGUAGES C CXX)

set(FFLITE_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
add_subdirectory(vendor/FFmpegLite)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE fflite::fflite)
```

`BUILD_TESTING` 是 CTest 通用选项，会同时影响当前构建中的其他子项目。
如果主工程已经管理测试开关，应沿用主工程约定，而不是强制覆盖。

`main.cpp` 可以直接采用 [memory_walkthrough.cpp](examples/memory_walkthrough.cpp)。
只链接 `fflite::fflite` 即可取得 C++17 要求、头文件路径和链接依赖，不需要在应用中包含 FFmpeg 头文件。
当前提供源码子目录集成，尚未提供安装后的 `find_package(fflite)` 包配置。

## 6. 文档附带的完整示例

| 文件 | 用途 | 参数 |
| --- | --- | --- |
| [memory_walkthrough.cpp](examples/memory_walkthrough.cpp) | 带注释的纯内存编码解码 | 无 |
| [file_transcode.cpp](examples/file_transcode.cpp) | 解码首选视频流、缩放并编码到容器 | 输入路径、输出路径 |
| [audio_memory.cpp](examples/audio_memory.cpp) | 生成 PCM、重采样并编码 AAC | 无 |
| [encode_file.cpp](examples/encode_file.cpp) | 生成 12 张图片并录制到容器 | 输出路径 |

这些文件都有独立的 `main()`。每次选其中一个作为应用源码，避免把多个 main 链入同一个程序。
文件转码示例只输出视频，不保留音频；需要多流处理时按[组合用法](recipes.md)显式分发。

也可以直接在仓库中构建这四个示例：

```sh
cmake -S . -B build -DFFLITE_BUILD_DOC_EXAMPLES=ON
cmake --build build -j2
./build/docs/examples/fflite_doc_memory_walkthrough
./build/docs/examples/fflite_doc_audio_memory
./build/docs/examples/fflite_doc_encode_file /tmp/fflite-input.mkv
./build/docs/examples/fflite_doc_file_transcode /tmp/fflite-input.mkv /tmp/fflite-output.mkv
```

录制与转码示例要求输出文件不存在，重跑时自行选择新路径。

## 7. 构建产物

| 路径 | 内容 |
| --- | --- |
| `build/ffmpeg-build/` | FFmpeg 的编译工作目录 |
| `build/ffmpeg-install/` | 供 fflite 使用的 FFmpeg 静态库和头文件 |
| `build/libfflite.a` | 默认静态库构建时的 fflite 产物 |
| `build/fflite_memory` | 纯内存示例 |
| `build/fflite_decode` | 文件读取示例 |

内部调用 FFmpeg 库函数，不启动 ffmpeg 命令行进程。
公开头使用 PImpl 隐藏内部 FFmpeg 类型；FFmpeg 仍是必须编译和链接的底层依赖。
