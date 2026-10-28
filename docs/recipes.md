# 组合用法

[文档首页](README.md)

本页展示对象怎样组合。业务相关的 analyse、renderer 等名称表示应用自己的代码，
不是 fflite 额外提供的接口。带 main 的完整程序位于 [examples/](examples/)。

## 1. 逐帧读取视频给算法

只需要原始视频帧时：

```cpp
fflite::VideoReader input("input.mp4");
while (auto frame = input.read()) {
    analyse(*frame);
}
```

VideoReader 自动排空延迟帧，只读取首选视频，忽略音频和字幕。
算法需要 RGBA 时增加处理器：

```cpp
fflite::VideoReader input("input.mp4");
fflite::VideoProcessor video;
video.resize(640, 480).format(fflite::PixelFormat::RGBA);
while (auto frame = input.read()) {
    auto rgba = video.process(*frame);
    analyse(rgba);
}
```

若 analyse 只是异步排队，队列中保留 VideoFrame 的值对象，不能只保留平面裸指针。

## 2. 应用自己的网络包解码

```cpp
fflite::VideoDecoder decoder(fflite::Codec::H264);
auto on_frame = [&](const fflite::VideoFrame& frame) {
    renderer.submit(frame);
};
auto on_encoded_data = [&](const void* data, std::size_t size,
                           fflite::Timestamp presentation_time) {
    auto packet = fflite::Packet::copy(data, size);
    packet.set_pts(presentation_time);
    decoder.decode(packet, on_frame);
};
// 连接正常结束且不会再收到这段输入时：
decoder.flush(on_frame);
```

on_encoded_data 是供应用网络模块调用的回调，需要在 flush 之前实际调用。
输入须是完整且含初始化参数集的 Annex B 编码包；长度前缀格式等情况使用完整 CodecParameters。
这个示例不实现协议组包、重传、丢帧恢复或显示时钟。
如果协议提供真实 DTS，应同样传递；仅解码与封装对时间戳的要求不同，不能把此片段直接当作通用转封装代码。

## 3. 转封装音视频，不重新编码

下面的函数可直接放进包含 `<fflite/fflite.hpp>` 的程序。
要求目标容器支持源编码和包格式，且输入包提供 PTS/DTS。

```cpp
void remux_av(const std::string& source, const std::string& destination) {
    fflite::Demuxer input(source);
    fflite::Muxer output(destination);
    std::vector<std::pair<fflite::StreamId, fflite::StreamId>> mapping;

    for (const auto& stream : input.streams()) {
        if (stream.media_type() == fflite::MediaType::Video ||
            stream.media_type() == fflite::MediaType::Audio) {
            mapping.push_back({stream.id(), output.add_stream(stream.parameters())});
        }
    }
    while (auto packet = input.read()) {
        for (const auto& pair : mapping) {
            if (packet->stream_id() == pair.first) {
                output.write(pair.second, *packet);
                break;
            }
        }
    }
    output.finish();
}
```

这里没有 Decoder 或 Encoder。mapping 左边是输入 ID，右边是输出 ID。
时间基自动换算。函数只保留音视频数据流，不声称复制章节、附件、所有元数据或输入字幕。
编码格式不兼容时应选择转码，而不是仅更换文件后缀。

## 4. 解码、缩放、编码并保存

完整程序：[file_transcode.cpp](examples/file_transcode.cpp)。

数据流是：

```text
Demuxer → VideoDecoder → VideoProcessor → VideoEncoder → Muxer
```

主要回调按结果类型连接：

```cpp
auto on_packet = [&](const fflite::Packet& packet) {
    output.write(target_stream, packet);
};
auto on_frame = [&](const fflite::VideoFrame& frame) {
    auto processed = video.process(frame);
    encoder.encode(processed, on_packet);
};
```

这里假定 output、target_stream、video、encoder 已创建并配置。
然后将输入视频包送给 decoder，结束顺序为：

```cpp
decoder.flush(on_frame);
encoder.flush(on_packet);
output.finish();
```

文档程序只转首选视频，不保留音频。不要在业务中默认认为“视频转码函数会自动保留所有流”。
输入帧率也不是由输出 fps 设置强制重采样；保留的输入 PTS 继续决定时间位置。

## 5. 生成原始帧并录制

完整程序：[encode_file.cpp](examples/encode_file.cpp)。
应用可把生成图片替换为摄像头结果，或使用 VideoFrame::wrap 接入采集缓冲。
编码器尺寸与输入尺寸一致；如果不一致，先用 VideoProcessor。
没有采集 PTS 时配置 fps；有真实采集时间时用 set_pts 设置，不以函数调用耗时替代媒体时间。

这个程序只录制视频，不创建音频轨，也不启动摄像头。

## 6. 音频重采样后编码

完整程序：[audio_memory.cpp](examples/audio_memory.cpp)。

```text
48 kHz 单声道 PCM → AudioProcessor → 44.1 kHz 双声道 → AudioEncoder → AAC 包
```

先由处理器改变采样率和布局，编码器再匹配编码帧长与样本存储格式。
应用可一次提交任意正采样数的帧，不需要先手动凑成 AAC 固定帧长。

```cpp
audio.flush(on_processed_audio);
encoder.flush(on_encoded_packet);
```

顺序不能反过来。最终音频包可交给自己的网络模块，或注册好音频流的 Muxer。

## 7. 加入可开关字幕轨

```cpp
fflite::Muxer output("with-subtitles.mp4");
auto video_target = output.add_stream(encoder.parameters());
auto subtitles = fflite::SubtitleTrack::load("captions.srt");
output.add_subtitles(subtitles, {"zho", "中文字幕"});
// 现在才可以开始 output.write(video_target, packet)。
```

这段代码假定 encoder 已配置。字幕使用和视频相同的时间轴，输入非零起点不会自动被减掉。
字幕必须在首次 write 前注册；生成器开始输出包以后，不能临时添加新的字幕流。
烧录到画面的 API 当前不可用。

## 8. seek 后继续解码

```cpp
input.seek(fflite::Timestamp{10, {1, 1}}); // 媒体绝对时间 10 秒
decoder.reset();
// 有相关 AudioProcessor 时同时 reset；如果要交付旧数据，须在 seek 前先正常排空。
```

这里假定 input、decoder 已存在。seek 不保证返回的首帧正好等于目标时间。
继续解码，在回调里检查 frame.pts，按业务需要丢弃目标之前的帧。
VideoReader 没有 seek 接口，需要此控制时使用 Demuxer + Decoder。

## 9. 常见错误定位

| 现象 | 检查 |
| --- | --- |
| 最后少了几帧或少了一段声音 | 上游到下游是否都正常 flush |
| 容器不能播放或尾部损坏 | 编码器是否排空、Muxer 是否 finish |
| 输出文件已存在错误 | 选择新路径，或由业务明确管理旧文件 |
| H.264 编码器不可用 | 本地 FFmpeg 是否包含软件编码实现及链接依赖 |
| Missing PTS requires an explicit fps | 输入提供真实 PTS，或构造时配置 fps |
| PTS must increase | 时间戳是否乱序、重复，或换算到 1/60000 秒后碰撞 |
| Rate/layout differ | 先重采样/转换声道，并使用一致布局名称 |
| Unknown output stream / different source | 是否误用了输入流 ID 或其他 Muxer 的 ID |
| 音频数据听起来不正确 | 交错/平面、每声道采样数、设备期望格式是否一致 |
| 图片错行或越界 | 是否按每个 plane 的 stride/row_bytes/rows 访问 |
| 回调后偶尔出现坏图 | 是否只保存裸指针而没有保留 Frame |
| 已失败对象继续调用报 InvalidState | 捕获异常后重建对象，不反复重试同一个 Failed 对象 |
