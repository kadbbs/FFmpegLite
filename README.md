# FFmpegLite

C++17 音视频基础库，使用 CMake 管理，本地编译并直接链接 FFmpeg。
开发者控制数据来源、处理步骤和调用节奏，库负责资源、数据引用、编解码收发和时间基转换。
公共接口不包含 FFmpeg 类型，可以直接接入应用自己的网络、采集、渲染或算法模块。

整体架构及接口约定见 [DESIGN.md](DESIGN.md)。当前是核心接口的首版实现。

完整使用指南、API 参考和示例逐行解释见 [docs/README.md](docs/README.md)。

## 解码应用自己的数据

```cpp
#include <fflite/fflite.hpp>

fflite::VideoDecoder decoder(fflite::Codec::H264);
fflite::VideoProcessor video;
video.resize(1280, 720).format(fflite::PixelFormat::RGBA);

auto consume = [&](const fflite::VideoFrame& frame) {
    auto rgba = video.process(frame);
    renderer.present(rgba.plane(0).data, rgba.plane(0).stride);
};

// 应用收到完整编码包时调用。
decoder.decode(packet, consume);

// 输入结束时调用，交付延迟帧。
decoder.flush(consume);
```

只指定 `Codec::H264` 时输入应为含参数集的完整 Annex B 编码包。原始 TCP 分片或 RTP 包
需要由应用先组包。需要 extradata 的格式可传入 `CodecParameters`；文件来源可直接传入
`Demuxer::video()` 返回的流描述。H.264 解码器需要包含在本地 FFmpeg 构建中。

## 原始帧编码

```cpp
fflite::VideoEncodeOptions options;
options.fps = {30, 1};
options.bitrate = 4'000'000;

fflite::VideoEncoder encoder(fflite::Codec::H264, {1280, 720}, options);
auto send = [&](const fflite::Packet& packet) {
    transport.send(packet.data(), packet.size());
};

// 编码参数/参数集通过应用协议交给接收端。
transport.configure(encoder.parameters());
encoder.encode(frame, send);
encoder.flush(send);
```

视频输入尺寸应与编码器配置一致，像素存储格式可自动转换。视频缺失 PTS 时，必须显式
配置 fps；已有 PTS 时保留时间轴。指定的编码格式或实现不可用时明确报告错误。

## 图像和音频处理

```cpp
fflite::VideoProcessor video;
video.crop(100, 50, 800, 600)
     .resize(640, 480)
     .format(fflite::PixelFormat::RGBA);
auto processed = video.process(frame);

fflite::AudioProcessor audio;
audio.resample(48000, 2)
     .format(fflite::SampleFormat::S16)
     .volume(0.8);
auto play = [&](const fflite::AudioFrame& output) {
    device.write(output.plane(0).data, output.sample_count());
};
audio.process(audio_frame, play);
audio.flush(play);
```

视频操作按调用顺序执行。缩放默认 Fit，保留显示比例、填黑、输出方形像素；另有
`ResizeMode::Fill` 和 `Stretch`。裁剪限制依据实际像素格式，RGB 可以使用奇数坐标。
处理器在首次处理前配置，开始后不能追加操作；音频处理器可 reset 后重新配置。

音频 `sample_count()` 是每声道采样数；`plane(0).size` 可取得平面的有效字节数。
采样率变化产生的剩余样本由 flush 交付。AudioEncoder 自动匹配编码器帧长、转换采样
存储格式；采样率和声道布局不同则提示先使用 AudioProcessor。

## 可选的容器接口

```cpp
fflite::Demuxer input("input.mp4");
auto stream = input.video();
fflite::VideoDecoder decoder(stream);
while (auto packet = input.read()) {
    if (packet->stream_id() == stream.id()) decoder.decode(*packet, consume);
}
decoder.flush(consume);

// 只需要逐帧读取时，也可使用组合好的便捷对象。
fflite::VideoReader reader("input.mp4");
while (auto frame = reader.read()) analyse(*frame);
```

写出压缩包时显式注册输出流，时间基自动转换：

```cpp
fflite::Muxer output("output.mp4");
auto stream = output.add_stream(encoder.parameters());
auto write = [&](const fflite::Packet& packet) { output.write(stream, packet); };
encoder.encode(frame, write);
encoder.flush(write);
output.finish();
```

所有输出流必须在第一次 write 前注册。输出 StreamId 只能用于创建它的 Muxer。
finish 写入尾部并报告最终 I/O 错误，不会替应用排空外部编码器。已有输出文件会被拒绝。
输入 seek 使用绝对媒体时间戳；应用应同时 reset 关联解码器，并从可解码边界继续供包。

可单独加载、创建并封装文本字幕：

```cpp
auto captions = fflite::SubtitleTrack::load("captions.srt");
output.add_subtitles(captions, {"zho", "中文字幕"}); // 首次 write 之前
```

MKV 使用 SRT，MP4 自动转为 MOV_TEXT。当前没有启用字幕烧录组件，调用
`VideoProcessor::subtitles()` 会在配置时返回 `ErrorCode::Unavailable`。

## 数据、回调和错误

- Packet、VideoFrame、AudioFrame 和 CodecParameters 可复制并独立持有数据引用。
  `clone()` 深拷贝，`writable_plane()` 按需写时复制；元数据修改也会独立。
- `allocate()` 创建原始帧，`Packet::copy()` 接入编码包；`Frame::wrap()` 接入外部内存时
  必须提供 owner。视频支持带填充行及负 stride。外部所有者应保证数据不被并发修改。
- 结果回调同步执行，返回后如需保留数据，复制帧/包对象即可。支持 lambda、函数和可调用
  对象；不要求 std::function，不保存回调，不创建应用线程。
- 编解码与音频处理的 flush 可重复调用。排空后继续输入报错。析构只释放资源，不执行回调。
- 回调异常原样传播，相关有状态对象进入失败状态，需要重建。解码器正常工作或排空后
  可 reset；失败对象不允许 reset。
- pts/dts 为 `optional<Timestamp>`；Timestamp 含整数 ticks 和有理数时间基，缺失不等于零。
  例：`frame.set_pts(fflite::Timestamp(9000, {1, 90000}))`。
- 错误统一为 `fflite::Error`，包含 `code()`、`operation()`、`what()` 和可选底层错误码。
  EOF 返回空 optional，暂时没有编解码输出时不调用回调。
- 同一执行对象的调用需要串行；独立对象和已保留的数据可由应用分配到自己的线程。

## 构建与集成

需要 CMake 3.20+、C/C++17 编译器、GNU Make、POSIX shell 和 FFmpeg 7.1.1 源码。
当前已验证 Linux；原生 Windows/MSVC 尚未适配。

源码默认位于 `third_party/ffmpeg`，构建阶段不自动下载。当前工作区已准备该源码，
该目录被 gitignore 忽略；新工作副本可自行准备同一版本：

```sh
curl -fL https://ffmpeg.org/releases/ffmpeg-7.1.1.tar.xz -o ffmpeg-7.1.1.tar.xz
# SHA-256: 733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1
mkdir -p third_party
tar -xJf ffmpeg-7.1.1.tar.xz -C third_party
mv third_party/ffmpeg-7.1.1 third_party/ffmpeg

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
ctest --test-dir build --output-on-failure
./build/fflite_memory
```

也可使用 `-DFFLITE_FFMPEG_SOURCE_DIR=/path/to/ffmpeg` 指定已有未配置源码。
CMake 调用 FFmpeg 自带 configure/Makefile，产物分别放在 `build/ffmpeg-build` 和
`build/ffmpeg-install`。默认不检测外部库、不依赖 nasm/yasm。fflite 不调用 ffmpeg CLI。

精简构建可减少初次编译时间，也与本次测试使用的组件一致：

```sh
cmake -S . -B build-minimal \
  '-DFFLITE_FFMPEG_CONFIGURE_ARGS=--disable-everything;--enable-encoder=mpeg4,pcm_s16le,aac,movtext;--enable-decoder=mpeg4,pcm_s16le,aac,subrip,movtext;--enable-demuxer=matroska,mov,avi,srt;--enable-muxer=matroska,mp4,avi;--enable-protocol=file;--enable-parser=mpeg4video,aac'
cmake --build build-minimal -j2
ctest --test-dir build-minimal --output-on-failure
```

精简配置没有 H.264。默认配置包含 H.264 解码器，但 H.264 软件编码需要额外提供编码器
（例如 libx264），并配置对应依赖的编译及静态链接。不启用编码器时，构造会明确报错。
组件范围和最终分发方式应遵守对应 FFmpeg 组件的许可证。

嵌入自己的 CMake 项目：

```cmake
add_subdirectory(path/to/FFmpegLite)
target_link_libraries(my_app PRIVATE fflite::fflite)
```

可用 `-DFFLITE_BUILD_EXAMPLES=OFF -DBUILD_TESTING=OFF` 关闭示例及测试。

测试仅使用公共头和 fflite 目标，覆盖纯内存视频编解码、B 帧排空、原始数据包装、引用
生命周期、写时复制、时间戳、空间操作、重采样尾部、音量、音频编解码、多流封装、
MKV/MP4 字幕、seek、VideoReader 和错误/回调状态。

当前尚无硬件帧、协议组包、非阻塞 I/O、原生 FFmpeg 互操作头或高级编码器参数字典。
音频 wrap 最多 8 个平面。编码器、容器和格式支持以本地构建为准；复杂码流自动适配、
HDR/色彩管理和动态容器重配置尚未完整实现。错误可能留下不完整输出文件。

## 许可证

FFmpegLite 自有代码、示例和文档采用 [MIT 协议](LICENSE)。本项目仅发布自有源码，
不附带 FFmpeg 或其他第三方依赖的源码与二进制；使用者自行获取和构建依赖。
第三方依赖保留各自许可证。使用者自行配置额外 FFmpeg 组件时，应按实际构建和
分发情况履行对应义务，详见[第三方依赖与许可边界](THIRD_PARTY_NOTICES.md)。
