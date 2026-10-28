#pragma once
#include "frame.hpp"
namespace fflite {
class VideoReader {
  public:
    explicit VideoReader(const std::string &path);
    ~VideoReader();
    VideoReader(VideoReader &&) noexcept;
    VideoReader &operator=(VideoReader &&) noexcept;
    VideoReader(const VideoReader &) = delete;
    VideoReader &operator=(const VideoReader &) = delete;
    std::optional<VideoFrame> read();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace fflite
