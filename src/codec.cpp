#include "detail.hpp"
namespace fflite {
using namespace detail;
namespace {
struct DecoderRuntime : Runtime {
    CodecPtr context;
    DecoderRuntime(const CodecParameters &parameters, AVMediaType type, DecodeOptions options,
                   AVRational base = {1, 1000000})
        : context(
              detail::context(avcodec_find_decoder(detail_Access::params(parameters)->codec_id))) {
        require(detail_Access::params(parameters)->codec_type == type, "decoder",
                "Codec media type mismatch");
        check(avcodec_parameters_to_context(context.get(), detail_Access::params(parameters)),
              "Configure decoder");
        context->thread_count = threads(options.threads);
        context->pkt_timebase = base;
        check(avcodec_open2(context.get(), context->codec, nullptr), "Open decoder");
    }
    template <class F> void drain(F sink) {
        auto f = frame();
        int result;
        while ((result = avcodec_receive_frame(context.get(), f.get())) >= 0) {
            f->pts = f->best_effort_timestamp;
            f->time_base = context->pkt_timebase;
            if (f->sample_rate > 0 && f->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC) {
                const int count = f->ch_layout.nb_channels;
                av_channel_layout_uninit(&f->ch_layout);
                av_channel_layout_default(&f->ch_layout, count);
            }
            sink(std::move(f));
            f = frame();
        }
        if (result != AVERROR(EAGAIN) && result != AVERROR_EOF)
            check(result, "Decode frame");
    }
    template <class F> void send(const Packet *input, F sink) {
        invoke([&] {
            if (!input && state == State::Drained)
                return;
            if (input) {
                this->input();
                require(input->size() > 0, "decode", "Empty packet is not EOF; use flush()");
            }
            auto p = input ? ref(detail_Access::packet(*input)) : PacketPtr{};
            if (p)
                av_packet_rescale_ts(p.get(), av(rational(p->time_base)), context->pkt_timebase);
            int result = avcodec_send_packet(context.get(), p.get());
            if (result == AVERROR(EAGAIN)) {
                drain(sink);
                result = avcodec_send_packet(context.get(), p.get());
            }
            check(result, "Send encoded packet");
            drain(sink);
            if (!input)
                state = State::Drained;
        });
    }
    void reset() {
        invoke([&] {
            avcodec_flush_buffers(context.get());
            state = State::Ready;
        });
    }
};
CodecParameters bare_parameters(Codec id) {
    CodecParameters p;
    auto *raw = detail_Access::params(p);
    raw->codec_id = av(id);
    raw->codec_type = avcodec_get_type(raw->codec_id);
    return p;
}
} // namespace
struct VideoDecoder::Impl : DecoderRuntime {
    using DecoderRuntime::DecoderRuntime;
};
struct AudioDecoder::Impl : DecoderRuntime {
    using DecoderRuntime::DecoderRuntime;
};
#define DECODER(Name, Frame, Type, Wrap)                                                           \
    Name::Name(Codec c, DecodeOptions o) : Name(bare_parameters(c), o) {}                          \
    Name::Name(const CodecParameters &p, DecodeOptions o)                                          \
        : impl_(std::make_unique<Impl>(p, Type, o)) {}                                             \
    Name::Name(const StreamInfo &s, DecodeOptions o)                                               \
        : impl_(std::make_unique<Impl>(s.parameters(), Type, o, av(s.time_base()))) {}             \
    Name::~Name() = default;                                                                       \
    Name::Name(Name &&) noexcept = default;                                                        \
    Name &Name::operator=(Name &&) noexcept = default;                                             \
    void Name::decode_impl(const Packet &p, Sink<Frame> f) {                                       \
        impl_->send(&p, [&](FramePtr out) { f(detail_Access::Wrap(std::move(out))); });            \
    }                                                                                              \
    void Name::flush_impl(Sink<Frame> f) {                                                         \
        impl_->send(nullptr, [&](FramePtr out) { f(detail_Access::Wrap(std::move(out))); });       \
    }                                                                                              \
    void Name::reset() {                                                                           \
        impl_->reset();                                                                            \
    }
DECODER(VideoDecoder, VideoFrame, AVMEDIA_TYPE_VIDEO, video)
DECODER(AudioDecoder, AudioFrame, AVMEDIA_TYPE_AUDIO, audio)
#undef DECODER

namespace {
struct EncoderRuntime : Runtime {
    CodecPtr context;
    explicit EncoderRuntime(Codec id, const std::string &implementation)
        : context(detail::context(encoder(id, implementation))) {}
    void drain(Sink<Packet> sink) {
        auto p = packet();
        int result;
        while ((result = avcodec_receive_packet(context.get(), p.get())) >= 0) {
            p->time_base = context->time_base;
            sink(detail_Access::packet(std::move(p)));
            p = packet();
        }
        if (result != AVERROR(EAGAIN) && result != AVERROR_EOF)
            check(result, "Encode frame");
    }
    void send(AVFrame *frame, Sink<Packet> sink) {
        int result = avcodec_send_frame(context.get(), frame);
        if (result == AVERROR(EAGAIN)) {
            drain(sink);
            result = avcodec_send_frame(context.get(), frame);
        }
        check(result, "Send raw frame");
        drain(sink);
    }
};
template <class T> const T *supported(AVCodecContext *context, AVCodecConfig config, int &count) {
    const void *out = nullptr;
    check(avcodec_get_supported_config(context, nullptr, config, 0, &out, &count),
          "Query encoder formats");
    return static_cast<const T *>(out);
}
} // namespace
struct VideoEncoder::Impl : EncoderRuntime {
    std::optional<Rational> fps;
    std::int64_t next = 0;
    std::optional<std::int64_t> last;
    VideoProcessor converter;
    Impl(Codec id, Size size, const VideoEncodeOptions &options)
        : EncoderRuntime(id, options.implementation), fps(options.fps) {
        require(context->codec_type == AVMEDIA_TYPE_VIDEO, "video encoder", "Codec is not video");
        require(size.width > 0 && size.height > 0 && options.bitrate > 0 && options.gop_size > 0 &&
                    options.max_b_frames >= 0,
                "video encoder", "Invalid encoding configuration");
        context->width = size.width;
        context->height = size.height;
        context->bit_rate = options.bitrate;
        context->thread_count = threads(options.threads);
        context->gop_size = options.gop_size;
        context->max_b_frames = options.max_b_frames;
        context->time_base = {1, 60000};
        if (fps)
            context->framerate = av(*fps);
        int count = 0;
        auto *formats = supported<AVPixelFormat>(context.get(), AV_CODEC_CONFIG_PIX_FORMAT, count);
        auto chosen = options.pixel_format ? av(*options.pixel_format) : AV_PIX_FMT_YUV420P;
        if (formats && std::find(formats, formats + count, chosen) == formats + count) {
            if (options.pixel_format)
                fail(ErrorCode::Unsupported, "video encoder",
                     "Requested pixel format is unsupported");
            chosen = formats[0];
        }
        require(pixels(chosen) != PixelFormat::Unknown, "video encoder",
                "Encoder requires a pixel format not exposed by fflite");
        const auto *desc = av_pix_fmt_desc_get(chosen);
        require(size.width % (1 << desc->log2_chroma_w) == 0 &&
                    size.height % (1 << desc->log2_chroma_h) == 0,
                "video encoder", "Dimensions violate encoder chroma alignment");
        context->pix_fmt = chosen;
        context->sample_aspect_ratio = {1, 1};
        context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        check(avcodec_open2(context.get(), context->codec, nullptr), "Open video encoder");
        converter.format(pixels(chosen));
    }
};
VideoEncoder::VideoEncoder(Codec c, Size s, VideoEncodeOptions o)
    : impl_(std::make_unique<Impl>(c, s, o)) {}
VideoEncoder::~VideoEncoder() = default;
VideoEncoder::VideoEncoder(VideoEncoder &&) noexcept = default;
VideoEncoder &VideoEncoder::operator=(VideoEncoder &&) noexcept = default;
CodecParameters VideoEncoder::parameters() const {
    return detail_Access::params(impl_->context.get());
}
void VideoEncoder::encode_impl(const VideoFrame &source, Sink<Packet> sink) {
    impl_->invoke([&] {
        impl_->input();
        auto *c = impl_->context.get();
        require(source.width() == c->width && source.height() == c->height, "video encode",
                "Dimensions differ; use VideoProcessor");
        auto converted = impl_->converter.process(source);
        auto f = ref(detail_Access::frame(converted));
        if (source.pts())
            f->pts = ticks(*source.pts(), c->time_base);
        else {
            require(impl_->fps.has_value(), "video encode", "Missing PTS requires an explicit fps");
            f->pts = impl_->next;
        }
        if (impl_->last)
            require(f->pts > *impl_->last, "video encode",
                    "PTS must increase at the encoder time base");
        f->time_base = c->time_base;
        f->pict_type = AV_PICTURE_TYPE_NONE;
        const auto step =
            impl_->fps
                ? av_rescale_q(1, av_inv_q(av(*impl_->fps)), c->time_base)
                : std::max(std::int64_t{1},
                           av_rescale_q(f->duration,
                                        av(rational(detail_Access::frame(source)->time_base)),
                                        c->time_base));
        require(step > 0 && f->pts <= std::numeric_limits<std::int64_t>::max() - step,
                "video encode", "Timestamp overflow or fps too high");
        impl_->last = f->pts;
        impl_->next = f->pts + step;
        f->duration = step;
        impl_->send(f.get(), sink);
    });
}
void VideoEncoder::flush_impl(Sink<Packet> sink) {
    impl_->invoke([&] {
        if (impl_->state == State::Drained)
            return;
        impl_->send(nullptr, sink);
        impl_->state = State::Drained;
    });
}

struct AudioEncoder::Impl : EncoderRuntime {
    AudioFormat target;
    std::unique_ptr<AudioGraph> graph;
    std::int64_t next = 0;
    Impl(Codec c, const AudioFormat &fmt, const AudioEncodeOptions &options)
        : EncoderRuntime(c, options.implementation), target(fmt) {
        require(context->codec_type == AVMEDIA_TYPE_AUDIO, "audio encoder", "Codec is not audio");
        require(fmt.sample_rate > 0 && options.bitrate > 0, "audio encoder",
                "Invalid configuration");
        context->sample_rate = fmt.sample_rate;
        context->time_base = {1, fmt.sample_rate};
        context->bit_rate = options.bitrate;
        context->thread_count = threads(options.threads);
        set_layout(context->ch_layout, fmt.layout);
        int count = 0;
        auto *formats =
            supported<AVSampleFormat>(context.get(), AV_CODEC_CONFIG_SAMPLE_FORMAT, count);
        const auto preferred = av(fmt.samples);
        context->sample_fmt = preferred;
        if (formats && std::find(formats, formats + count, preferred) == formats + count)
            context->sample_fmt = formats[0];
        target.samples = samples(context->sample_fmt);
        require(target.samples != SampleFormat::Unknown, "audio encoder",
                "Unsupported negotiated sample format");
        context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        check(avcodec_open2(context.get(), context->codec, nullptr), "Open audio encoder");
    }
    void drain_audio(Sink<Packet> sink) {
        auto f = frame();
        int result;
        while ((result = graph->receive(f.get())) >= 0) {
            f->pts = av_rescale_q(f->pts, graph->time_base(), context->time_base);
            f->time_base = context->time_base;
            send(f.get(), sink);
            f = frame();
        }
        if (result != AVERROR(EAGAIN) && result != AVERROR_EOF)
            check(result, "Frame audio for encoder");
    }
};
AudioEncoder::AudioEncoder(Codec c, AudioFormat f, AudioEncodeOptions o)
    : impl_(std::make_unique<Impl>(c, f, o)) {}
AudioEncoder::~AudioEncoder() = default;
AudioEncoder::AudioEncoder(AudioEncoder &&) noexcept = default;
AudioEncoder &AudioEncoder::operator=(AudioEncoder &&) noexcept = default;
CodecParameters AudioEncoder::parameters() const {
    return detail_Access::params(impl_->context.get());
}
void AudioEncoder::encode_impl(const AudioFrame &source, Sink<Packet> sink) {
    impl_->invoke([&] {
        impl_->input();
        require(source.sample_rate() == impl_->target.sample_rate &&
                    source.channel_layout().name() == impl_->target.layout.name(),
                "audio encode", "Rate/layout differ; use AudioProcessor");
        auto f = ref(detail_Access::frame(source));
        const auto base = impl_->context->time_base;
        f->pts = source.pts() ? ticks(*source.pts(), base) : impl_->next;
        f->time_base = base;
        require(f->pts <= std::numeric_limits<std::int64_t>::max() - f->nb_samples, "audio encode",
                "Timestamp overflow");
        impl_->next = f->pts + f->nb_samples;
        if (impl_->graph && !impl_->graph->matches(*f)) {
            impl_->graph->push(nullptr);
            impl_->drain_audio(sink);
            impl_->graph.reset();
        }
        if (!impl_->graph)
            impl_->graph = std::make_unique<AudioGraph>(*f, base, impl_->target, 1.0,
                                                        impl_->context->frame_size);
        impl_->graph->push(f.get());
        impl_->drain_audio(sink);
    });
}
void AudioEncoder::flush_impl(Sink<Packet> sink) {
    impl_->invoke([&] {
        if (impl_->state == State::Drained)
            return;
        if (impl_->graph) {
            impl_->graph->push(nullptr);
            impl_->drain_audio(sink);
        }
        impl_->send(nullptr, sink);
        impl_->state = State::Drained;
    });
}
} // namespace fflite
