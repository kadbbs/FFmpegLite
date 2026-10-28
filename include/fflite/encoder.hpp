#pragma once
#include "frame.hpp"
#include "packet.hpp"
namespace fflite {
struct VideoEncodeOptions {
    std::int64_t bitrate = 2'000'000;
    std::optional<Rational> fps;
    int gop_size = 60, max_b_frames = 0, threads = 0;
    std::string implementation;
    std::optional<PixelFormat> pixel_format;
};
struct AudioEncodeOptions {
    std::int64_t bitrate = 128'000;
    int threads = 0;
    std::string implementation;
};
class VideoEncoder {
  public:
    VideoEncoder(Codec codec, Size size, VideoEncodeOptions options = {});
    ~VideoEncoder();
    VideoEncoder(VideoEncoder &&) noexcept;
    VideoEncoder &operator=(VideoEncoder &&) noexcept;
    VideoEncoder(const VideoEncoder &) = delete;
    VideoEncoder &operator=(const VideoEncoder &) = delete;
    CodecParameters parameters() const;
    template <class F> void encode(const VideoFrame &frame, F &&f) {
        detail::dispatch<Packet>(f, [&](auto sink) { encode_impl(frame, sink); });
    }
    template <class F> void flush(F &&f) {
        detail::dispatch<Packet>(f, [&](auto sink) { flush_impl(sink); });
    }

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    void encode_impl(const VideoFrame &, detail::Sink<Packet>);
    void flush_impl(detail::Sink<Packet>);
};
class AudioEncoder {
  public:
    AudioEncoder(Codec codec, AudioFormat format, AudioEncodeOptions options = {});
    ~AudioEncoder();
    AudioEncoder(AudioEncoder &&) noexcept;
    AudioEncoder &operator=(AudioEncoder &&) noexcept;
    AudioEncoder(const AudioEncoder &) = delete;
    AudioEncoder &operator=(const AudioEncoder &) = delete;
    CodecParameters parameters() const;
    template <class F> void encode(const AudioFrame &frame, F &&f) {
        detail::dispatch<Packet>(f, [&](auto sink) { encode_impl(frame, sink); });
    }
    template <class F> void flush(F &&f) {
        detail::dispatch<Packet>(f, [&](auto sink) { flush_impl(sink); });
    }

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    void encode_impl(const AudioFrame &, detail::Sink<Packet>);
    void flush_impl(detail::Sink<Packet>);
};
} // namespace fflite
