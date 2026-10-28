# FFmpegLite 核心 API 设计草案

状态：整体架构已确定，核心接口已有首版实现。旧 Pipeline 原型已移除。
Packet/Frame、音视频编解码、处理器、容器、SRT 字幕轨和 VideoReader 已实现；
字幕烧录和高级扩展仍是后续设计，具体可用范围与构建方法见 README.md。
目标语言 C++17，CMake 管理，本地编译 FFmpeg。

## 1. 目标和设计标准

fflite 是可以嵌入应用的音视频基础库。应用决定输入来源、处理步骤、调用时机和输出去向；
fflite 提供直接可用的音视频对象，管理 FFmpeg 上下文、数据引用、格式转换和收发细节。

核心对象应能独立用于摄像头、播放器、实时通信、自定义网络协议和文件处理。
文件路径、输出文件、运行时管道图都不是使用核心编解码功能的前提。

接口遵守以下标准：

1. 常见用途优先采用构造对象后调用方法的方式；不要求初始化全局环境、分配上下文和手工释放。
2. 必选参数在构造时提供，可选参数通过有字段名的配置结构表达。
3. 公共接口不要求调用方编写 FFmpeg 参数字典、滤镜表达式或手工处理 EAGAIN。
4. 输入数据和输出结果都能被应用直接访问，应用可以在任意一步插入自己的处理。
5. 内存所有权、流结束和错误必须有明确语义，不能依赖隐式行为保证结果完整。
6. 不替应用创建线程、事件循环或无界队列，不擅自播放、丢帧、重置时间轴或选择输出位置。
7. 第一版聚焦 CPU 数据；硬件帧和异步执行后续独立设计。

## 2. 对象划分

| 类别 | 对象 | 职责 |
| --- | --- | --- |
| 数据 | `Packet` | 一份压缩编码数据及其时间信息 |
| 数据 | `VideoFrame` | 像素数据、平面、步长、尺寸和色彩信息 |
| 数据 | `AudioFrame` | 音频样本、平面、采样数、采样率和声道布局 |
| 描述 | `CodecParameters` | 编码格式、解码配置、extradata 和包格式的独立值对象 |
| 输入输出 | `Demuxer` / `Muxer` | 容器和压缩包之间的转换，可选使用 |
| 编解码 | `VideoDecoder` / `AudioDecoder` | 压缩包解码为帧 |
| 编解码 | `VideoEncoder` / `AudioEncoder` | 帧编码为压缩包 |
| 处理 | `VideoProcessor` | 缩放、裁剪、像素格式转换、字幕烧录等空间操作 |
| 处理 | `AudioProcessor` | 重采样、声道转换、采样格式转换、音量等音频操作 |
| 字幕 | `SubtitleTrack` | 有时间信息的文本字幕，可供封装或烧录使用 |
| 便捷封装 | `VideoReader` 等 | 组合已有核心对象，减少固定任务的样板代码 |

不创建统一的 `Node` 基类、插件注册系统或动态端口系统。内部按处理需求组合具体对象。
单独一个 `VideoDecoder` 只依赖解码功能，不依赖文件读取器或处理管道。

```text
网络、摄像头、业务代码 ───────┐
                           ↓
文件 → Demuxer → Packet → Decoder → Frame → Processor → Frame → Encoder → Packet → Muxer → 文件
                  ↓                    ↓                                      ↓
               直接转发           渲染、算法、分析                           自定义网络发送
```

## 3. 先看实际调用

以下代码展示核心接口的使用。`renderer`、`transport`、`capture` 等由应用提供。
配置使用 C++17 写法，不使用指定成员初始化语法。

### 3.1 自定义来源的 H.264 解码后渲染

```cpp
fflite::VideoDecoder decoder(fflite::Codec::H264);

fflite::VideoProcessor video;
video.resize(1280, 720).format(fflite::PixelFormat::RGBA);

auto present = [&](const fflite::VideoFrame& frame) {
    auto rgba = video.process(frame);
    renderer.present(rgba.plane(0).data, rgba.plane(0).stride);
};

// 由应用在收到完整编码包时调用。
auto on_packet = [&](const fflite::Packet& packet) {
    decoder.decode(packet, present);
};

// 应用确认输入结束时调用一次，交付解码器中剩余的帧。
decoder.flush(present);
```

只给出 `Codec::H264` 的构造方式采用已定义的原始码流约定：输入为完整 Annex B 编码包，
参数集在码流内。不将任意 TCP 分片视为完整编码包，也不将 RTP 包视为 H.264 编码包。
来自 MP4、私有协议的长度前缀码流或需要额外解码配置的数据，使用 `CodecParameters` 构造。

### 3.2 从文件读取压缩包，解码指定视频流

```cpp
fflite::Demuxer input("input.mp4");
auto stream = input.video();
fflite::VideoDecoder decoder(stream);

auto consume = [&](const fflite::VideoFrame& frame) {
    analyse(frame);
};

while (auto packet = input.read()) {
    if (packet->stream_id() == stream.id()) {
        decoder.decode(*packet, consume);
    }
}
decoder.flush(consume);
```

`input.video()` 返回默认最佳视频流的完整描述，包含 extradata 和包格式。
`input.read()` 返回 `std::optional<Packet>`，文件结束返回空值，读取失败抛出异常。
多流应用使用 `input.streams()` 枚举，用 `StreamId` 分发，不要求分别打开相同文件。

只想逐帧读视频的应用可以选择便捷对象：

```cpp
fflite::VideoReader reader("input.mp4");
while (auto frame = reader.read()) {
    analyse(*frame);
}
```

`VideoReader` 内部组合 Demuxer 和 VideoDecoder，自动处理 EOF 排空；这是独立的便捷层。

### 3.3 采集原始视频帧，编码后交给自己的网络模块

```cpp
fflite::VideoEncodeOptions options;
options.bitrate = 4'000'000;
options.fps = {30, 1};

fflite::VideoEncoder encoder(fflite::Codec::H264, {1280, 720}, options);

// 参数集及包格式需要按应用协议交给接收端。
transport.configure(encoder.parameters());

auto send = [&](const fflite::Packet& packet) {
    transport.send(packet.data(), packet.size());
};

while (auto frame = capture.read()) {
    encoder.encode(*frame, send);
}
encoder.flush(send);
```

若网络模块异步使用数据，应保留 `Packet` 的副本，不应只保存回调中的裸指针。
H.264 编码器须由本地 FFmpeg 构建提供；不可用时报告错误，不自动换成其他编码格式。
包格式通过编码参数描述，RTP 打包、分片、协议握手由应用或专门的协议组件处理。

### 3.4 处理音频，直接送给音频设备

```cpp
fflite::AudioProcessor audio;
audio.resample(48000, 2)
     .format(fflite::SampleFormat::S16)
     .volume(0.8);

auto play = [&](const fflite::AudioFrame& frame) {
    device.write(frame.plane(0).data, frame.sample_count());
};

while (auto frame = microphone.read()) {
    audio.process(*frame, play);
}
audio.flush(play);
```

`S16` 表示交错排列的 16 位有符号样本；设备需要的字节数可由 `plane(0).size` 获取。
`sample_count()` 表示每声道采样数，不是所有声道相加后的数量。
重采样可能暂存样本，也可能一次交付多个帧，使用回调表达实际的输出数量。

### 3.5 组合裁剪、缩放和字幕烧录

```cpp
fflite::VideoProcessor video;
video.crop(100, 50, 800, 600)
     .resize(640, 480)
     .subtitles(fflite::SubtitleTrack::load("captions.srt"))
     .format(fflite::PixelFormat::RGBA);

auto output = video.process(input_frame);
```

空间操作按调用顺序执行。字幕渲染按帧时间戳选择文本；缺失时间戳时报错。
字幕烧录需要可选文本渲染依赖，未启用时在配置阶段报告能力不可用。
字幕烧录在当前实现中返回 Unavailable；外部字幕轨封装已实现。

## 4. 编解码方法：隐藏收发循环，保留流结束

拟定方法族如下，回调类型可接受 lambda，不要求用户声明特殊接口：

```cpp
decoder.decode(packet, on_frame);
decoder.flush(on_frame);

encoder.encode(frame, on_packet);
encoder.flush(on_packet);
```

- 单次输入可能没有输出，也可能有多个输出；回调只在有结果时调用。
- 方法同步执行，回调发生在调用线程中，返回时本次能够交付的结果已全部交付。
- 内部处理 FFmpeg 的 EAGAIN 和 send/receive 重试，调用方不参与该循环。
- 一次调用完成后，库不再使用传入的 C++ 包装对象；需要延迟处理的数据会保留自己的引用。
- `flush()` 表示正常输入结束，交付剩余结果；重复调用安全，排空后继续输入则报错。
- 解码器的 `reset()` 丢弃延迟输出并清空解码参考状态，保留格式配置，适用于 seek 或断流重启。
  重置后应从可解码的边界继续供包；重置本身不能补回丢失的参考帧。
- 编码器完成后如需新编码会话，创建新对象，避免把编码参数更改混入重置语义。
- 回调抛出的异常原样传播，当前有状态对象进入失败状态，后续需要重建；不会吞掉回调异常或重复交付。

回调接收 `const Frame&` 或 `const Packet&`，引用只保证本次回调期间有效；
需要在回调之外持有结果时，复制值对象即可保留底层数据。

不在析构函数中调用用户回调。析构只释放资源；未调用 `flush()` 可能丢弃延迟结果。
文件便捷封装负责组织其内部对象的排空，让固定任务的使用者不必手写每层的结束步骤。

## 5. 数据对象：可直接访问，也可以安全持有

### 5.1 所有权

`Packet`、`VideoFrame`、`AudioFrame` 是可复制、可移动的值对象。
复制默认只增加数据引用计数；`clone()` 明确请求深拷贝。
内部数据默认只读，通过 `writable_plane()` 请求可写平面；共享数据先执行写时复制。

```cpp
auto retained = frame;                   // 保留引用，不复制像素
auto independent = frame.clone();        // 独立数据
auto plane = independent.writable_plane(0);
process_pixels(plane.data, plane.stride);
```

独立包装对象可以跨线程传递；同一个对象的并发读写由调用方同步。
独占的数据允许复用内存；处理器实现不能让后续输出覆盖应用仍持有的结果。

### 5.2 原始数据接入

同时提供明确命名的复制和托管入口：

```cpp
auto packet = fflite::Packet::copy(data, size);
auto frame = fflite::VideoFrame::allocate(1280, 720, fflite::PixelFormat::RGBA);
auto plane = frame.writable_plane(0);
fill_image(plane.data, plane.stride);
```

`wrap(format, planes, owner)` 用于包装外部内存，必须传入保持内存存活的 owner。
不会提供默认无所有权、却可被异步保留的隐式包装方式。
外部持有者必须保证读取期间不修改或释放数据；需要独立数据时使用 `copy()`。

C++17 没有 `std::span`，使用小型 `ByteView`、`PlaneView` 描述借用数据。
视频平面包含 `data`、有符号 `stride`、每行有效字节数和行数，支持行填充及负步长。
音频平面包含 `data` 和 `size`，支持交错与平面格式。不会假设数据连续或只有一个平面。

### 5.3 拟定数据接口

```cpp
packet.data(); packet.size(); packet.stream_id(); packet.is_keyframe();
packet.pts(); packet.dts(); packet.duration();

video.width(); video.height(); video.format(); video.plane_count(); video.plane(index);
video.pts(); video.duration(); video.color_info();

audio.sample_rate(); audio.channel_layout(); audio.format(); audio.sample_count();
audio.plane_count(); audio.plane(index); audio.pts(); audio.duration();
```

只提供实际语义不同的对象，不要求调用方给 `Frame` 设置媒体类型再手动判断字段是否合法。
`CodecParameters` 必须独立持有 extradata，不能引用已经关闭的 Demuxer 或 Decoder 的内部存储。

## 6. 时间戳：准确表达，自动换算

采用显式的有理数时间，不将公共时间戳统一转换为毫秒或 double：

```cpp
struct Rational { int num; int den; };
struct Timestamp { std::int64_t ticks; Rational time_base; };
```

实际类型提供合法性校验和转换方法，拒绝零或负时间基。
`pts()`、`dts()` 返回 `std::optional<Timestamp>`，缺失值不会伪装成零。
负时间戳合法。时长使用单独的 `Duration` 值对象，避免与绝对时间混淆。

```cpp
frame.set_pts(fflite::Timestamp::from(std::chrono::milliseconds(100)));
auto seconds = frame.pts()->seconds();    // 展示用途
```

核心规则：

- 解码、编码、重采样和封装自动处理相应时间基换算；保持原有时间轴和流间偏移。
- `Muxer::write()` 从包内的时间基换算至目标流时间基，调用方不手写 rescale。
- 有效的输入 PTS 保留，`fps` 配置不能偷偷将已有 VFR 时间轴改为 CFR。
- 原始视频没有时间戳时，编码器可按显式配置的 fps 生成时间戳。
- 原始音频没有时间戳时，可按累计每声道采样数和采样率生成连续时间戳。
- 解码可交付无时间戳帧；需要输出时间轴的操作在既无时间戳、也无法推导时间时报告错误。
- seek、丢包和跳变的业务策略由应用决定；库不默认纠正时钟或丢帧。
- 播放时的等待、音视频时钟同步属于播放器职责，解码方法不为此 sleep。

因此，使用者可以不做时间基换算，但需要在原始数据有真实采集时间时提供该时间。

## 7. 参数：明确、够用、可扩展

### 7.1 构造与配置

常见用途只需要必选项：

```cpp
fflite::VideoDecoder decoder(video_stream);
fflite::VideoEncoder encoder(fflite::Codec::H264, {1280, 720});
fflite::AudioEncoder audio_encoder(fflite::Codec::AAC, fflite::AudioFormat::stereo(48000));
```

可选项使用配置结构，不增加不断增长的位置参数列表：

```cpp
fflite::VideoEncodeOptions options;
options.bitrate = 4'000'000;
options.fps = {30000, 1001};
options.gop_size = 60;
options.max_b_frames = 0;
options.implementation = "libx264";

fflite::VideoEncoder encoder(fflite::Codec::H264, {1280, 720}, options);
```

主要默认值拟定如下：

| 配置 | 默认 |
| --- | --- |
| 编码标准、视频尺寸、音频采样率及布局 | 调用方明确提供 |
| 视频/音频码率 | 分别为 2 Mbps / 128 kbps，可显式设置 |
| 视频 fps | 未指定；保留输入时间戳，需要生成时间戳时必须提供 |
| 编解码线程数 | 自动选择，由库限制资源使用；允许显式指定 |
| 编码实现 | 在匹配标准的软件实现中选择；不自动使用硬件 |
| 输出像素/采样格式 | 从编码器支持的格式中协商；允许显式约束 |
| 缩放模式 | Fit，保持比例并填黑，输出方形像素 |
| 音量 | 1.0，无变化 |
| 日志与执行 | 无全局日志劫持，同步调用 |

`Codec` 表达编码标准，`implementation` 按需指定具体实现。指定实现不匹配该标准时错误。
默认选择可用的软件编码器；不自动改编码标准，也不自动选择硬件编码器。
输入格式由流描述或首个帧推断；输出契约由构造配置确定。

编码器构造后即可取得 `parameters()`，包含封装所需的编码信息。
用于 MP4/MKV 的编码器默认准备全局编码头；包格式属于编码参数的一部分。
其他容器可能需要码流适配，由封装层在已实现的格式范围内处理；无法适配时明确报错。
不能假设任意编码包可直接写入任意容器。

### 7.2 自动处理的范围

库自动管理分配、引用、收发、格式协商及排空中的底层步骤。
编码器可协商或转换像素/采样存储格式；不擅自改变分辨率、采样率或声道布局。
这些属性不匹配时，提示使用对应 Processor，保证配置变化具有可见含义。
需要保持特定像素格式的应用可以通过选项约束协商结果，失败时错误。

`VideoProcessor::resize(w, h)` 默认 Fit：输出尺寸精确为目标尺寸，保持显示比例，空白区域填黑。
可选 `ResizeMode::Fill` 居中裁剪铺满，`ResizeMode::Stretch` 拉伸。
默认输出使用方形像素，避免仅改像素宽高比导致像素尺寸与显示尺寸不一致。

`crop()` 按当前帧坐标工作，区域越界报错。对齐约束依据实际像素格式，
例如 YUV420 的色度采样要求，不给所有 RGB 操作强加偶数限制。

### 7.3 扩展口

先为常见能力提供具名、类型化配置。后续可以增加显式的高级编码器选项字典，
只作为特殊参数的补充；未知或未消费的参数必须报告，不能静默忽略。
若提供原生 FFmpeg 互操作，放进独立的可选头文件 `fflite/ffmpeg.hpp`，
写明借用时长和所有权转移；不让普通调用者依赖该头文件。
高级参数字典和原生互操作不作为第一版基础调用的前置要求。

## 8. 封装和独立字幕

Muxer 接收明确注册的输出流，不根据第一次收到的包猜测媒体类型：

```cpp
fflite::Muxer output("output.mp4");
auto video_out = output.add_stream(encoder.parameters());

auto write = [&](const fflite::Packet& packet) {
    output.write(video_out, packet);
};

// 应用调用 encoder.encode(frame, write) 若干次。
encoder.flush(write);
output.finish();
```

第一个 `write()` 自动提交容器头；此前必须注册完所有输出流。
`finish()` 排空封装队列、写入尾部，并报告最终 I/O 错误；重复调用安全。
Muxer 不持有外部编码器，也不替应用排空外部编码器。
`StreamId` 显式区分输入流与输出流；不同 Muxer 的流标识不可混用。

独立字幕和烧录是两个明确的用途：

- `VideoProcessor::subtitles(track)` 渲染到画面，可设置字体、大小、颜色、位置。
- `Muxer::add_subtitles(track, options)` 加入可开关字幕轨；第一版目标为 SRT→MKV/MP4。

`SubtitleTrack` 包含文本、开始时间和结束时间，不依赖必须先打开视频文件。
轨道封装按容器支持的字幕格式转换；样式能否保留由格式能力决定，不能默默承诺全部保留。
字幕时间轴与视频帧时间轴相同；时间平移通过具名操作表达。

## 9. 状态、错误和性能契约

有状态对象的基本状态：Ready → Running → Drained；任何不可恢复错误进入 Failed。
解码器允许通过显式 reset 从 Running/Drained 回到 Ready；失败对象重建。
可执行对象只移动、不复制；数据和格式描述对象可复制。

错误统一为 `fflite::Error`，至少包含 `code()`、`operation()`、`what()`：
参数错误、能力缺失、无效数据、格式不支持、错误状态、I/O 失败。
底层错误码可以附带，但业务代码不必使用 FFmpeg 错误常量。
EOF 和暂时无输出是正常结果，不使用异常报告。
实时非阻塞输入的 WouldBlock 后续单独设计，不混入首版文件 read 的 EOF。

回调默认同步执行，不持有跨调用的用户回调；内部实现避免要求每次调用分配堆内存。
公开模板入口可经轻量 callback trampoline 调用内部实现，不强制所有回调转换为 std::function。
处理器和编解码器内部复用上下文与缓冲，数据未改变时可复用引用。
不承诺每种处理都零拷贝；缩放和格式转换本身通常需要输出缓冲。

单个执行对象不支持并发调用；不同对象可以由应用分配到不同线程。
库不隐式创建线程队列，不更改进程全局日志回调；日志接入采用显式配置。
输入动态格式变化时，Decoder 输出带新格式的帧，Processor 可重建内部转换器；
如果变化违反 Encoder 已固定的输出配置，则报告错误，不自动修改正在写出的容器流。

## 10. 实现分层与顺序

```text
include/fflite/
  types.hpp        # 时间、格式、流描述、错误
  packet.hpp
  frame.hpp
  decoder.hpp
  encoder.hpp
  processor.hpp
  demuxer.hpp
  muxer.hpp
  subtitle.hpp
  reader.hpp       # 可选便捷层
  fflite.hpp        # 聚合头
src/
  detail/          # FFmpeg 资源封装、错误和时间基转换
  ...              # 对应对象实现；公共头使用 PImpl 隔离 FFmpeg
```

第一阶段：Packet、Frame、CodecParameters、时间、错误，以及独立视频解码和编码。
用自定义内存数据完成解码→处理→编码，证明核心确实独立于文件管道。

第二阶段：音频编解码和 Processor，覆盖重采样残留样本及帧尺寸匹配。
第三阶段：Demuxer、Muxer 和必要的包格式适配，保证多流、extradata 和时间轴正确。
第四阶段：字幕、VideoReader 等便捷对象，直接组合核心对象。

旧 Pipeline 原型已删除，文件便捷对象由核心类组合，不维护第二套编解码逻辑。
当前已完成前三阶段及字幕轨封装、VideoReader；字幕烧录组件尚未启用。

## 11. 验收标准

- 无文件参与，外部内存的编码包能够解码，原始帧能够编码，结果可由应用取得。
- 视频 B 帧、音频延迟和重采样残留不会因简化 API 被遗漏。
- 回调保留的帧或包在下一次调用、原始对象销毁后仍有效。
- 分辨率、stride、平面数量、声道布局、extradata 和包格式均有覆盖。
- 时间戳换算、多流偏移和 VFR 不要求应用手动修正；缺失时间有明确策略。
- 参数错误、回调异常、flush、reset 和错误状态的行为可预测。
- 同一 API 可嵌入应用自己的循环和线程，不依赖内置调度器。
- 常见解码、编码、缩放及重采样示例不出现 AV* 类型、EAGAIN、手动 free 和滤镜字符串。
- CMake 使用者只链接 `fflite::fflite`；普通公共头不要求包含 FFmpeg 头文件。
