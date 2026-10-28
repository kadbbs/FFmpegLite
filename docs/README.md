# FFmpegLite 使用文档

这套文档面向把音视频能力嵌入自己项目的 C++ 开发者，对应仓库当前的公共接口。
示例使用 C++17。除非明确标为业务回调，代码中的类型和方法均来自当前实现。

## 建议阅读顺序

1. [快速开始与 CMake 集成](getting-started.md)：构建、运行、接入自己的工程。
2. [核心概念与执行约定](concepts.md)：帧、压缩包、回调、引用、排空和状态。
3. [memory.cpp 逐行详解](tutorials/memory.md)：从生成图片到编码、解码，解释每条语句。
4. [组合用法](recipes.md)：文件解码、转封装、处理后编码、音频处理和字幕。

## API 参考

| 文档 | 对象 |
| --- | --- |
| [类型、格式、时间与错误](api/types.md) | Rational、Timestamp、Duration、枚举、布局、CodecParameters、StreamInfo、Error |
| [压缩包与原始帧](api/data.md) | Packet、VideoFrame、AudioFrame、PlaneView、MutablePlaneView |
| [编码与解码](api/codecs.md) | VideoDecoder、AudioDecoder、VideoEncoder、AudioEncoder、配置项 |
| [视频与音频处理](api/processors.md) | VideoProcessor、AudioProcessor |
| [容器、字幕与便捷读取](api/io.md) | Demuxer、Muxer、SubtitleTrack、VideoReader |

可直接编译的文档示例位于 [examples/](examples/)。构建方式见快速开始。
整体架构取舍见 [DESIGN.md](../DESIGN.md)，具体使用行为以本目录和公共头文件为准。

## 当前能力

| 能力 | 状态 |
| --- | --- |
| 内存中的音视频编解码 | 已实现，具体编码器取决于 FFmpeg 构建 |
| 原始帧分配、外部内存包装、写时复制 | 已实现 |
| 缩放、裁剪、像素格式转换 | 已实现 |
| 重采样、声道转换、采样格式转换、音量 | 已实现 |
| 容器读写、流分发、时间基换算、seek | 已实现 |
| SRT 加载与 MKV/MP4 字幕轨封装 | 已实现 |
| 自动排空的逐帧视频读取 | 已实现 |
| 字幕烧录到画面 | 未实现，配置时抛出 Unavailable |
| 硬件帧、硬件加速、协议组包、非阻塞 I/O | 当前不提供 |
| 通用滤镜字符串、原生 FFmpeg 互操作入口 | 当前不提供 |

最常用的调用形式：

```cpp
decoder.decode(packet, on_frame);
auto processed = video.process(frame);
audio.process(audio_frame, on_audio);
encoder.encode(frame, on_packet);
```

解码、编码和音频处理完成后显式调用 `flush()`；封装完成后调用 `finish()`。
这些结束操作的顺序决定延迟数据是否完整交付，详见[执行约定](concepts.md)。
