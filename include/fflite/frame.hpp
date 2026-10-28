#pragma once
#include "types.hpp"
namespace fflite {
class VideoFrame {
  public:
    static VideoFrame allocate(int width, int height, PixelFormat format);
    static VideoFrame wrap(VideoFormat format, const std::vector<PlaneView> &planes,
                           std::shared_ptr<void> owner);
    int width() const;
    int height() const;
    PixelFormat format() const;
    int plane_count() const;
    PlaneView plane(int index) const;
    MutablePlaneView writable_plane(int index);
    std::optional<Timestamp> pts() const;
    Duration duration() const;
    void set_pts(std::optional<Timestamp> value);
    void set_duration(Duration value);
    ColorInfo color_info() const;
    VideoFrame clone() const;

  private:
    struct Impl;
    explicit VideoFrame(std::shared_ptr<Impl> impl);
    std::shared_ptr<Impl> impl_;
    void detach();
    friend struct detail_Access;
};
class AudioFrame {
  public:
    static AudioFrame allocate(AudioFormat format, int sample_count);
    static AudioFrame wrap(AudioFormat format, int sample_count,
                           const std::vector<PlaneView> &planes, std::shared_ptr<void> owner);
    int sample_rate() const;
    ChannelLayout channel_layout() const;
    SampleFormat format() const;
    int sample_count() const;
    int plane_count() const;
    PlaneView plane(int index) const;
    MutablePlaneView writable_plane(int index);
    std::optional<Timestamp> pts() const;
    Duration duration() const;
    void set_pts(std::optional<Timestamp> value);
    AudioFrame clone() const;

  private:
    struct Impl;
    explicit AudioFrame(std::shared_ptr<Impl> impl);
    std::shared_ptr<Impl> impl_;
    void detach();
    friend struct detail_Access;
};
} // namespace fflite
