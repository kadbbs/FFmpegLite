# 编码与解码

[文档首页](../README.md) · 公共头：[decoder.hpp](../../include/fflite/decoder.hpp)、[encoder.hpp](../../include/fflite/encoder.hpp)

四种编解码器都只能移动、不能复制。构造时打开相应编解码上下文，资源由析构自动释放。
调用和回调同步执行，内部处理 send/receive 和暂时无输出的情况。
结果数量、所有权和状态规则见[核心概念](../concepts.md)。

## VideoDecoder / AudioDecoder

两个类的方法相同，仅输出帧类型不同：

```cpp
explicit VideoDecoder(Codec codec, DecodeOptions options = {});
explicit VideoDecoder(const CodecParameters& parameters, DecodeOptions options = {});
explicit VideoDecoder(const StreamInfo& stream, DecodeOptions options = {});

template<class F> void decode(const Packet& packet, F&& on_frame);
template<class F> void flush(F&& on_frame);
void reset();
```

AudioDecoder 提供相同三个构造重载。视频回调接受 `const VideoFrame&`，音频回调接受 `const AudioFrame&`。

### 如何选择构造方式

| 已有数据 | 推荐方式 |
| --- | --- |
| Demuxer 读取的容器数据 | `VideoDecoder(input.video())` / `AudioDecoder(input.audio())` |
| 自定义协议已经提供完整编码配置 | 传入 CodecParameters |
| 含参数集的完整 H.264 Annex B 编码包 | 可以只指定 Codec::H264 |
| AAC 等需要额外配置的裸包 | 显式提供音频参数及必要 extradata，不要仅猜 Codec |

StreamInfo 构造会保留输入流时间基用于解码；仅 Codec/CodecParameters 构造采用内部默认时间基，
输入包会自动换算。使用者不需要手动改写包时间。
音视频类型不匹配会被拒绝。

### DecodeOptions

| 字段 | 默认 | 约束 |
| --- | --- | --- |
| threads | 0 | 非负；0 自动选择，当前限制为 1..8 个逻辑工作线程的请求值 |

正数按请求传给 FFmpeg；编解码器最终如何利用线程取决于具体实现。
应用回调仍在调用 decode/flush 的线程执行。

### decode(packet, on_frame)

提交一个完整编码包，交付当前可以取得的全部解码帧。方法不修改输入 Packet。
不保证一包一帧，也不保证本次提交立即产生结果。
无结果时回调不会执行，方法正常返回。

```cpp
fflite::Demuxer input("input.mp4");
auto stream = input.video();
fflite::VideoDecoder decoder(stream);
auto consume = [&](const fflite::VideoFrame& frame) {
    // 在此调用算法、渲染器，或复制 frame 到应用队列。
};
while (auto packet = input.read()) {
    if (packet->stream_id() == stream.id()) decoder.decode(*packet, consume);
}
decoder.flush(consume);
```

输出 PTS 使用解码器的最佳估计呈现时间；无可用时间时保留为空。
Decoder 不根据 Packet::stream_id 自动筛选，流分发由应用负责。
不能把多个不相关视频流混送到同一个解码器。

### flush(on_frame)

结束当前输入，提交 EOF 并交付延迟帧。成功后再次 flush 不会重复交付。
成功排空后 decode 抛 InvalidState；如需新的一段输入，正常 reset 或重新构造。
不要用默认空 Packet 代替 flush。

### reset()

丢弃尚未交付的帧及参考状态，保留格式配置，回到可输入状态。
适用于 seek、放弃当前片段后从可解码边界重启。不会调用结果回调。
运行失败或回调异常后的对象不可 reset，需重建。

## VideoEncoder

```cpp
VideoEncoder(Codec codec, Size size, VideoEncodeOptions options = {});
CodecParameters parameters() const;
template<class F> void encode(const VideoFrame& frame, F&& on_packet);
template<class F> void flush(F&& on_packet);
```

### 构造和选项

```cpp
fflite::VideoEncodeOptions options;
options.fps = {25, 1};
options.bitrate = 2'000'000;
options.max_b_frames = 2;
fflite::VideoEncoder encoder(fflite::Codec::MPEG4, {96, 64}, options);
```

| 字段 | 默认 | 含义及约束 |
| --- | --- | --- |
| bitrate | 2,000,000 | 每秒 bit，正数；实际码率受具体编码器影响 |
| fps | 未设置 | 正有理数帧率；无输入 PTS 时必须设置 |
| gop_size | 60 | 关键帧间隔配置，正整数 |
| max_b_frames | 0 | B 帧数量配置，非负；大于零可能增加延迟 |
| threads | 0 | 非负；0 自动，当前自动请求上限为 8 |
| implementation | 空字符串 | 自动选择软件实现；指定时必须存在且匹配 codec |
| pixel_format | 未设置 | 优先 YUV420P，不支持时在编码器支持列表中协商 |

显式 pixel_format 不受支持时抛 Unsupported，而不是改用另一格式。
协商到公开 API 尚未映射的像素格式时当前会拒绝构造。
视频尺寸必须为正，并符合实际编码像素格式的色度采样对齐要求；YUV420P 通常要求偶数宽高。

库不会因为 H.264 编码器缺失就改用 MPEG4。默认 FFmpeg 配置不提供 libx264 之类外部编码器。
实现名如 `libx264` 是具体编码器名称，而 `Codec::H264` 是编码标准。

### encode(frame, on_packet)

回调类型为 `void(const Packet&)`。
输入宽高必须与构造时一致；像素存储格式可以自动转换。
需要改变尺寸时先使用 VideoProcessor，不会在编码时偷偷缩放。

```cpp
auto collect = [&](const fflite::Packet& packet) {
    encoded_packets.push_back(packet); // encoded_packets 为 vector<Packet>
};
encoder.encode(frame, collect);
```

输入不被修改。具有 PTS 的帧按实际时间换算，要求在 1/60000 秒编码时间基上严格递增。
没有 PTS 时必须配置 fps，库从零或已有累计时间位置生成下一个 PTS。
为同一段输入持续提供真实时间，或持续使用自动时钟，通常比混合缺失/有效 PTS 更容易控制。

编码包可能因帧间预测而延迟，PTS 与 DTS 可能不同。
编码输出包没有 Demuxer 来源流 ID，写入 Muxer 时显式传入输出流 ID。
音视频包格式及参数集是协议的一部分；仅把 data/size 发到接收端不能替代传递编码配置。

### parameters()

返回可独立持有的 CodecParameters，构造后、首次 encode 前即可调用。
用于注册容器流、创建对应解码器或通过应用协议传给接收端。
当前编码器准备全局编码头，主要覆盖 MP4/MKV；不保证任意输出容器都能直接接收这些包。

### flush(on_packet)

结束并交付所有延迟编码包。重复成功调用安全。没有 reset，需要新编码会话时重建对象。
flush 不会结束外部 Muxer，应在包全部交付后调用 Muxer::finish。

## AudioEncoder

```cpp
AudioEncoder(Codec codec, AudioFormat format, AudioEncodeOptions options = {});
CodecParameters parameters() const;
template<class F> void encode(const AudioFrame& frame, F&& on_packet);
template<class F> void flush(F&& on_packet);
```

构造中的 AudioFormat 定义目标采样率、声道布局和偏好的存储格式。
构造后同样可以立即取得 parameters。

```cpp
fflite::AudioEncodeOptions options;
options.bitrate = 128'000;
fflite::AudioEncoder encoder(
    fflite::Codec::AAC, fflite::AudioFormat::stereo(48000), options);
```

| 选项 | 默认 | 约束 |
| --- | --- | --- |
| bitrate | 128,000 | 每秒 bit，正数；PCM 等实现可能不使用该目标值 |
| threads | 0 | 非负；自动请求最多 8 |
| implementation | 空字符串 | 自动软件实现，或显式匹配标准的实现名 |

目标采样率和布局须被编码器支持。偏好采样存储格式不支持时，可以自动协商其他格式。
encode 接受的帧必须匹配目标采样率和布局；同声道数但不同布局名称也可能被拒绝。
如需转换，先使用 AudioProcessor。

编码器自动完成采样存储格式转换和编码帧长匹配，不要求每个输入 AudioFrame 都有编码器固定的样本数。
输入 PTS 有效则保留并换算；缺失时从零或累计位置生成连续时间。
不会自动修复业务时间跳变、时钟漂移或缺失的音频片段。

编码结束先排空音频处理器，再排空 AudioEncoder。AAC 等编码器可能有起始编码延迟和尾部填充，
有损音频编解码后的样本总数不能简单假定与原始帧的数量完全一样。

## 错误与回调规则

- 支持 lambda、普通函数、函数指针及可调用对象，不要求显式 std::function。
- 回调返回值不用于取消。需要业务中断时，在调用方法的外层控制下一次输入。
- 回调抛异常会原样传播，并使当前运行对象 Failed；需要重建。
- 已 Failed 的对象、重入同一个对象、排空后继续输入都会报 InvalidState。
- 解码器只提供结果，不做音视频同步播放或按 PTS 自动等待。

完整的内存编码解码过程见 [memory.cpp 详解](../tutorials/memory.md)。
