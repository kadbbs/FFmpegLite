#pragma once
#include <fflite/fflite.hpp>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
namespace fflite::detail {
[[noreturn]] void fail(ErrorCode code, const char *operation, const std::string &message);
void check(int result, const char *operation, ErrorCode code = ErrorCode::InvalidData);
void require(bool condition, const char *operation, const std::string &message);
AVRational av(Rational value);
Rational rational(AVRational value);
AVCodecID av(Codec value);
Codec codec(AVCodecID value);
AVPixelFormat av(PixelFormat value);
PixelFormat pixels(int value);
AVSampleFormat av(SampleFormat value);
SampleFormat samples(int value);
MediaType media(AVMediaType value);
ChannelLayout layout(const AVChannelLayout &value);
void set_layout(AVChannelLayout &out, const ChannelLayout &value);
std::optional<Timestamp> timestamp(std::int64_t value, AVRational base);
std::int64_t ticks(const Timestamp &value, AVRational base);
std::uint64_t identity();
int threads(int requested);
struct FrameFree {
    void operator()(AVFrame *p) const { av_frame_free(&p); }
};
struct PacketFree {
    void operator()(AVPacket *p) const { av_packet_free(&p); }
};
struct ParamsFree {
    void operator()(AVCodecParameters *p) const { avcodec_parameters_free(&p); }
};
struct CodecFree {
    void operator()(AVCodecContext *p) const { avcodec_free_context(&p); }
};
struct GraphFree {
    void operator()(AVFilterGraph *p) const { avfilter_graph_free(&p); }
};
struct InputFree {
    void operator()(AVFormatContext *p) const { avformat_close_input(&p); }
};
struct OutputFree {
    void operator()(AVFormatContext *p) const {
        if (p->pb)
            avio_closep(&p->pb);
        avformat_free_context(p);
    }
};
using FramePtr = std::unique_ptr<AVFrame, FrameFree>;
using PacketPtr = std::unique_ptr<AVPacket, PacketFree>;
using ParamsPtr = std::unique_ptr<AVCodecParameters, ParamsFree>;
using CodecPtr = std::unique_ptr<AVCodecContext, CodecFree>;
FramePtr frame();
PacketPtr packet();
ParamsPtr params();
CodecPtr context(const AVCodec *codec);
FramePtr ref(const AVFrame *frame);
PacketPtr ref(const AVPacket *packet);
const AVCodec *encoder(Codec codec, const std::string &implementation);
enum class State { Ready, Running, Drained, Failed };
struct Runtime {
    State state = State::Ready;
    bool active = false;
    template <class F> void invoke(F &&f) {
        if (active || state == State::Failed)
            fail(ErrorCode::InvalidState, "runtime", "Object is active or failed");
        active = true;
        try {
            f();
            active = false;
        } catch (...) {
            state = State::Failed;
            active = false;
            throw;
        }
    }
    void input() {
        if (state == State::Drained)
            fail(ErrorCode::InvalidState, "input", "Object has been drained");
        state = State::Running;
    }
};
class AudioGraph {
  public:
    AudioGraph(const AVFrame &input, AVRational input_base, const AudioFormat &target, double gain,
               int frame_size = 0);
    void push(AVFrame *frame);
    int receive(AVFrame *frame);
    AVRational time_base() const;
    bool matches(const AVFrame &frame) const;

  private:
    std::unique_ptr<AVFilterGraph, GraphFree> graph_;
    AVFilterContext *source_ = nullptr;
    AVFilterContext *sink_ = nullptr;
    int rate_, format_;
    ChannelLayout layout_;
};
} // namespace fflite::detail
namespace fflite {
struct CodecParameters::Impl {
    detail::ParamsPtr data = detail::params();
};
struct Packet::Impl {
    detail::PacketPtr data = detail::packet();
    StreamId stream;
};
struct VideoFrame::Impl {
    detail::FramePtr data;
    explicit Impl(detail::FramePtr f) : data(std::move(f)) {}
};
struct AudioFrame::Impl {
    detail::FramePtr data;
    explicit Impl(detail::FramePtr f) : data(std::move(f)) {}
};
struct detail_Access {
    static AVFrame *frame(const VideoFrame &f) { return f.impl_->data.get(); }
    static AVFrame *frame(const AudioFrame &f) { return f.impl_->data.get(); }
    static AVPacket *packet(const Packet &p) { return p.impl_->data.get(); }
    static AVCodecParameters *params(const CodecParameters &p) { return p.impl_->data.get(); }
    static VideoFrame video(detail::FramePtr f) {
        return VideoFrame(std::make_shared<VideoFrame::Impl>(std::move(f)));
    }
    static AudioFrame audio(detail::FramePtr f) {
        return AudioFrame(std::make_shared<AudioFrame::Impl>(std::move(f)));
    }
    static Packet packet(detail::PacketPtr f, StreamId stream = {}) {
        Packet out;
        out.impl_->data = std::move(f);
        out.impl_->stream = stream;
        return out;
    }
    static CodecParameters params(const AVCodecParameters *p) {
        CodecParameters out;
        detail::check(avcodec_parameters_copy(params(out), p), "Copy codec parameters");
        return out;
    }
    static CodecParameters params(const AVCodecContext *p) {
        CodecParameters out;
        detail::check(avcodec_parameters_from_context(params(out), p), "Export codec parameters");
        return out;
    }
    static StreamId id(std::uint64_t owner, int index) {
        StreamId id;
        id.owner_ = owner;
        id.index_ = index;
        return id;
    }
    static int index(StreamId id, std::uint64_t owner) {
        if (id.owner_ != owner || id.index_ < 0)
            detail::fail(ErrorCode::InvalidArgument, "stream",
                         "Stream belongs to a different source/destination");
        return id.index_;
    }
    static StreamInfo stream(std::uint64_t owner, AVStream *s) {
        StreamInfo out;
        out.id_ = id(owner, s->index);
        out.parameters_ = params(s->codecpar);
        out.time_base_ = detail::rational(s->time_base);
        return out;
    }
};
} // namespace fflite
