#include "detail.hpp"
#include <iomanip>
#include <locale>
#include <sstream>
namespace fflite::detail {
AudioGraph::AudioGraph(const AVFrame &input, AVRational base, const AudioFormat &target,
                       double gain, int frame_size)
    : graph_(avfilter_graph_alloc()), rate_(input.sample_rate), format_(input.format),
      layout_(layout(input.ch_layout)) {
    if (!graph_)
        fail(ErrorCode::Unavailable, "audio graph", "Allocation failed");
    graph_->nb_threads = 1;
    AVFilterContext *tail = nullptr;
    auto add = [&](const char *name,
                   std::initializer_list<std::pair<const char *, std::string>> options) {
        auto *filter = avfilter_get_by_name(name);
        if (!filter)
            fail(ErrorCode::Unavailable, "audio filter", name);
        auto *node = avfilter_graph_alloc_filter(
            graph_.get(), filter, (std::string(name) + std::to_string(graph_->nb_filters)).c_str());
        if (!node)
            fail(ErrorCode::Unavailable, "audio filter", "Allocation failed");
        for (const auto &option : options)
            check(av_opt_set(node, option.first, option.second.c_str(), AV_OPT_SEARCH_CHILDREN),
                  "Set audio option");
        check(avfilter_init_str(node, nullptr), "Initialize audio filter");
        if (tail)
            check(avfilter_link(tail, 0, node, 0), "Connect audio filters");
        else
            source_ = node;
        tail = node;
        return node;
    };
    const auto *sample_name = av_get_sample_fmt_name(static_cast<AVSampleFormat>(input.format));
    require(sample_name, "audio format", "Unknown sample format");
    add("abuffer", {{"time_base", std::to_string(base.num) + "/" + std::to_string(base.den)},
                    {"sample_rate", std::to_string(input.sample_rate)},
                    {"sample_fmt", sample_name},
                    {"channel_layout", layout_.name()}});
    add("aresample", {{"sample_rate", std::to_string(target.sample_rate)}});
    std::ostringstream volume;
    volume.imbue(std::locale::classic());
    volume << std::setprecision(17) << gain;
    add("volume", {{"volume", volume.str()}});
    add("aformat", {{"sample_fmts", av_get_sample_fmt_name(av(target.samples))},
                    {"sample_rates", std::to_string(target.sample_rate)},
                    {"channel_layouts", target.layout.name()}});
    sink_ = add("abuffersink", {});
    check(avfilter_graph_config(graph_.get(), nullptr), "Configure audio graph");
    if (frame_size > 0)
        av_buffersink_set_frame_size(sink_, frame_size);
}
void AudioGraph::push(AVFrame *f) {
    check(av_buffersrc_add_frame_flags(source_, f, AV_BUFFERSRC_FLAG_KEEP_REF), "Process audio");
}
int AudioGraph::receive(AVFrame *f) {
    return av_buffersink_get_frame(sink_, f);
}
AVRational AudioGraph::time_base() const {
    return av_buffersink_get_time_base(sink_);
}
bool AudioGraph::matches(const AVFrame &f) const {
    return rate_ == f.sample_rate && format_ == f.format &&
           layout_.name() == layout(f.ch_layout).name();
}
} // namespace fflite::detail
namespace fflite {
using namespace detail;
struct VideoProcessor::Impl : Runtime {
    struct Op {
        enum Kind { Crop, Resize, Format } kind;
        int x = 0, y = 0, w = 0, h = 0;
        ResizeMode mode = ResizeMode::Fit;
        PixelFormat fmt = PixelFormat::Unknown;
    };
    std::vector<Op> operations;
    struct ScaleFree {
        void operator()(SwsContext *p) const { sws_freeContext(p); }
    };
    std::unique_ptr<SwsContext, ScaleFree> scaler;
    void configurable() {
        if (state != State::Ready)
            fail(ErrorCode::InvalidState, "video processor", "Configure before first process()");
    }
    FramePtr convert(const AVFrame *source, int w, int h, AVPixelFormat format) {
        auto out = frame();
        out->width = w;
        out->height = h;
        out->format = format;
        check(av_frame_get_buffer(out.get(), 32), "Allocate converted video");
        check(av_frame_copy_props(out.get(), source), "Copy video metadata");
        auto *ctx = sws_getCachedContext(scaler.release(), source->width, source->height,
                                         static_cast<AVPixelFormat>(source->format), w, h, format,
                                         SWS_BILINEAR, nullptr, nullptr, nullptr);
        scaler.reset(ctx);
        if (!ctx)
            fail(ErrorCode::Unsupported, "video conversion", "Unsupported dimensions or formats");
        check(sws_scale(ctx, source->data, source->linesize, 0, source->height, out->data,
                        out->linesize),
              "Scale video");
        return out;
    }
    FramePtr resize_frame(const AVFrame *source, const Op &op) {
        if (op.mode == ResizeMode::Stretch) {
            auto out = convert(source, op.w, op.h, static_cast<AVPixelFormat>(source->format));
            out->sample_aspect_ratio = {1, 1};
            return out;
        }
        double sar = source->sample_aspect_ratio.num > 0 && source->sample_aspect_ratio.den > 0
                         ? av_q2d(source->sample_aspect_ratio)
                         : 1;
        const double aspect = double(source->width) * sar / source->height;
        require(std::isfinite(aspect) && aspect > 0, "resize", "Invalid display aspect ratio");
        int w = op.w, h = op.h;
        if (op.mode == ResizeMode::Fit) {
            if (double(w) / h > aspect)
                w = std::max(1, int(std::floor(h * aspect)));
            else
                h = std::max(1, int(std::floor(w / aspect)));
        } else {
            const double needed_w = std::ceil(h * aspect), needed_h = std::ceil(w / aspect);
            require(needed_w <= 32768 && needed_h <= 32768, "resize",
                    "Intermediate image too large");
            if (double(w) / h > aspect)
                h = std::max(h, int(needed_h));
            else
                w = std::max(w, int(needed_w));
        }
        auto scaled = convert(source, w, h, AV_PIX_FMT_RGBA);
        auto out = VideoFrame::allocate(op.w, op.h, PixelFormat::RGBA);
        auto *dst = detail_Access::frame(out);
        check(av_frame_copy_props(dst, source), "Copy resize metadata");
        dst->sample_aspect_ratio = {1, 1};
        for (int row = 0; row < op.h; ++row) {
            auto *p = dst->data[0] + row * dst->linesize[0];
            std::memset(p, 0, op.w * 4);
            for (int x = 0; x < op.w; ++x)
                p[x * 4 + 3] = 255;
        }
        const int copy_w = std::min(w, op.w), copy_h = std::min(h, op.h);
        const int sx = (w - copy_w) / 2, sy = (h - copy_h) / 2, dx = (op.w - copy_w) / 2,
                  dy = (op.h - copy_h) / 2;
        for (int row = 0; row < copy_h; ++row)
            std::memcpy(dst->data[0] + (row + dy) * dst->linesize[0] + dx * 4,
                        scaled->data[0] + (row + sy) * scaled->linesize[0] + sx * 4, copy_w * 4);
        if (source->format == AV_PIX_FMT_RGBA)
            return ref(dst);
        auto result = convert(dst, op.w, op.h, static_cast<AVPixelFormat>(source->format));
        result->sample_aspect_ratio = {1, 1};
        return result;
    }
};
VideoProcessor::VideoProcessor() : impl_(std::make_unique<Impl>()) {}
VideoProcessor::~VideoProcessor() = default;
VideoProcessor::VideoProcessor(VideoProcessor &&) noexcept = default;
VideoProcessor &VideoProcessor::operator=(VideoProcessor &&) noexcept = default;
VideoProcessor &VideoProcessor::crop(int x, int y, int w, int h) {
    impl_->configurable();
    require(x >= 0 && y >= 0 && w > 0 && h > 0, "crop", "Invalid rectangle");
    Impl::Op o;
    o.kind = Impl::Op::Crop;
    o.x = x;
    o.y = y;
    o.w = w;
    o.h = h;
    impl_->operations.push_back(o);
    return *this;
}
VideoProcessor &VideoProcessor::resize(int w, int h, ResizeMode mode) {
    impl_->configurable();
    require(w > 0 && h > 0 && w <= 32768 && h <= 32768, "resize", "Dimensions must be 1..32768");
    Impl::Op o;
    o.kind = Impl::Op::Resize;
    o.w = w;
    o.h = h;
    o.mode = mode;
    impl_->operations.push_back(o);
    return *this;
}
VideoProcessor &VideoProcessor::format(PixelFormat f) {
    impl_->configurable();
    (void)av(f);
    Impl::Op o;
    o.kind = Impl::Op::Format;
    o.fmt = f;
    impl_->operations.push_back(o);
    return *this;
}
VideoProcessor &VideoProcessor::subtitles(const SubtitleTrack &) {
    impl_->configurable();
    fail(
        ErrorCode::Unavailable, "subtitle rendering",
        "This build does not include a subtitle renderer; subtitle tracks can be muxed separately");
}
VideoFrame VideoProcessor::process(const VideoFrame &source) {
    auto result = source;
    impl_->invoke([&] {
        impl_->input();
        auto current = ref(detail_Access::frame(source));
        for (const auto &op : impl_->operations) {
            if (op.kind == Impl::Op::Crop) {
                require(op.x <= current->width && op.y <= current->height &&
                            op.w <= current->width - op.x && op.h <= current->height - op.y,
                        "crop", "Rectangle exceeds current frame");
                const auto *d = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(current->format));
                require(d, "crop", "Unknown pixel format");
                require(op.x % (1 << d->log2_chroma_w) == 0 && op.y % (1 << d->log2_chroma_h) == 0,
                        "crop", "Coordinates violate chroma alignment");
                current->crop_left = op.x;
                current->crop_top = op.y;
                current->crop_right = current->width - op.x - op.w;
                current->crop_bottom = current->height - op.y - op.h;
                check(av_frame_apply_cropping(current.get(), AV_FRAME_CROP_UNALIGNED),
                      "Crop frame");
            } else if (op.kind == Impl::Op::Resize)
                current = impl_->resize_frame(current.get(), op);
            else if (current->format != av(op.fmt))
                current =
                    impl_->convert(current.get(), current->width, current->height, av(op.fmt));
        }
        result = detail_Access::video(std::move(current));
    });
    return result;
}

struct AudioProcessor::Impl : Runtime {
    int rate = 0, channels = 0;
    std::optional<SampleFormat> format;
    double gain = 1;
    std::unique_ptr<AudioGraph> graph;
    std::optional<Timestamp> next;
    void configurable() {
        if (state != State::Ready)
            fail(ErrorCode::InvalidState, "audio processor",
                 "Configure before processing or after reset()");
    }
    void drain(Sink<AudioFrame> sink) {
        auto f = frame();
        int result;
        while ((result = graph->receive(f.get())) >= 0) {
            f->time_base = graph->time_base();
            sink(detail_Access::audio(std::move(f)));
            f = frame();
        }
        if (result != AVERROR(EAGAIN) && result != AVERROR_EOF)
            check(result, "Receive processed audio");
    }
};
AudioProcessor::AudioProcessor() : impl_(std::make_unique<Impl>()) {}
AudioProcessor::~AudioProcessor() = default;
AudioProcessor::AudioProcessor(AudioProcessor &&) noexcept = default;
AudioProcessor &AudioProcessor::operator=(AudioProcessor &&) noexcept = default;
AudioProcessor &AudioProcessor::resample(int rate, int channels) {
    impl_->configurable();
    require(rate > 0 && channels >= 0 && channels <= 8, "resample",
            "Invalid sample rate or channels");
    impl_->rate = rate;
    impl_->channels = channels;
    return *this;
}
AudioProcessor &AudioProcessor::format(SampleFormat f) {
    impl_->configurable();
    (void)av(f);
    impl_->format = f;
    return *this;
}
AudioProcessor &AudioProcessor::volume(double gain) {
    impl_->configurable();
    require(std::isfinite(gain) && gain >= 0, "volume", "Gain must be finite and nonnegative");
    impl_->gain = gain;
    return *this;
}
void AudioProcessor::process_impl(const AudioFrame &input, Sink<AudioFrame> sink) {
    impl_->invoke([&] {
        impl_->input();
        auto f = ref(detail_Access::frame(input));
        const auto base = AVRational{1, f->sample_rate};
        f->pts = f->pts == AV_NOPTS_VALUE ? (impl_->next ? ticks(*impl_->next, base) : 0)
                                          : av_rescale_q(f->pts, av(rational(f->time_base)), base);
        f->time_base = base;
        require(f->pts <= std::numeric_limits<std::int64_t>::max() - f->nb_samples, "audio process",
                "Timestamp overflow");
        impl_->next = Timestamp(f->pts + f->nb_samples, rational(base));
        if (impl_->graph && !impl_->graph->matches(*f)) {
            impl_->graph->push(nullptr);
            impl_->drain(sink);
            impl_->graph.reset();
        }
        if (!impl_->graph) {
            AudioFormat target{impl_->rate ? impl_->rate : f->sample_rate,
                               impl_->channels ? ChannelLayout(impl_->channels)
                                               : layout(f->ch_layout),
                               impl_->format.value_or(samples(f->format))};
            impl_->graph = std::make_unique<AudioGraph>(*f, base, target, impl_->gain);
        }
        impl_->graph->push(f.get());
        impl_->drain(sink);
    });
}
void AudioProcessor::flush_impl(Sink<AudioFrame> sink) {
    impl_->invoke([&] {
        if (impl_->state == State::Drained)
            return;
        if (impl_->graph) {
            impl_->graph->push(nullptr);
            impl_->drain(sink);
        }
        impl_->state = State::Drained;
    });
}
void AudioProcessor::reset() {
    impl_->invoke([&] {
        impl_->graph.reset();
        impl_->next.reset();
        impl_->state = State::Ready;
    });
}
} // namespace fflite
