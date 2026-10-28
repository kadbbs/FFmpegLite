#include "detail.hpp"
#include <deque>
#include <filesystem>
namespace fflite {
using namespace detail;
struct Demuxer::Impl : Runtime {
    std::unique_ptr<AVFormatContext, InputFree> context;
    std::uint64_t owner = identity();
    explicit Impl(const std::string &path) {
        require(!path.empty(), "demuxer", "Empty path");
        AVFormatContext *raw = nullptr;
        const int result = avformat_open_input(&raw, path.c_str(), nullptr, nullptr);
        context.reset(raw);
        check(result, "Open input", ErrorCode::Io);
        check(avformat_find_stream_info(context.get(), nullptr), "Read streams");
    }
    StreamInfo best(AVMediaType type) const {
        int i = av_find_best_stream(context.get(), type, -1, -1, nullptr, 0);
        check(i, "Find media stream", ErrorCode::Unavailable);
        return detail_Access::stream(owner, context->streams[i]);
    }
};
Demuxer::Demuxer(const std::string &p) : impl_(std::make_unique<Impl>(p)) {}
Demuxer::~Demuxer() = default;
Demuxer::Demuxer(Demuxer &&) noexcept = default;
Demuxer &Demuxer::operator=(Demuxer &&) noexcept = default;
std::vector<StreamInfo> Demuxer::streams() const {
    std::vector<StreamInfo> out;
    for (unsigned i = 0; i < impl_->context->nb_streams; ++i)
        out.push_back(detail_Access::stream(impl_->owner, impl_->context->streams[i]));
    return out;
}
StreamInfo Demuxer::video() const {
    return impl_->best(AVMEDIA_TYPE_VIDEO);
}
StreamInfo Demuxer::audio() const {
    return impl_->best(AVMEDIA_TYPE_AUDIO);
}
std::optional<Packet> Demuxer::read() {
    std::optional<Packet> out;
    impl_->invoke([&] {
        if (impl_->state == State::Drained)
            return;
        auto p = packet();
        const int result = av_read_frame(impl_->context.get(), p.get());
        if (result == AVERROR_EOF) {
            impl_->state = State::Drained;
            return;
        }
        check(result, "Read packet", ErrorCode::Io);
        const auto index = p->stream_index;
        p->time_base = impl_->context->streams[index]->time_base;
        out = detail_Access::packet(std::move(p), detail_Access::id(impl_->owner, index));
    });
    return out;
}
void Demuxer::seek(Timestamp position) {
    impl_->invoke([&] {
        check(avformat_seek_file(impl_->context.get(), -1, std::numeric_limits<std::int64_t>::min(),
                                 ticks(position, AV_TIME_BASE_Q),
                                 std::numeric_limits<std::int64_t>::max(), 0),
              "Seek input", ErrorCode::Io);
        impl_->state = State::Ready;
    });
}

void SubtitleTrack::add(SubtitleCue cue) {
    require(cue.duration.ticks > 0 && !cue.text.empty(), "subtitle cue",
            "Text and positive duration are required");
    cues_.push_back(std::move(cue));
    std::stable_sort(cues_.begin(), cues_.end(), [](const auto &a, const auto &b) {
        return av_compare_ts(a.start.ticks, av(a.start.time_base), b.start.ticks,
                             av(b.start.time_base)) < 0;
    });
}
SubtitleTrack SubtitleTrack::load(const std::string &path) {
    Demuxer input(path);
    std::optional<StreamInfo> subtitles;
    for (const auto &s : input.streams())
        if (s.media_type() == MediaType::Subtitle) {
            subtitles = s;
            break;
        }
    if (!subtitles || subtitles->parameters().codec() != Codec::SubRip)
        fail(ErrorCode::Unsupported, "load subtitles", "Expected an SRT file");
    SubtitleTrack track;
    while (auto p = input.read())
        if (p->stream_id() == subtitles->id()) {
            if (!p->pts())
                fail(ErrorCode::InvalidData, "load subtitles", "Cue has no timestamp");
            track.add({*p->pts(), p->duration(),
                       std::string(reinterpret_cast<const char *>(p->data()), p->size())});
        }
    return track;
}

struct Muxer::Impl : Runtime {
    std::unique_ptr<AVFormatContext, OutputFree> context;
    std::uint64_t owner = identity();
    std::string path;
    bool header = false;
    struct Cue {
        int stream;
        PacketPtr packet;
        AVRational base;
    };
    std::vector<Cue> subtitles;
    std::size_t cursor = 0;
    explicit Impl(const std::string &name) : path(name) {
        require(!name.empty(), "muxer", "Empty path");
        std::error_code error;
        const bool exists = std::filesystem::exists(name, error);
        if (error)
            fail(ErrorCode::Io, "output path", error.message());
        require(!exists, "muxer", "Output already exists");
        AVFormatContext *raw = nullptr;
        const int result = avformat_alloc_output_context2(&raw, nullptr, nullptr, name.c_str());
        context.reset(raw);
        check(result, "Create output", ErrorCode::Unsupported);
        if (!context)
            fail(ErrorCode::Unavailable, "muxer", "Allocation failed");
    }
    AVStream *add(const CodecParameters &parameters) {
        if (header || state != State::Ready)
            fail(ErrorCode::InvalidState, "add stream", "Register streams before first write()");
        auto *p = detail_Access::params(parameters);
        require(p->codec_type == AVMEDIA_TYPE_VIDEO || p->codec_type == AVMEDIA_TYPE_AUDIO ||
                    p->codec_type == AVMEDIA_TYPE_SUBTITLE,
                "add stream", "Unsupported media type");
        if (p->codec_type == AVMEDIA_TYPE_VIDEO)
            require(p->width > 0 && p->height > 0, "add stream", "Video dimensions are missing");
        if (p->codec_type == AVMEDIA_TYPE_AUDIO)
            require(p->sample_rate > 0 && p->ch_layout.nb_channels > 0, "add stream",
                    "Audio format is incomplete");
        if (avformat_query_codec(context->oformat, p->codec_id, FF_COMPLIANCE_NORMAL) == 0)
            fail(ErrorCode::Unsupported, "add stream",
                 "Codec is not supported by output container");
        auto *s = avformat_new_stream(context.get(), nullptr);
        if (!s)
            fail(ErrorCode::Unavailable, "add stream", "Allocation failed");
        check(avcodec_parameters_copy(s->codecpar, p), "Copy output parameters");
        s->codecpar->codec_tag = 0;
        s->time_base = p->codec_type == AVMEDIA_TYPE_AUDIO   ? AVRational{1, p->sample_rate}
                       : p->codec_type == AVMEDIA_TYPE_VIDEO ? AVRational{1, 60000}
                                                             : AVRational{1, 1000};
        return s;
    }
    void start() {
        if (header)
            return;
        require(context->nb_streams > 0, "muxer", "No output streams");
        if (!(context->oformat->flags & AVFMT_NOFILE))
            check(avio_open(&context->pb, path.c_str(), AVIO_FLAG_WRITE), "Open output",
                  ErrorCode::Io);
        check(avformat_write_header(context.get(), nullptr), "Write container header",
              ErrorCode::Io);
        header = true;
        state = State::Running;
        std::stable_sort(subtitles.begin(), subtitles.end(), [](const Cue &a, const Cue &b) {
            return av_compare_ts(a.packet->pts, a.base, b.packet->pts, b.base) < 0;
        });
    }
    void write_native(int index, AVPacket *packet, AVRational base) {
        av_packet_rescale_ts(packet, base, context->streams[index]->time_base);
        packet->stream_index = index;
        packet->pos = -1;
        check(av_interleaved_write_frame(context.get(), packet), "Write packet", ErrorCode::Io);
    }
    void cues(std::int64_t time, AVRational base, bool all = false) {
        while (cursor < subtitles.size()) {
            auto &cue = subtitles[cursor];
            if (!all && av_compare_ts(cue.packet->pts, cue.base, time, base) > 0)
                break;
            write_native(cue.stream, cue.packet.get(), cue.base);
            ++cursor;
        }
    }
};
Muxer::Muxer(const std::string &path) : impl_(std::make_unique<Impl>(path)) {}
Muxer::~Muxer() = default;
Muxer::Muxer(Muxer &&) noexcept = default;
Muxer &Muxer::operator=(Muxer &&) noexcept = default;
StreamId Muxer::add_stream(const CodecParameters &p) {
    StreamId id;
    impl_->invoke([&] { id = detail_Access::id(impl_->owner, impl_->add(p)->index); });
    return id;
}
StreamId Muxer::add_subtitles(const SubtitleTrack &track, SubtitleOptions options) {
    StreamId result;
    impl_->invoke([&] {
        const bool srt = avformat_query_codec(impl_->context->oformat, AV_CODEC_ID_SUBRIP,
                                              FF_COMPLIANCE_NORMAL) > 0;
        const auto id = srt ? AV_CODEC_ID_SUBRIP : AV_CODEC_ID_MOV_TEXT;
        if (avformat_query_codec(impl_->context->oformat, id, FF_COMPLIANCE_NORMAL) <= 0)
            fail(ErrorCode::Unsupported, "subtitles", "Container does not support text subtitles");
        CodecParameters parameters;
        auto *p = detail_Access::params(parameters);
        p->codec_type = AVMEDIA_TYPE_SUBTITLE;
        p->codec_id = id;
        CodecPtr decoder, encoder;
        if (!srt) {
            decoder = context(avcodec_find_decoder(AV_CODEC_ID_SUBRIP));
            decoder->pkt_timebase = {1, 1000};
            check(avcodec_open2(decoder.get(), decoder->codec, nullptr), "Open subtitle decoder");
            encoder = context(avcodec_find_encoder(AV_CODEC_ID_MOV_TEXT));
            encoder->time_base = {1, 1000};
            encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
            encoder->subtitle_header = static_cast<std::uint8_t *>(
                av_mallocz(decoder->subtitle_header_size + AV_INPUT_BUFFER_PADDING_SIZE));
            if (!encoder->subtitle_header)
                fail(ErrorCode::Unavailable, "subtitle header", "Allocation failed");
            encoder->subtitle_header_size = decoder->subtitle_header_size;
            std::memcpy(encoder->subtitle_header, decoder->subtitle_header,
                        decoder->subtitle_header_size);
            check(avcodec_open2(encoder.get(), encoder->codec, nullptr), "Open subtitle encoder");
            parameters = detail_Access::params(encoder.get());
        }
        auto *stream = impl_->add(parameters);
        result = detail_Access::id(impl_->owner, stream->index);
        check(av_dict_set(&stream->metadata, "language", options.language.c_str(), 0),
              "Subtitle language");
        if (!options.title.empty())
            check(av_dict_set(&stream->metadata, "title", options.title.c_str(), 0),
                  "Subtitle title");
        std::vector<std::uint8_t> buffer(srt ? 0 : 1024 * 1024);
        for (const auto &cue : track.cues()) {
            auto raw = Packet::copy(cue.text.data(), cue.text.size());
            raw.set_pts(cue.start.rescaled({1, 1000}));
            raw.set_dts(raw.pts());
            raw.set_duration(cue.duration);
            auto *input = detail_Access::packet(raw);
            auto out = packet();
            if (srt)
                check(av_packet_ref(out.get(), input), "Copy subtitle cue");
            else {
                struct Sub {
                    AVSubtitle value{};
                    ~Sub() { avsubtitle_free(&value); }
                } sub;
                int got = 0;
                check(avcodec_decode_subtitle2(decoder.get(), &sub.value, &got, input),
                      "Decode text cue");
                if (!got)
                    fail(ErrorCode::InvalidData, "subtitle", "No decoded text");
                const int size = avcodec_encode_subtitle(
                    encoder.get(), buffer.data(), static_cast<int>(buffer.size()), &sub.value);
                check(size, "Encode text cue");
                check(av_new_packet(out.get(), size), "Allocate subtitle output");
                std::memcpy(out->data, buffer.data(), size);
                out->pts = input->pts;
                out->dts = input->dts;
                out->duration = input->duration;
            }
            impl_->subtitles.push_back({stream->index, std::move(out), {1, 1000}});
        }
    });
    return result;
}
void Muxer::write(StreamId id, const Packet &packet) {
    impl_->invoke([&] {
        if (impl_->state == State::Drained)
            fail(ErrorCode::InvalidState, "muxer", "Output is finished");
        const int index = detail_Access::index(id, impl_->owner);
        require(index < int(impl_->context->nb_streams), "write", "Unknown output stream");
        require(packet.size() > 0 && packet.pts().has_value() && packet.dts().has_value(), "write",
                "Packet data, PTS and DTS are required");
        impl_->start();
        auto p = ref(detail_Access::packet(packet));
        const auto base = av(rational(p->time_base));
        impl_->cues(p->dts, base);
        impl_->write_native(index, p.get(), base);
    });
}
void Muxer::finish() {
    impl_->invoke([&] {
        if (impl_->state == State::Drained)
            return;
        impl_->start();
        impl_->cues(0, {1, 1}, true);
        check(av_write_trailer(impl_->context.get()), "Finish container", ErrorCode::Io);
        if (impl_->context->pb)
            check(avio_closep(&impl_->context->pb), "Close output", ErrorCode::Io);
        impl_->state = State::Drained;
    });
}

struct VideoReader::Impl {
    Demuxer demux;
    StreamInfo stream;
    VideoDecoder decoder;
    std::deque<VideoFrame> frames;
    bool eof = false;
    Runtime runtime;
    explicit Impl(const std::string &path) : demux(path), stream(demux.video()), decoder(stream) {}
};
VideoReader::VideoReader(const std::string &path) : impl_(std::make_unique<Impl>(path)) {}
VideoReader::~VideoReader() = default;
VideoReader::VideoReader(VideoReader &&) noexcept = default;
VideoReader &VideoReader::operator=(VideoReader &&) noexcept = default;
std::optional<VideoFrame> VideoReader::read() {
    std::optional<VideoFrame> out;
    impl_->runtime.invoke([&] {
        auto collect = [&](const VideoFrame &f) { impl_->frames.push_back(f); };
        while (impl_->frames.empty() && !impl_->eof) {
            auto packet = impl_->demux.read();
            if (!packet) {
                impl_->decoder.flush(collect);
                impl_->eof = true;
            } else if (packet->stream_id() == impl_->stream.id())
                impl_->decoder.decode(*packet, collect);
        }
        if (!impl_->frames.empty()) {
            out = std::move(impl_->frames.front());
            impl_->frames.pop_front();
        }
    });
    return out;
}
} // namespace fflite
