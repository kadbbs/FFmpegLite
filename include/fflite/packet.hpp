#pragma once
#include "types.hpp"
namespace fflite {
class Packet {
  public:
    Packet();
    static Packet copy(const void *data, std::size_t size);
    const std::uint8_t *data() const;
    std::size_t size() const;
    StreamId stream_id() const;
    bool is_keyframe() const;
    std::optional<Timestamp> pts() const;
    std::optional<Timestamp> dts() const;
    Duration duration() const;
    void set_pts(std::optional<Timestamp> value);
    void set_dts(std::optional<Timestamp> value);
    void set_duration(Duration value);
    void set_keyframe(bool value);
    Packet clone() const;

  private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
    void detach();
    friend struct detail_Access;
};
} // namespace fflite
