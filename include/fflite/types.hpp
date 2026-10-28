#pragma once
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>
namespace fflite {
enum class ErrorCode { InvalidArgument, Unavailable, InvalidData, Unsupported, InvalidState, Io };
class Error : public std::runtime_error {
  public:
    Error(ErrorCode code, std::string operation, std::string message, int native_code = 0);
    ErrorCode code() const noexcept { return code_; }
    const std::string &operation() const noexcept { return operation_; }
    int native_code() const noexcept { return native_; }

  private:
    ErrorCode code_;
    std::string operation_;
    int native_;
};
struct Rational {
    int num = 1, den = 1;
    Rational() = default;
    Rational(int numerator, int denominator);
};
struct Timestamp {
    std::int64_t ticks;
    Rational time_base;
    Timestamp(std::int64_t value, Rational base);
    double seconds() const noexcept;
    Timestamp rescaled(Rational base) const;
    template <class Rep, class Period>
    static Timestamp from(std::chrono::duration<Rep, Period> value) {
        return Timestamp(std::chrono::duration_cast<std::chrono::nanoseconds>(value).count(),
                         {1, 1'000'000'000});
    }
};
struct Duration {
    std::int64_t ticks;
    Rational time_base;
    Duration(std::int64_t value, Rational base);
    double seconds() const noexcept;
};
enum class Codec {
    H264,
    HEVC,
    MPEG4,
    VP8,
    VP9,
    AV1,
    AAC,
    Opus,
    MP3,
    PCM_S16LE,
    SubRip,
    MovText,
    Unknown
};
enum class MediaType { Video, Audio, Subtitle, Other };
enum class PixelFormat { YUV420P, YUV422P, YUV444P, NV12, RGB24, RGBA, BGRA, Gray8, Unknown };
enum class SampleFormat {
    U8,
    S16,
    S32,
    F32,
    F64,
    U8Planar,
    S16Planar,
    S32Planar,
    F32Planar,
    F64Planar,
    Unknown
};
enum class ResizeMode { Fit, Fill, Stretch };
enum class ColorRange { Unspecified, Limited, Full };
struct ColorInfo {
    ColorRange range = ColorRange::Unspecified;
};
struct Size {
    int width = 0, height = 0;
};
struct PlaneView {
    const std::uint8_t *data;
    std::ptrdiff_t stride;
    std::size_t size;
    int row_bytes, rows;
};
struct MutablePlaneView {
    std::uint8_t *data;
    std::ptrdiff_t stride;
    std::size_t size;
    int row_bytes, rows;
};
struct VideoFormat {
    int width, height;
    PixelFormat pixels = PixelFormat::RGBA;
};
class ChannelLayout {
  public:
    explicit ChannelLayout(int channels = 2);
    static ChannelLayout mono() { return ChannelLayout(1); }
    static ChannelLayout stereo() { return ChannelLayout(2); }
    int channels() const noexcept { return channels_; }
    const std::string &name() const noexcept { return name_; }
    static ChannelLayout named(std::string name);

  private:
    int channels_;
    std::string name_;
};
struct AudioFormat {
    int sample_rate;
    ChannelLayout layout;
    SampleFormat samples = SampleFormat::F32;
    static AudioFormat stereo(int rate, SampleFormat samples = SampleFormat::F32) {
        return {rate, ChannelLayout::stereo(), samples};
    }
    static AudioFormat mono(int rate, SampleFormat samples = SampleFormat::F32) {
        return {rate, ChannelLayout::mono(), samples};
    }
};
class StreamId {
  public:
    StreamId() = default;
    bool operator==(StreamId other) const noexcept {
        return owner_ == other.owner_ && index_ == other.index_;
    }
    bool operator!=(StreamId other) const noexcept { return !(*this == other); }

  private:
    std::uint64_t owner_ = 0;
    int index_ = -1;
    friend struct detail_Access;
};
class CodecParameters {
  public:
    CodecParameters();
    static CodecParameters video(Codec codec, std::vector<std::uint8_t> extradata = {});
    static CodecParameters audio(Codec codec, AudioFormat format,
                                 std::vector<std::uint8_t> extradata = {});
    Codec codec() const;
    MediaType media_type() const;
    Size size() const;
    int sample_rate() const;
    ChannelLayout channel_layout() const;
    std::vector<std::uint8_t> extradata() const;

  private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
    friend struct detail_Access;
};
class StreamInfo {
  public:
    StreamId id() const { return id_; }
    const CodecParameters &parameters() const { return parameters_; }
    MediaType media_type() const { return parameters_.media_type(); }
    Rational time_base() const { return time_base_; }

  private:
    StreamId id_;
    CodecParameters parameters_;
    Rational time_base_;
    friend struct detail_Access;
};
namespace detail {
template <class T> struct Sink {
    void *object;
    void (*call)(void *, const T &);
    void operator()(const T &value) const { call(object, value); }
};
template <class T, class F> Sink<T> sink(F &f) {
    return {const_cast<void *>(static_cast<const void *>(std::addressof(f))),
            [](void *p, const T &v) { (*static_cast<F *>(p))(v); }};
}
template <class T, class F, class Run> void dispatch(F &f, Run &&run) {
    auto callback = [&](const T &value) { f(value); };
    run(sink<T>(callback));
}
} // namespace detail
} // namespace fflite
