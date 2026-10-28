#pragma once
#include "packet.hpp"
#include "subtitle.hpp"
namespace fflite {
class Muxer {
  public:
    explicit Muxer(const std::string &path);
    ~Muxer();
    Muxer(Muxer &&) noexcept;
    Muxer &operator=(Muxer &&) noexcept;
    Muxer(const Muxer &) = delete;
    Muxer &operator=(const Muxer &) = delete;
    StreamId add_stream(const CodecParameters &parameters);
    StreamId add_subtitles(const SubtitleTrack &track, SubtitleOptions options = {});
    void write(StreamId stream, const Packet &packet);
    void finish();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace fflite
