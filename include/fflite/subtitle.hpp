#pragma once
#include "types.hpp"
namespace fflite {
struct SubtitleCue {
    Timestamp start;
    Duration duration;
    std::string text;
};
class SubtitleTrack {
  public:
    static SubtitleTrack load(const std::string &path);
    void add(SubtitleCue cue);
    const std::vector<SubtitleCue> &cues() const noexcept { return cues_; }

  private:
    std::vector<SubtitleCue> cues_;
};
struct SubtitleOptions {
    std::string language = "und";
    std::string title;
};
} // namespace fflite
