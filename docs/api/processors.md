# 视频与音频处理

[文档首页](../README.md) · 公共头：[processor.hpp](../../include/fflite/processor.hpp)

处理器接收原始帧，不处理压缩 Packet。它们独立于文件输入输出和编解码器。
两个类都只能移动、不能复制。

## VideoProcessor

```cpp
VideoProcessor();
VideoProcessor& crop(int x, int y, int width, int height);
VideoProcessor& resize(int width, int height, ResizeMode mode = ResizeMode::Fit);
VideoProcessor& format(PixelFormat format);
VideoProcessor& subtitles(const SubtitleTrack& track);
VideoFrame process(const VideoFrame& frame);
```

构造为空操作集合。crop、resize、format 返回自身引用，便于链式配置。
每调用一次就追加一个操作，按顺序执行；首次 process 后不能再修改配置。
空操作集合可以返回共享数据引用的帧，没有强制复制像素。

```cpp
fflite::VideoProcessor video;
video.crop(100, 50, 800, 600)
     .resize(640, 480)
     .format(fflite::PixelFormat::RGBA);
auto output = video.process(frame);
```

### crop(x, y, width, height)

坐标原点是当前帧左上角，x 向右、y 向下。x/y 非负，width/height 为正。
区域须完全位于执行到该步骤的画面内，越界在 process 时报告 InvalidArgument。

例如先 resize 到 640×480，再 crop(100, 50, 400, 300)，裁剪坐标属于 640×480 的结果。
反过来先 crop 再 resize 会得到不同内容。

对 RGB/RGBA，可以使用奇数坐标和尺寸。色度子采样格式的 x/y 需对齐：

| 格式 | x 对齐 | y 对齐 |
| --- | --- | --- |
| YUV420P / NV12 | 2 | 2 |
| YUV422P | 2 | 1 |
| YUV444P / RGB / RGBA | 1 | 1 |

当前 crop 校验的是坐标色度对齐；后续编码器还可能要求偶数输出尺寸。
如需对任意像素区域裁剪，可先 format(RGBA) 再 crop。
裁剪可通过共享原图像内存的视图完成，输出的 stride 不一定等于裁剪后宽度乘字节数。

### resize(width, height, mode)

目标宽高范围为 1..32768。默认输出方形像素，实际结果尺寸精确等于目标。
插值使用当前实现的双线性算法，没有公开插值选项。

以 1920×1080 缩放到 640×480 为例：

| 模式 | 结果 |
| --- | --- |
| Fit，默认 | 图像约 640×360，居中，上下各约 60 像素黑边 |
| Fill | 画面铺满 640×480，居中裁掉左右超出部分 |
| Stretch | 直接拉伸到 640×480，可能改变显示比例 |

Fit/Fill 会考虑输入的像素宽高比；整数像素取整可能使两侧边框差一个像素。
Fill 的中间图像也受到尺寸上限约束，极端比例可能报错。
resize 不代表改帧率，也不会改变 PTS。

### format(PixelFormat)

追加像素存储格式转换。Unknown 不可作为目标。尺寸不变，已有时间属性随结果传递。
转换可能产生新的内存；重复的格式转换不是零成本操作。
当前没有完整 HDR 或色彩管理配置，不能把通用格式转换当成完整的 HDR 到 SDR 映射。

### subtitles(track)

这是字幕烧录接口的预留位置。当前所有调用都在配置时抛 `ErrorCode::Unavailable`，
不提供启用开关，也不能仅靠给 FFmpeg 打开 libass 就认为此接口已有实现。
需要可开关字幕轨时使用 [Muxer::add_subtitles](io.md)。

### process(frame)

同步返回一张结果帧，不修改输入，保留结果的应用不会被后续处理覆盖。
可以处理输入尺寸/格式变化，但固定 crop 区域仍须在新画面范围内。
VideoProcessor 没有 flush/reset；运行失败后重建，想改变操作集合时也重建。

## AudioProcessor

```cpp
AudioProcessor();
AudioProcessor& resample(int sample_rate, int channels = 0);
AudioProcessor& format(SampleFormat format);
AudioProcessor& volume(double gain);
template<class F> void process(const AudioFrame& frame, F&& on_audio);
template<class F> void flush(F&& on_audio);
void reset();
```

配置方法设置目标参数，并非逐项追加有顺序的处理节点。
重复 resample、format、volume 分别采用最后一次设置。
参数在首次 process 前固定；正常 reset 后可以重新配置。

```cpp
fflite::AudioProcessor audio;
audio.resample(44100, 2).format(fflite::SampleFormat::S16).volume(0.5);
```

### resample(sample_rate, channels)

sample_rate 为正；channels 为 0..8。channels=0 保留输入布局，否则使用该声道数的默认布局。
同时完成采样率和声道转换，不是简单复制、删除或交错原始字节。

不调用 resample 时保留输入采样率和布局。输入格式变化时排空旧转换器，再为新格式建立转换器。
转换器重建不能替代输出设备或编码器的重新配置：下游仍须接受实际输出格式。

### format(SampleFormat)

设置输出样本的存储方式，包括整数/浮点、交错/平面。Unknown 不可作为目标。
未设置时保留输入存储格式。

例如双声道 F32Planar 有两个 float 平面，双声道 S16 有一个交错的 int16 平面。
转换后应重新读取 plane_count 和 size，不要沿用输入帧的平面信息。

### volume(gain)

线性振幅倍率，默认 1，要求有限且非负。

| gain | 含义 |
| --- | --- |
| 0 | 静音 |
| 0.5 | 振幅减半，约 -6.02 dB |
| 1 | 振幅不变 |
| 2 | 振幅加倍，约 +6.02 dB；可能削波 |

它不是以 dB 为单位的接口，也不是自动归一化或响度标准化。
整数存储格式转换时可能发生舍入，不能要求每个转换结果都保持位级一致。

### process(frame, on_audio)

回调接受 `const AudioFrame&`，可产生零个或多个结果。
输入 PTS 有效时保留其时间位置并换算；缺失时从零或累计采样位置生成。
不自行填补业务音频缺口、校正设备时钟或等待播放时间。
每帧输出的采样数可能与输入不同，应使用输出的 sample_count。

### flush(on_audio)

输入真正结束时排空重采样尾部。成功后重复调用安全，不再交付相同样本。
排空后继续 process 报 InvalidState；如需下一段输入，正常 reset 或重建。
若结果送入 AudioEncoder，应先 flush 处理器，让所有样本进入编码器，再 flush 编码器。

### reset()

丢弃重采样内部残留及累计时间，不交付旧结果；保留目标参数，允许重新设置配置。
例如切换采集源或从新时间位置重新开始。已经 Failed 的对象需要重建。
