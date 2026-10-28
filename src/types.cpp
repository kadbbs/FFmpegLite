#include "detail.hpp"
#include <thread>
namespace fflite {
Error::Error(ErrorCode code, std::string operation, std::string message, int native_code)
    : std::runtime_error(operation + ": " + message), code_(code), operation_(std::move(operation)),
      native_(native_code) {}
Rational::Rational(int n, int d) : num(n), den(d) {
    detail::require(n > 0 && d > 0, "time base", "Numerator and denominator must be positive");
}
Timestamp::Timestamp(std::int64_t v, Rational b) : ticks(v), time_base(b) {
    detail::require(b.num > 0 && b.den > 0 && v != AV_NOPTS_VALUE, "timestamp",
                    "Invalid time base or reserved timestamp");
}
double Timestamp::seconds() const noexcept {
    return double(ticks) * time_base.num / time_base.den;
}
Timestamp Timestamp::rescaled(Rational base) const {
    return Timestamp(detail::ticks(*this, detail::av(base)), base);
}
Duration::Duration(std::int64_t v, Rational b) : ticks(v), time_base(b) {
    detail::require(v >= 0 && b.num > 0 && b.den > 0, "duration", "Invalid duration");
}
double Duration::seconds() const noexcept {
    return double(ticks) * time_base.num / time_base.den;
}
ChannelLayout::ChannelLayout(int channels) : channels_(channels) {
    detail::require(channels > 0 && channels <= 8, "channel layout",
                    "Default layouts support 1..8 channels; use named() for other layouts");
    AVChannelLayout layout{};
    av_channel_layout_default(&layout, channels);
    char name[256]{};
    detail::check(av_channel_layout_describe(&layout, name, sizeof(name)), "Describe channels");
    name_ = name;
    av_channel_layout_uninit(&layout);
}
ChannelLayout ChannelLayout::named(std::string name) {
    AVChannelLayout raw{};
    detail::check(av_channel_layout_from_string(&raw, name.c_str()), "Channel layout",
                  ErrorCode::InvalidArgument);
    const int channels = raw.nb_channels;
    av_channel_layout_uninit(&raw);
    detail::require(channels > 0, "channel layout", "Layout must have channels");
    ChannelLayout result;
    result.channels_ = channels;
    result.name_ = std::move(name);
    return result;
}
CodecParameters::CodecParameters() : impl_(std::make_shared<Impl>()) {}
CodecParameters CodecParameters::video(Codec codec, std::vector<std::uint8_t> extra) {
    CodecParameters out;
    auto *p = detail_Access::params(out);
    p->codec_id = detail::av(codec);
    p->codec_type = AVMEDIA_TYPE_VIDEO;
    detail::require(avcodec_get_type(p->codec_id) == AVMEDIA_TYPE_VIDEO, "video parameters",
                    "Codec is not video");
    detail::require(extra.size() <=
                        std::size_t(std::numeric_limits<int>::max() - AV_INPUT_BUFFER_PADDING_SIZE),
                    "extradata", "Data too large");
    if (!extra.empty()) {
        p->extradata =
            static_cast<std::uint8_t *>(av_mallocz(extra.size() + AV_INPUT_BUFFER_PADDING_SIZE));
        if (!p->extradata)
            detail::fail(ErrorCode::Unavailable, "extradata", "Allocation failed");
        p->extradata_size = static_cast<int>(extra.size());
        std::memcpy(p->extradata, extra.data(), extra.size());
    }
    return out;
}
CodecParameters CodecParameters::audio(Codec codec, AudioFormat format,
                                       std::vector<std::uint8_t> extra) {
    detail::require(avcodec_get_type(detail::av(codec)) == AVMEDIA_TYPE_AUDIO &&
                        format.sample_rate > 0,
                    "audio parameters", "Expected audio codec and positive sample rate");
    CodecParameters out;
    auto *p = detail_Access::params(out);
    p->codec_id = detail::av(codec);
    p->codec_type = AVMEDIA_TYPE_AUDIO;
    p->sample_rate = format.sample_rate;
    p->format = detail::av(format.samples);
    detail::set_layout(p->ch_layout, format.layout);
    detail::require(extra.size() <=
                        std::size_t(std::numeric_limits<int>::max() - AV_INPUT_BUFFER_PADDING_SIZE),
                    "extradata", "Data too large");
    if (!extra.empty()) {
        p->extradata =
            static_cast<std::uint8_t *>(av_mallocz(extra.size() + AV_INPUT_BUFFER_PADDING_SIZE));
        if (!p->extradata)
            detail::fail(ErrorCode::Unavailable, "extradata", "Allocation failed");
        p->extradata_size = static_cast<int>(extra.size());
        std::memcpy(p->extradata, extra.data(), extra.size());
    }
    return out;
}
Codec CodecParameters::codec() const {
    return detail::codec(impl_->data->codec_id);
}
MediaType CodecParameters::media_type() const {
    return detail::media(impl_->data->codec_type);
}
Size CodecParameters::size() const {
    return {impl_->data->width, impl_->data->height};
}
int CodecParameters::sample_rate() const {
    return impl_->data->sample_rate;
}
ChannelLayout CodecParameters::channel_layout() const {
    return detail::layout(impl_->data->ch_layout);
}
std::vector<std::uint8_t> CodecParameters::extradata() const {
    auto *p = impl_->data.get();
    if (!p->extradata_size)
        return {};
    return {p->extradata, p->extradata + p->extradata_size};
}
} // namespace fflite
namespace fflite::detail {
void fail(ErrorCode c, const char *op, const std::string &msg) {
    throw Error(c, op, msg);
}
void require(bool c, const char *op, const std::string &msg) {
    if (!c)
        fail(ErrorCode::InvalidArgument, op, msg);
}
void check(int value, const char *op, ErrorCode code) {
    if (value >= 0)
        return;
    char text[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(value, text, sizeof(text));
    throw Error(code, op, text, value);
}
AVRational av(Rational v) {
    require(v.num > 0 && v.den > 0, "time base", "Invalid rational");
    return {v.num, v.den};
}
Rational rational(AVRational v) {
    return v.num > 0 && v.den > 0 ? Rational{v.num, v.den} : Rational{1, 1000000};
}
std::optional<Timestamp> timestamp(std::int64_t v, AVRational b) {
    if (v == AV_NOPTS_VALUE)
        return {};
    return Timestamp(v, rational(b));
}
std::int64_t ticks(const Timestamp &v, AVRational b) {
    auto result = av_rescale_q(v.ticks, av(v.time_base), b);
    if (result == AV_NOPTS_VALUE)
        fail(ErrorCode::InvalidArgument, "timestamp", "Timestamp overflow");
    return result;
}
std::uint64_t identity() {
    static std::atomic<std::uint64_t> next{1};
    return next.fetch_add(1);
}
int threads(int n) {
    require(n >= 0, "threads", "Thread count must be nonnegative");
    return n ? n : std::min(8u, std::max(1u, std::thread::hardware_concurrency()));
}
FramePtr frame() {
    FramePtr p(av_frame_alloc());
    if (!p)
        fail(ErrorCode::Unavailable, "frame", "Allocation failed");
    return p;
}
PacketPtr packet() {
    PacketPtr p(av_packet_alloc());
    if (!p)
        fail(ErrorCode::Unavailable, "packet", "Allocation failed");
    return p;
}
ParamsPtr params() {
    ParamsPtr p(avcodec_parameters_alloc());
    if (!p)
        fail(ErrorCode::Unavailable, "parameters", "Allocation failed");
    return p;
}
CodecPtr context(const AVCodec *codec) {
    if (!codec)
        fail(ErrorCode::Unavailable, "codec",
             "Requested codec is not present in this FFmpeg build");
    CodecPtr p(avcodec_alloc_context3(codec));
    if (!p)
        fail(ErrorCode::Unavailable, "codec", "Allocation failed");
    return p;
}
FramePtr ref(const AVFrame *f) {
    FramePtr p(av_frame_clone(f));
    if (!p)
        fail(ErrorCode::Unavailable, "frame reference", "Allocation failed");
    return p;
}
PacketPtr ref(const AVPacket *f) {
    auto p = packet();
    check(av_packet_ref(p.get(), f), "Reference packet");
    return p;
}
const AVCodec *encoder(Codec id, const std::string &implementation) {
    const AVCodec *selected = nullptr;
    if (!implementation.empty())
        selected = avcodec_find_encoder_by_name(implementation.c_str());
    else {
        void *iterator = nullptr;
        while (auto *c = av_codec_iterate(&iterator)) {
            if (c->id == av(id) && av_codec_is_encoder(c) &&
                !(c->capabilities & AV_CODEC_CAP_HARDWARE)) {
                selected = c;
                break;
            }
        }
    }
    if (!selected)
        fail(ErrorCode::Unavailable, "encoder",
             implementation.empty()
                 ? std::string("No software encoder for ") + avcodec_get_name(av(id))
                 : implementation);
    require(selected->id == av(id) && av_codec_is_encoder(selected), "encoder",
            "Implementation does not match the requested codec");
    return selected;
}
AVCodecID av(Codec v) {
    switch (v) {
#define C(a, b)                                                                                    \
    case Codec::a:                                                                                 \
        return AV_CODEC_ID_##b;
        C(H264, H264)
        C(HEVC, HEVC) C(MPEG4, MPEG4) C(VP8, VP8) C(VP9, VP9) C(AV1, AV1) C(AAC, AAC) C(Opus, OPUS)
            C(MP3, MP3) C(PCM_S16LE, PCM_S16LE) C(SubRip, SUBRIP) C(MovText, MOV_TEXT)
#undef C
                default : return AV_CODEC_ID_NONE;
    }
}
Codec codec(AVCodecID v) {
    switch (v) {
#define C(a, b)                                                                                    \
    case AV_CODEC_ID_##b:                                                                          \
        return Codec::a;
        C(H264, H264)
        C(HEVC, HEVC) C(MPEG4, MPEG4) C(VP8, VP8) C(VP9, VP9) C(AV1, AV1) C(AAC, AAC) C(Opus, OPUS)
            C(MP3, MP3) C(PCM_S16LE, PCM_S16LE) C(SubRip, SUBRIP) C(MovText, MOV_TEXT)
#undef C
                default : return Codec::Unknown;
    }
}
AVPixelFormat av(PixelFormat v) {
    switch (v) {
#define C(a, b)                                                                                    \
    case PixelFormat::a:                                                                           \
        return AV_PIX_FMT_##b;
        C(YUV420P, YUV420P)
        C(YUV422P, YUV422P) C(YUV444P, YUV444P) C(NV12, NV12) C(RGB24, RGB24) C(RGBA, RGBA)
            C(BGRA, BGRA) C(Gray8, GRAY8)
#undef C
                default : fail(ErrorCode::Unsupported, "pixel format", "Unsupported pixel format");
    }
}
PixelFormat pixels(int v) {
    switch (v) {
#define C(a, b)                                                                                    \
    case AV_PIX_FMT_##b:                                                                           \
        return PixelFormat::a;
        C(YUV420P, YUV420P)
        C(YUV422P, YUV422P) C(YUV444P, YUV444P) C(NV12, NV12) C(RGB24, RGB24) C(RGBA, RGBA)
            C(BGRA, BGRA) C(Gray8, GRAY8)
#undef C
                default : return PixelFormat::Unknown;
    }
}
AVSampleFormat av(SampleFormat v) {
    switch (v) {
#define C(a, b)                                                                                    \
    case SampleFormat::a:                                                                          \
        return AV_SAMPLE_FMT_##b;
        C(U8, U8)
        C(S16, S16) C(S32, S32) C(F32, FLT) C(F64, DBL) C(U8Planar, U8P) C(S16Planar, S16P)
            C(S32Planar, S32P) C(F32Planar, FLTP) C(F64Planar, DBLP)
#undef C
                default
            : fail(ErrorCode::Unsupported, "sample format", "Unsupported sample format");
    }
}
SampleFormat samples(int v) {
    switch (v) {
#define C(a, b)                                                                                    \
    case AV_SAMPLE_FMT_##b:                                                                        \
        return SampleFormat::a;
        C(U8, U8)
        C(S16, S16) C(S32, S32) C(F32, FLT) C(F64, DBL) C(U8Planar, U8P) C(S16Planar, S16P)
            C(S32Planar, S32P) C(F32Planar, FLTP) C(F64Planar, DBLP)
#undef C
                default : return SampleFormat::Unknown;
    }
}
MediaType media(AVMediaType t) {
    switch (t) {
    case AVMEDIA_TYPE_VIDEO:
        return MediaType::Video;
    case AVMEDIA_TYPE_AUDIO:
        return MediaType::Audio;
    case AVMEDIA_TYPE_SUBTITLE:
        return MediaType::Subtitle;
    default:
        return MediaType::Other;
    }
}
ChannelLayout layout(const AVChannelLayout &l) {
    require(l.nb_channels > 0, "channel layout", "No channel information");
    if (l.order == AV_CHANNEL_ORDER_UNSPEC)
        return ChannelLayout(l.nb_channels);
    char text[512]{};
    check(av_channel_layout_describe(&l, text, sizeof(text)), "Describe layout");
    return ChannelLayout::named(text);
}
void set_layout(AVChannelLayout &out, const ChannelLayout &l) {
    av_channel_layout_uninit(&out);
    check(av_channel_layout_from_string(&out, l.name().c_str()), "Set channel layout");
}
} // namespace fflite::detail
