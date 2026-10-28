#pragma once
#include "packet.hpp"
namespace fflite {
class Demuxer {
  public:
    explicit Demuxer(const std::string &path);
    ~Demuxer();
    Demuxer(Demuxer &&) noexcept;
    Demuxer &operator=(Demuxer &&) noexcept;
    Demuxer(const Demuxer &) = delete;
    Demuxer &operator=(const Demuxer &) = delete;
    std::vector<StreamInfo> streams() const;
    StreamInfo video() const;
    StreamInfo audio() const;
    std::optional<Packet> read();
    void seek(Timestamp position);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace fflite
