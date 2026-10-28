# 容器、字幕与便捷读取

[文档首页](../README.md) · 公共头：[demuxer.hpp](../../include/fflite/demuxer.hpp)、[muxer.hpp](../../include/fflite/muxer.hpp)、[subtitle.hpp](../../include/fflite/subtitle.hpp)、[reader.hpp](../../include/fflite/reader.hpp)

## Demuxer

```cpp
explicit Demuxer(const std::string& path);
std::vector<StreamInfo> streams() const;
StreamInfo video() const;
StreamInfo audio() const;
std::optional<Packet> read();
void seek(Timestamp position);
```

打开输入并读取流信息。不能复制，可移动。输入格式支持依赖本地 FFmpeg 构建。
首版主要验证本地文件，不承诺通用实时网络和非阻塞输入行为。

| 方法 | 行为 |
| --- | --- |
| streams | 返回所有流描述的值，包括 Other；应用自行决定处理哪些流 |
| video | 返回首选视频流；不存在时报 Unavailable |
| audio | 返回首选音频流；不存在时报 Unavailable |
| read | 返回下一包，包含来源 StreamId 和时间信息；不是只读视频 |
| seek | 定位到绝对媒体时间，恢复后续 read；不是精确逐帧切割 |

read 返回空 optional 表示 EOF，之后重复 read 仍为空。I/O 失败抛异常，不能把异常当成 EOF。
包和已复制的流参数在 Demuxer 销毁后仍可使用。

多流分发：

```cpp
auto video = input.video();
auto audio = input.audio(); // 仅适用于已知有音频的输入
while (auto packet = input.read()) {
    if (packet->stream_id() == video.id()) {
        video_decoder.decode(*packet, on_video);
    } else if (packet->stream_id() == audio.id()) {
        audio_decoder.decode(*packet, on_audio);
    }
}
video_decoder.flush(on_video);
audio_decoder.flush(on_audio);
```

不确定有没有音频时，先从 streams 枚举并按 media_type 判断，避免直接 audio()。
seek 的位置使用输入媒体本身的时间轴，不自动加减首包偏移。
底层通常落在可解码的附近位置，若业务要求精确到某时刻，需继续解码并按帧 PTS 丢弃之前结果。
seek 后应用同时 reset 解码器和相关音频处理器，避免混入旧位置的缓冲数据。

## Muxer

```cpp
explicit Muxer(const std::string& path);
StreamId add_stream(const CodecParameters& parameters);
StreamId add_subtitles(const SubtitleTrack& track, SubtitleOptions options = {});
void write(StreamId stream, const Packet& packet);
void finish();
```

按输出路径选择容器，不能复制，可移动。构造时检查路径和容器，但首次 write/finish 才真正打开输出并写头。
拒绝覆盖已存在的文件；没有 overwrite 选项。操作失败可能留下部分输出，需要应用决定如何处置。

### add_stream(parameters)

复制完整编码参数，返回该 Muxer 的输出流标识。
仅支持视频、音频、字幕；视频需正尺寸，音频需正采样率和声道信息。
推荐使用 Encoder::parameters() 或 Demuxer 取得的流参数。

所有输出流必须在首次 write 前注册。参数是否与容器兼容会在注册或写头时检查。
不支持编解码的容器不能通过“仅改扩展名”强行兼容；某些包还需要尚未提供的码流适配。

```cpp
fflite::Muxer output("output.mp4");
auto target = output.add_stream(encoder.parameters());
auto write = [&](const fflite::Packet& packet) { output.write(target, packet); };
```

target 不等于输入包的 stream_id，不能传入另一个 Muxer 的 ID。

### write(stream, packet)

写一个非空压缩包。当前要求 PTS、DTS 都存在；自动换算时间基，不修改输入 Packet。
应用负责给每条流提供合理、合法的包顺序。封装器执行交错写入，但不会修复乱序协议流。
存在 B 帧时不要按 PTS 重新排序编码器输出包，也不要把真实 DTS 覆盖为 PTS。

原始输入缺失时间时，需要由应用决定生成策略；Muxer 不根据包大小或到达时间猜测时钟。
write 不会调用解码或编码，因此复制原始压缩数据的转封装不会自行改变编码格式。

### finish()

必要时写头、排空已注册的静态字幕、写尾并关闭输出，报告最终 I/O 错误。
成功后重复调用安全，之后 write 或添加流会报 InvalidState。
它不排空应用持有的 Encoder；必须先 encoder.flush，再 output.finish。
析构只关闭资源，不保证补写合法容器尾部。

## SubtitleCue / SubtitleTrack

```cpp
struct SubtitleCue {
    Timestamp start;
    Duration duration;
    std::string text;
};

static SubtitleTrack SubtitleTrack::load(const std::string& path);
void SubtitleTrack::add(SubtitleCue cue);
const std::vector<SubtitleCue>& SubtitleTrack::cues() const noexcept;
```

字幕轨可默认构造并按值复制。add 要求文本非空、duration 正数，内部按开始时间排序。
不要求打开视频文件才能创建字幕，也不会自动重新编号、翻译或调整字幕时间。

```cpp
fflite::SubtitleTrack track;
track.add({fflite::Timestamp{200, {1, 1000}},
           fflite::Duration{1500, {1, 1000}}, "你好，世界"});
```

这句字幕从 0.2 秒开始，持续 1.5 秒。start 是位置，duration 是长度。
load 读取首条 SubRip 字幕流，常见用途为 SRT 文件。没有该格式的轨道时报 Unsupported，
无时间戳、空文本或非法时长也可能导致失败。当前不是通用 ASS/位图字幕加载器。
cues 返回借用引用，后续 add 可能使其中元素引用/指针失效。

## Muxer::add_subtitles 与 SubtitleOptions

```cpp
auto track = fflite::SubtitleTrack::load("captions.srt");
output.add_subtitles(track, {"zho", "中文字幕"});
```

| SubtitleOptions 字段 | 默认 | 含义 |
| --- | --- | --- |
| language | `"und"` | 未指定语言；可设置 zho、eng 等语言标记 |
| title | 空 | 显示标题，容器或播放器未必展示 |

MKV 中写入 SRT，MP4 自动转换为 MOV_TEXT；不支持这些格式的容器会报 Unsupported。
调用时复制并预编码全部字幕，之后修改原始 SubtitleTrack 不改变已经注册的输出内容。
它是静态字幕轨接口，不提供开始写出后持续追加实时字幕的 API。

字幕 PTS/时长写入毫秒级时间基，亚毫秒精度不能保证；建议每条字幕至少持续 1 ms。
单条转换后的 MOV_TEXT 数据使用最大 1 MiB 缓冲，过大将报错。
字幕时间与视频时间使用同一时间轴，不自动根据非零输入起点平移。
这是可选择字幕轨，不是画面烧录；VideoProcessor::subtitles 当前未实现。

## VideoReader

```cpp
explicit VideoReader(const std::string& path);
std::optional<VideoFrame> read();
```

组合 Demuxer 和 VideoDecoder，读取首选视频流，自动忽略其他流并在 EOF 排空解码器。
不保留/输出音频或字幕，不创建播放窗口，不按 PTS 等待。

```cpp
fflite::VideoReader input("input.mp4");
while (auto frame = input.read()) {
    // frame 为 optional<VideoFrame>，*frame 是真实帧对象。
}
```

read 返回空表示所有延迟视频帧也已交付。重复 EOF read 仍为空。
读取或解码失败抛异常；没有公开 seek/reset/流选择参数，复杂场景使用 Demuxer + Decoder。
