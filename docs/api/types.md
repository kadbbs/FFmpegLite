# 类型、格式、时间与错误

[文档首页](../README.md) · 公共头：[types.hpp](../../include/fflite/types.hpp)

所有公开类型位于 `fflite` 命名空间。可以包含 `<fflite/fflite.hpp>`，也可只包含具体头文件。
`detail` 命名空间及 `detail_Access` 是内部实现，不属于应用接口。

## Rational

```cpp
struct Rational {
    int num = 1;
    int den = 1;
    Rational();
    Rational(int numerator, int denominator);
};
```

表示正有理数 `num / den`。默认 1/1，带参构造要求分子、分母均大于零。
时间基和帧率都使用这个类型，但含义由所在字段决定：

```cpp
fflite::Rational time_base{1, 90000}; // 一个 tick 为 1/90000 秒
fflite::Rational fps{30000, 1001};   // 每秒约 29.97 帧
```

不要直接修改公开字段使分子或分母成为零、负数。API 并不提供通用分数运算符。

## Timestamp

```cpp
Timestamp(std::int64_t ticks, Rational time_base);
double seconds() const noexcept;
Timestamp rescaled(Rational time_base) const;
template<class Rep, class Period>
static Timestamp from(std::chrono::duration<Rep, Period> value);
```

时间计算为 `ticks × time_base.num / time_base.den` 秒。
允许负时间戳；`INT64_MIN` 保留给内部缺失时间标记，不能用于构造 Timestamp。

```cpp
fflite::Timestamp a{9000, {1, 90000}}; // 0.1 秒
auto b = a.rescaled({1, 48000});     // ticks 为 4800
auto c = fflite::Timestamp::from(std::chrono::milliseconds(100));
```

`seconds()` 返回 double，适合显示和统计。处理链应保留整数 tick 和时间基。
`rescaled()` 返回新时间戳，不修改原对象；目标时间基可能产生舍入误差。
`from()` 先转换为整数纳秒，需保证传入时长可由 int64 纳秒表示；小于一纳秒的部分会丢失。
不要把一个以毫秒计数的整数直接当成另一种时间基的 ticks。

帧和包的 `pts()` 返回 `std::optional<Timestamp>`：

```cpp
if (auto pts = frame.pts()) {
    std::cout << pts->seconds();
}
frame.set_pts(fflite::Timestamp{100, {1, 1000}});
frame.set_pts(std::nullopt); // 清除时间戳
```

## Duration

```cpp
Duration(std::int64_t ticks, Rational time_base);
double seconds() const noexcept;
```

表示时长，要求 ticks 非负、时间基为正。零时长在包或视频帧中通常表示未知/未设置，
不能据此认为内容不存在。与绝对时间分开使用，避免把“持续 2 秒”误作“第 2 秒”。

```cpp
fflite::Duration frame_duration{1, {1, 25}}; // 40 ms
fflite::Duration audio_duration{1024, {1, 48000}};
```

## 时间戳传递规则

| 情况 | 当前行为 |
| --- | --- |
| 解码输入包的时间基与解码器不同 | 自动换算，原包不修改 |
| 解码结果有可用时间 | 输出最佳估计的呈现时间 |
| 解码结果没有时间 | PTS 可为空，不伪装成零 |
| 视频编码输入有 PTS | 换算到 1/60000 秒，要求严格递增 |
| 视频编码输入没有 PTS | 必须配置 fps，按帧间隔生成，从零或已有累计位置开始 |
| 音频处理/编码输入没有 PTS | 按每声道采样数累计生成 |
| AudioProcessor 输入采样率变化 | 换算累计位置，排空旧转换器后重建 |
| Muxer 写包 | 自动换算到容器最终流时间基 |

视频 fps 不会替换已有 PTS。但当前实现会用 fps 推导编码帧时长；若 fps 未配置，
则参考输入 duration，未知时使用最小编码时间步长。它不是通用 VFR 时长重建器。
极密时间戳换算到 1/60000 秒后可能碰撞并被拒绝。

## 编码与媒体类型

`MediaType`：`Video`、`Audio`、`Subtitle`、`Other`。

| Codec | 含义 |
| --- | --- |
| H264 / HEVC | H.264 / H.265 视频 |
| MPEG4 | MPEG-4 Part 2 视频，不是 MP4 容器 |
| VP8 / VP9 / AV1 | 对应视频编码标准 |
| AAC / Opus / MP3 | 对应音频编码标准 |
| PCM_S16LE | 小端交错 16 位有符号 PCM |
| SubRip / MovText | SRT 文本字幕 / MP4 MOV_TEXT 字幕 |
| Unknown | 公开枚举未映射的格式 |

枚举值存在不代表底层构建包含相应编解码器。默认软件编码实现由库从可用实现中选择，
需要指定时使用编码选项中的 `implementation`。

## 像素和音频存储格式

| PixelFormat | 主要布局 |
| --- | --- |
| YUV420P | Y、U、V 三平面，色度水平和垂直各减半 |
| YUV422P | Y、U、V 三平面，色度水平减半 |
| YUV444P | Y、U、V 三平面，分辨率相同 |
| NV12 | Y 平面和交错 UV 平面 |
| RGB24 | 单平面，每像素 R、G、B 共 3 字节 |
| RGBA / BGRA | 单平面，每像素 4 字节 |
| Gray8 | 单平面，8 位灰度 |
| Unknown | 当前公开枚举未映射的像素格式 |

`SampleFormat` 包含 U8、S16、S32、F32、F64 及其 Planar 版本。
U8 为无符号 8 位，S16/S32 为有符号整数，F32/F64 为浮点。
原始多字节采样格式使用主机原生字节序；`Codec::PCM_S16LE` 则明确指定编码输出为小端。

交错双声道：`L0 R0 L1 R1 ...`。平面双声道：平面 0 为 `L0 L1 ...`，平面 1 为 `R0 R1 ...`。
每声道采样数相同；AudioFrame 的 sample_count 不把声道数再乘进去。

## ChannelLayout / AudioFormat / VideoFormat / Size

```cpp
fflite::ChannelLayout stereo = fflite::ChannelLayout::stereo();
fflite::ChannelLayout mono = fflite::ChannelLayout::mono();
fflite::ChannelLayout surround = fflite::ChannelLayout::named("5.1");

auto audio = fflite::AudioFormat::stereo(48000, fflite::SampleFormat::S16);
fflite::VideoFormat video{1280, 720, fflite::PixelFormat::RGBA};
fflite::Size size{1280, 720};
```

| 类型/成员 | 约定 |
| --- | --- |
| `ChannelLayout(int channels = 2)` | 创建 1..8 声道的默认布局 |
| `ChannelLayout::named(string)` | 解析具名布局；无效名称抛 InvalidArgument |
| `channels()` / `name()` | 返回声道数及布局名称 |
| `AudioFormat` | sample_rate、layout、samples；samples 默认 F32 |
| `AudioFormat::mono/stereo(rate, samples)` | 便捷创建，samples 默认 F32 |
| `VideoFormat` | width、height、pixels；pixels 默认 RGBA |
| `Size` | width、height；默认均为 0，真正分配/编码时需正尺寸 |

AudioEncoder 当前会比较输入与目标的布局名称。仅有相同声道数不代表布局相同；
推荐使用 mono/stereo 工厂或同一个具名布局，避免把不同声道位置当成等价。

`ColorInfo` 当前只公开 `range`，取 `Unspecified`、`Limited`、`Full`。
完整色彩原色、传递函数和 HDR 处理不属于当前公开接口承诺。
`ResizeMode` 见[处理器参考](processors.md)。平面视图字段见[数据参考](data.md)。

## CodecParameters

可复制的编码信息值对象，持有独立有效的 extradata 引用，不依赖 Demuxer 或 Encoder 的存活。

```cpp
CodecParameters();
static CodecParameters video(Codec codec, std::vector<std::uint8_t> extradata = {});
static CodecParameters audio(Codec codec, AudioFormat format,
                             std::vector<std::uint8_t> extradata = {});
```

通常这样获得完整参数：

```cpp
auto parameters = demuxer.video().parameters(); // auto 复制值，避免保留临时对象中的引用
auto encoded_parameters = encoder.parameters();
```

| 查询 | 返回 |
| --- | --- |
| `codec()` | 编码标准；没有公开映射时为 Unknown |
| `media_type()` | 媒体类型 |
| `size()` | 视频宽高，非视频可能为 0 |
| `sample_rate()` | 音频采样率，非音频可能为 0 |
| `channel_layout()` | 音频布局；无布局信息时报 InvalidArgument |
| `extradata()` | 额外配置字节的复制；无配置返回空 vector |

`video()` 工厂只设置编码标准和 extradata，不设置视频尺寸，适用于创建能从码流解析尺寸的解码器。
因此不能直接把它当成完整视频输出流注册到 Muxer；封装需取得带尺寸的编码器/输入流参数。
`audio()` 同时设置采样率、布局和样本格式，适合自定义协议已提供完整音频配置的情况。
extradata 的二进制格式由具体编码标准规定，库不会根据随意的字节自动修正它。
默认构造的对象是空描述，不能直接作为已配置流使用。

## StreamId / StreamInfo

`StreamId` 是不透明流标识，只支持默认构造、相等和不等比较。
默认值不代表有效流；应用不能从整数构造它，也没有 setter 可以任意替换包所属流。

输入包携带产生它的 Demuxer 的标识。输出流由 Muxer 注册，不能把输入标识或另一 Muxer 的标识当成输出标识。

| StreamInfo 方法 | 返回 |
| --- | --- |
| `id()` | 流标识的值 |
| `parameters()` | 内部 CodecParameters 的 const 引用 |
| `media_type()` | 媒体类型 |
| `time_base()` | 容器流时间基的值 |

StreamInfo 自身可复制；参数持有数据，原始输入关闭后仍可使用。
保留 `parameters()` 引用时必须让该 StreamInfo 存活，或直接复制 CodecParameters。

## Error / ErrorCode

`Error` 派生自 `std::runtime_error`，可统一捕获 `std::exception`。

```cpp
try {
    // 音视频处理
} catch (const fflite::Error& error) {
    std::cerr << error.operation() << ": " << error.what() << '\n';
}
```

| 方法 | 含义 |
| --- | --- |
| `code()` | fflite 错误分类 |
| `operation()` | 失败的操作描述 |
| `what()` | 已包含操作描述的可读错误信息 |
| `native_code()` | 底层错误码；库自身检查产生的错误通常为 0 |

如果只需要输出错误，直接打印 `what()` 即可，不必再重复 operation。

| ErrorCode | 常见原因 |
| --- | --- |
| InvalidArgument | 非法尺寸、采样率、时间基、流标识、缺失必要时间戳 |
| Unavailable | 编解码器或资源不可用；字幕烧录未实现 |
| InvalidData | 默认分类的底层编解码/处理错误 |
| Unsupported | 容器或显式请求的格式不支持 |
| InvalidState | 已失败、已排空继续输入、配置时机错误、重入 |
| Io | 打开输入输出、读写包、seek、结束输出失败 |

分类反映当前实现的检查位置；某些 FFmpeg 不支持的配置可能表现为 InvalidData，
不能假定所有底层“格式不支持”都会统一映射为 Unsupported。
内存分配等 C++ 标准设施仍可能抛出标准异常，业务回调异常按原类型传播。
异常后的对象恢复约定见[核心概念](../concepts.md)。
