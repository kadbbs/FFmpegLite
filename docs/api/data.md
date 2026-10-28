# 压缩包与原始帧

[文档首页](../README.md) · 公共头：[packet.hpp](../../include/fflite/packet.hpp)、[frame.hpp](../../include/fflite/frame.hpp)

## 共同约定

Packet、VideoFrame、AudioFrame 可以按值复制和移动，自动管理内存。
复制共享数据，clone 深拷贝；帧的 writable_plane 按需执行写时复制。
元数据 setter 修改当前包装对象，不会修改已经复制出去的其他包装对象。

分配的帧内存没有“默认黑色/静音”的初始化保证。应用应填充全部有效数据后再编码或播放。
移动后的源对象只应销毁或重新赋值，不应继续调用数据查询方法。

## Packet

### 创建

```cpp
fflite::Packet empty;
auto packet = fflite::Packet::copy(encoded_bytes, byte_count);
```

默认对象没有数据和有效时间戳。`copy(const void*, size_t)` 拷贝外部编码数据，要求指针非空、
长度大于零且不超过 int 可表示范围。内部会准备解码所需的缓冲尾部空间。
没有 Packet::wrap 或公开的包数据可写入口。

解码器拒绝空包，结束输入请用 flush。编码器和 Demuxer 返回的 Packet 已具有相应数据及元信息。

### 查询和修改

| 方法 | 行为 |
| --- | --- |
| `data() const` | 压缩数据首地址，只读 |
| `size() const` | 有效字节数 |
| `stream_id() const` | 输入来源流；应用复制/编码产生的包通常没有有效输入流标识 |
| `is_keyframe() const` | 包的关键帧标记，不等于检查码流内容 |
| `pts()/dts() const` | optional 时间戳 |
| `duration() const` | 非负时长；没有有效时长时为零 |
| `set_pts(optional<Timestamp>)` | 设置或清除呈现时间 |
| `set_dts(optional<Timestamp>)` | 设置或清除解码时间 |
| `set_duration(Duration)` | 设置包时长，自动换算到当前包时间基 |
| `set_keyframe(bool)` | 修改标记，不改变编码数据，也不能把普通帧变成关键帧 |
| `clone() const` | 深拷贝数据并保留元信息 |

PTS/DTS setter 可改变当前包时间基，同时换算已经存在的另一时间戳和 duration。
因此读出时不应假定 ticks 和设置时完全相同，应结合返回的 time_base 判断时间。

```cpp
auto packet = fflite::Packet::copy(bytes, size);
packet.set_pts(fflite::Timestamp{9000, {1, 90000}});
packet.set_dts(fflite::Timestamp{100, {1, 1000}});
packet.set_duration(fflite::Duration{40, {1, 1000}});
```

这段代码中的 PTS 和 DTS 都为 100 ms。存在 B 帧时应保留真实 DTS，不应为了满足封装接口随意令 DTS=PTS。

## VideoFrame

### 创建与分配

```cpp
auto frame = fflite::VideoFrame::allocate(1280, 720, fflite::PixelFormat::RGBA);
```

宽高必须为正，格式不能 Unknown。VideoFrame 没有公开默认构造函数，必须由 allocate、wrap、
解码器、处理器或已有帧的复制取得。分配结果初始 PTS 为空，视频 duration 为零。

RGBA 填充示例：

```cpp
auto plane = frame.writable_plane(0);
for (int y = 0; y < plane.rows; ++y) {
    auto* row = plane.data + y * plane.stride;
    for (int x = 0; x < frame.width(); ++x) {
        row[4*x + 0] = 255; // R
        row[4*x + 1] = 0;   // G
        row[4*x + 2] = 0;   // B
        row[4*x + 3] = 255; // A，不透明
    }
}
```

### 方法

| 方法 | 返回或行为 |
| --- | --- |
| `width()/height()` | 当前像素尺寸 |
| `format()` | 公开像素格式，未映射时 Unknown |
| `plane_count()` | 像素平面数量 |
| `plane(index)` | 指定平面的只读借用视图，索引从 0 开始 |
| `writable_plane(index)` | 指定平面的可写视图，必要时复制共享数据 |
| `pts()` | optional 呈现时间 |
| `set_pts(optional<Timestamp>)` | 设置/清除 PTS，设置新时间基时换算已有 duration |
| `duration()` | 视频帧时长，零可能表示未知 |
| `set_duration(Duration)` | 按当前帧时间基换算后设置时长 |
| `color_info()` | 当前只提供色彩范围信息 |
| `clone()` | 深拷贝像素并保留元信息 |

不能通过修改宽高字段把一帧“变成另一尺寸”，请使用 VideoProcessor。
不要假定编码前后像素完全一致，MPEG4/AAC 等有损编码会改变结果数据。

## PlaneView / MutablePlaneView

| 字段 | 视频含义 | 音频含义 |
| --- | --- | --- |
| `data` | 逻辑第一行首地址 | 当前样本平面首地址 |
| `stride` | 相邻行起点的有符号字节距离 | 当前有效平面的字节大小 |
| `size` | 覆盖有效图像行的内存跨度 | 当前平面的有效字节数 |
| `row_bytes` | 每行实际像素字节数，不含行填充 | 与 size 相同，按单行视图表达 |
| `rows` | 当前平面的行数 | 1 |

`MutablePlaneView` 的 data 是可写指针，其余字段语义相同。
对视频，size 的跨度按 `abs(stride) * (rows - 1) + row_bytes` 计算。
负 stride 时，data 位于内存中的高地址行，不能从 data 开始直接读取 size 个字节；应逐行访问。

```cpp
for (int y = 0; y < plane.rows; ++y) {
    const auto* row = plane.data + y * plane.stride;
    // 只访问这一行的 row_bytes 个有效字节。
}
```

YUV420P 中 Y 为平面 0，U/V 为平面 1/2；色度平面通常比亮度平面小。
NV12 为 Y 和交错 UV 两个平面。必须按 format 和每个平面的实际字段处理。

这些视图不拥有内存。在持有对象析构、重新赋值，或可写访问触发重新分配后，旧视图可能失效。
需要保留数据时保留 Frame 对象，并在使用前重新取得视图。

## 包装外部视频内存

```cpp
auto storage = std::make_shared<std::vector<std::uint8_t>>(640 * 480 * 4);
fflite::PlaneView plane{storage->data(), 640 * 4, storage->size(), 640 * 4, 480};
auto frame = fflite::VideoFrame::wrap(
    {640, 480, fflite::PixelFormat::RGBA}, {plane}, storage);
```

`wrap(VideoFormat, vector<PlaneView>, shared_ptr<void> owner)` 不复制像素，保留 owner。
默认把包装内存视为只读；调用 writable_plane 会复制到库管理的可写内存，不在外部缓冲上直接写。

要求：

- owner 非空，并实际保持所有平面背后的内存存活。
- 平面数量与像素格式一致，当前最多 8 个平面。
- data 指向逻辑第一行；stride 可正可负，绝对值至少为有效行宽。
- size 足够覆盖全部行及其间距；负 stride 的 size 表示整个 backing span，而不是 data 之后的字节数。
- wrap 按 VideoFormat 和 stride 推导所需行数/行宽，不依赖调用方填写的 rows/row_bytes 作为几何配置。

在库或其他消费者读取期间，外部生产者不得复用或修改这些内存。
不能仅用一个不相关的 shared_ptr 充当 owner 来包装即将失效的栈数组。

## AudioFrame

```cpp
auto frame = fflite::AudioFrame::allocate(
    fflite::AudioFormat::stereo(48000, fflite::SampleFormat::S16), 1024);
```

采样率和每声道采样数都必须大于零。上例时长为 `1024 / 48000` 秒，
不是 `2048 / 48000` 秒；交错 S16 双声道的有效数据为 `1024 × 2 × 2 = 4096` 字节。

| 方法 | 返回或行为 |
| --- | --- |
| `sample_rate()` | 每秒、每声道采样数 |
| `channel_layout()` | 当前声道布局的值 |
| `format()` | 采样存储格式 |
| `sample_count()` | 当前帧每声道的采样数 |
| `plane_count()` | 交错格式为 1，平面格式为声道数 |
| `plane()/writable_plane()` | 音频平面的只读/可写借用视图 |
| `pts()/set_pts()` | 当前音频段的起始呈现时间 |
| `duration()` | 根据 sample_count/sample_rate 计算的时长 |
| `clone()` | 深拷贝所有有效样本和元信息 |

AudioFrame 的 duration 根据实际样本计算，没有 set_duration。
S16 静音可逐平面填零；U8 静音的中点为 128，不能把所有音频格式都简单视为零字节静音。

## 包装外部音频内存

```cpp
auto storage = std::make_shared<std::vector<std::uint8_t>>(1024 * 2 * 2);
fflite::PlaneView plane{storage->data(), 4096, 4096, 4096, 1};
auto frame = fflite::AudioFrame::wrap(
    fflite::AudioFormat::stereo(48000, fflite::SampleFormat::S16),
    1024, {plane}, storage);
```

每平面必须包含连续有效样本，当前不支持带额外采样间距的稀疏音频布局。
平面格式需逐声道提供平面；wrap 最多 8 个平面。
size 必须覆盖按格式、声道数和 sample_count 算出的字节数。
owner、只读包装和写时复制规则与视频相同。
