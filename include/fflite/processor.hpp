#pragma once
#include "frame.hpp"
#include "subtitle.hpp"
namespace fflite {
class VideoProcessor {
  public:
    VideoProcessor();
    ~VideoProcessor();
    VideoProcessor(VideoProcessor &&) noexcept;
    VideoProcessor &operator=(VideoProcessor &&) noexcept;
    VideoProcessor(const VideoProcessor &) = delete;
    VideoProcessor &operator=(const VideoProcessor &) = delete;
    VideoProcessor &crop(int x, int y, int width, int height);
    VideoProcessor &resize(int width, int height, ResizeMode mode = ResizeMode::Fit);
    VideoProcessor &format(PixelFormat format);
    VideoProcessor &subtitles(const SubtitleTrack &track);
    VideoFrame process(const VideoFrame &frame);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
class AudioProcessor {
  public:
    AudioProcessor();
    ~AudioProcessor();
    AudioProcessor(AudioProcessor &&) noexcept;
    AudioProcessor &operator=(AudioProcessor &&) noexcept;
    AudioProcessor(const AudioProcessor &) = delete;
    AudioProcessor &operator=(const AudioProcessor &) = delete;
    AudioProcessor &resample(int sample_rate, int channels = 0);
    AudioProcessor &format(SampleFormat format);
    AudioProcessor &volume(double gain);
    template <class F> void process(const AudioFrame &frame, F &&f) {
        detail::dispatch<AudioFrame>(f, [&](auto sink) { process_impl(frame, sink); });
    }
    template <class F> void flush(F &&f) {
        detail::dispatch<AudioFrame>(f, [&](auto sink) { flush_impl(sink); });
    }
    void reset();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    void process_impl(const AudioFrame &, detail::Sink<AudioFrame>);
    void flush_impl(detail::Sink<AudioFrame>);
};
} // namespace fflite
