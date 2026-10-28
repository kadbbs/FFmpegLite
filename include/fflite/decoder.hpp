#pragma once
#include "frame.hpp"
#include "packet.hpp"
namespace fflite {
struct DecodeOptions {
    int threads = 0;
};
#define FFLITE_DECODER(Name, Frame)                                                                 \
    class Name {                                                                                   \
      public:                                                                                      \
        explicit Name(Codec codec, DecodeOptions options = {});                                    \
        explicit Name(const CodecParameters &parameters, DecodeOptions options = {});              \
        explicit Name(const StreamInfo &stream, DecodeOptions options = {});                       \
        ~Name();                                                                                   \
        Name(Name &&) noexcept;                                                                    \
        Name &operator=(Name &&) noexcept;                                                         \
        Name(const Name &) = delete;                                                               \
        Name &operator=(const Name &) = delete;                                                    \
        template <class F> void decode(const Packet &packet, F &&f) {                              \
            detail::dispatch<Frame>(f, [&](auto sink) { decode_impl(packet, sink); });             \
        }                                                                                          \
        template <class F> void flush(F &&f) {                                                     \
            detail::dispatch<Frame>(f, [&](auto sink) { flush_impl(sink); });                      \
        }                                                                                          \
        void reset();                                                                              \
                                                                                                   \
      private:                                                                                     \
        struct Impl;                                                                               \
        std::unique_ptr<Impl> impl_;                                                               \
        void decode_impl(const Packet &, detail::Sink<Frame>);                                     \
        void flush_impl(detail::Sink<Frame>);                                                      \
    };
FFLITE_DECODER(VideoDecoder, VideoFrame)
FFLITE_DECODER(AudioDecoder, AudioFrame)
#undef FFLITE_DECODER
} // namespace fflite
