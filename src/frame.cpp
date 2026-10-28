#include "detail.hpp"
namespace fflite {
namespace {
using namespace detail;
void frame_time(AVFrame *f, std::optional<Timestamp> time) {
    if (time) {
        const auto old = av(rational(f->time_base));
        const auto next = av(time->time_base);
        f->duration = av_rescale_q(f->duration, old, next);
        f->time_base = next;
        f->pts = time->ticks;
    } else
        f->pts = AV_NOPTS_VALUE;
}
void packet_base(AVPacket *p, Rational next) {
    av_packet_rescale_ts(p, av(rational(p->time_base)), av(next));
    p->time_base = av(next);
}
PlaneView video_plane(const AVFrame *f, int index) {
    const auto fmt = static_cast<AVPixelFormat>(f->format);
    const auto *desc = av_pix_fmt_desc_get(fmt);
    require(desc && index >= 0 && index < av_pix_fmt_count_planes(fmt), "video plane",
            "Plane index out of range");
    const int shift = (index == 1 || index == 2) ? desc->log2_chroma_h : 0;
    const int rows = AV_CEIL_RSHIFT(f->height, shift);
    const int bytes = av_image_get_linesize(fmt, f->width, index);
    check(bytes, "Plane row size");
    const auto stride = std::ptrdiff_t(f->linesize[index]);
    const auto size = std::size_t(std::abs(stride)) * std::size_t(rows - 1) + bytes;
    return {f->data[index], stride, size, bytes, rows};
}
PlaneView audio_plane(const AVFrame *f, int index) {
    const auto fmt = static_cast<AVSampleFormat>(f->format);
    const bool planar = av_sample_fmt_is_planar(fmt);
    const int count = planar ? f->ch_layout.nb_channels : 1;
    require(index >= 0 && index < count, "audio plane", "Plane index out of range");
    const auto size = std::size_t(f->nb_samples) * av_get_bytes_per_sample(fmt) *
                      (planar ? 1 : f->ch_layout.nb_channels);
    require(size <= std::size_t(std::numeric_limits<int>::max()), "audio plane", "Plane too large");
    return {f->extended_data[index], std::ptrdiff_t(size), size, static_cast<int>(size), 1};
}
void owner_free(void *opaque, std::uint8_t *) {
    delete static_cast<std::shared_ptr<void> *>(opaque);
}
void wrap_planes(AVFrame *f, const std::vector<PlaneView> &planes, std::shared_ptr<void> owner,
                 bool audio) {
    require(bool(owner), "wrap", "An owner is required");
    const int count = audio ? (av_sample_fmt_is_planar(static_cast<AVSampleFormat>(f->format))
                                   ? f->ch_layout.nb_channels
                                   : 1)
                            : av_pix_fmt_count_planes(static_cast<AVPixelFormat>(f->format));
    require(count > 0 && count <= AV_NUM_DATA_POINTERS && planes.size() == std::size_t(count),
            "wrap", "Wrong plane count or more than 8 planes");
    for (int i = 0; i < count; ++i) {
        const auto &p = planes[i];
        require(p.data && p.stride != std::numeric_limits<std::ptrdiff_t>::min(), "wrap",
                "Invalid plane");
        if (!audio) {
            require(p.stride >= std::numeric_limits<int>::min() + 1LL &&
                        p.stride <= std::numeric_limits<int>::max(),
                    "wrap", "Stride too large");
            f->linesize[i] = static_cast<int>(p.stride);
        }
        const auto expected = audio ? audio_plane(f, i) : video_plane(f, i);
        require(p.size >= expected.size &&
                    (audio || std::size_t(std::abs(p.stride)) >= std::size_t(expected.row_bytes)),
                "wrap", "Plane backing span is too small");
        auto *start = const_cast<std::uint8_t *>(p.data);
        if (!audio && p.stride < 0)
            start += (expected.rows - 1) * p.stride;
        auto owned = std::make_unique<std::shared_ptr<void>>(owner);
        auto *buffer =
            av_buffer_create(start, p.size, owner_free, owned.get(), AV_BUFFER_FLAG_READONLY);
        if (!buffer)
            fail(ErrorCode::Unavailable, "wrap", "Buffer allocation failed");
        owned.release();
        f->buf[i] = buffer;
        f->data[i] = const_cast<std::uint8_t *>(p.data);
        if (audio)
            f->linesize[0] = static_cast<int>(expected.size);
    }
}
} // namespace
Packet::Packet() : impl_(std::make_shared<Impl>()) {
    impl_->data->time_base = {1, 1000000};
}
Packet Packet::copy(const void *data, std::size_t size) {
    detail::require(size > 0 && data && size <= std::size_t(std::numeric_limits<int>::max()),
                    "packet copy", "Expected nonempty data of representable size");
    Packet out;
    detail::check(av_new_packet(out.impl_->data.get(), static_cast<int>(size)), "Allocate packet");
    std::memcpy(out.impl_->data->data, data, size);
    return out;
}
void Packet::detach() {
    if (!impl_.unique()) {
        auto copy = std::make_shared<Impl>();
        copy->data = detail::ref(impl_->data.get());
        copy->stream = impl_->stream;
        impl_ = std::move(copy);
    }
}
const std::uint8_t *Packet::data() const {
    return impl_->data->data;
}
std::size_t Packet::size() const {
    return impl_->data->size;
}
StreamId Packet::stream_id() const {
    return impl_->stream;
}
bool Packet::is_keyframe() const {
    return (impl_->data->flags & AV_PKT_FLAG_KEY) != 0;
}
std::optional<Timestamp> Packet::pts() const {
    return detail::timestamp(impl_->data->pts, impl_->data->time_base);
}
std::optional<Timestamp> Packet::dts() const {
    return detail::timestamp(impl_->data->dts, impl_->data->time_base);
}
Duration Packet::duration() const {
    return Duration(std::max(std::int64_t{0}, impl_->data->duration),
                    detail::rational(impl_->data->time_base));
}
void Packet::set_pts(std::optional<Timestamp> t) {
    detach();
    if (t)
        packet_base(impl_->data.get(), t->time_base);
    impl_->data->pts = t ? t->ticks : AV_NOPTS_VALUE;
}
void Packet::set_dts(std::optional<Timestamp> t) {
    detach();
    if (t)
        packet_base(impl_->data.get(), t->time_base);
    impl_->data->dts = t ? t->ticks : AV_NOPTS_VALUE;
}
void Packet::set_duration(Duration t) {
    detach();
    impl_->data->duration = av_rescale_q(t.ticks, detail::av(t.time_base),
                                         detail::av(detail::rational(impl_->data->time_base)));
}
void Packet::set_keyframe(bool value) {
    detach();
    if (value)
        impl_->data->flags |= AV_PKT_FLAG_KEY;
    else
        impl_->data->flags &= ~AV_PKT_FLAG_KEY;
}
Packet Packet::clone() const {
    auto p = detail::ref(impl_->data.get());
    detail::check(av_packet_make_writable(p.get()), "Clone packet");
    return detail_Access::packet(std::move(p), impl_->stream);
}

VideoFrame::VideoFrame(std::shared_ptr<Impl> p) : impl_(std::move(p)) {}
AudioFrame::AudioFrame(std::shared_ptr<Impl> p) : impl_(std::move(p)) {}
VideoFrame VideoFrame::allocate(int w, int h, PixelFormat fmt) {
    detail::require(w > 0 && h > 0, "video allocation", "Dimensions must be positive");
    auto f = detail::frame();
    f->width = w;
    f->height = h;
    f->format = detail::av(fmt);
    f->time_base = {1, 1000000};
    f->sample_aspect_ratio = {1, 1};
    detail::check(av_frame_get_buffer(f.get(), 32), "Allocate video frame");
    return detail_Access::video(std::move(f));
}
AudioFrame AudioFrame::allocate(AudioFormat fmt, int count) {
    detail::require(count > 0 && fmt.sample_rate > 0, "audio allocation",
                    "Sample count and rate must be positive");
    auto f = detail::frame();
    f->sample_rate = fmt.sample_rate;
    f->nb_samples = count;
    f->format = detail::av(fmt.samples);
    detail::set_layout(f->ch_layout, fmt.layout);
    f->time_base = {1, fmt.sample_rate};
    detail::check(av_frame_get_buffer(f.get(), 32), "Allocate audio frame");
    return detail_Access::audio(std::move(f));
}
VideoFrame VideoFrame::wrap(VideoFormat fmt, const std::vector<PlaneView> &planes,
                            std::shared_ptr<void> owner) {
    detail::require(fmt.width > 0 && fmt.height > 0, "video wrap", "Invalid dimensions");
    auto f = detail::frame();
    f->width = fmt.width;
    f->height = fmt.height;
    f->format = detail::av(fmt.pixels);
    f->time_base = {1, 1000000};
    f->sample_aspect_ratio = {1, 1};
    wrap_planes(f.get(), planes, std::move(owner), false);
    return detail_Access::video(std::move(f));
}
AudioFrame AudioFrame::wrap(AudioFormat fmt, int count, const std::vector<PlaneView> &planes,
                            std::shared_ptr<void> owner) {
    detail::require(count > 0 && fmt.sample_rate > 0, "audio wrap", "Invalid count or rate");
    auto f = detail::frame();
    f->sample_rate = fmt.sample_rate;
    f->nb_samples = count;
    f->format = detail::av(fmt.samples);
    f->time_base = {1, fmt.sample_rate};
    detail::set_layout(f->ch_layout, fmt.layout);
    wrap_planes(f.get(), planes, std::move(owner), true);
    return detail_Access::audio(std::move(f));
}
#define FRAME_COMMON(Type)                                                                         \
    void Type::detach() {                                                                          \
        if (!impl_.unique())                                                                       \
            impl_ = std::make_shared<Impl>(detail::ref(impl_->data.get()));                        \
    }                                                                                              \
    std::optional<Timestamp> Type::pts() const {                                                   \
        return detail::timestamp(impl_->data->pts, impl_->data->time_base);                        \
    }                                                                                              \
    void Type::set_pts(std::optional<Timestamp> t) {                                               \
        detach();                                                                                  \
        frame_time(impl_->data.get(), t);                                                          \
    }                                                                                              \
    Type Type::clone() const {                                                                     \
        auto p = std::make_shared<Impl>(detail::ref(impl_->data.get()));                           \
        detail::check(av_frame_make_writable(p->data.get()), "Clone frame");                       \
        return Type(std::move(p));                                                                 \
    }
FRAME_COMMON(VideoFrame)
FRAME_COMMON(AudioFrame)
#undef FRAME_COMMON
int VideoFrame::width() const {
    return impl_->data->width;
}
int VideoFrame::height() const {
    return impl_->data->height;
}
PixelFormat VideoFrame::format() const {
    return detail::pixels(impl_->data->format);
}
int VideoFrame::plane_count() const {
    return av_pix_fmt_count_planes(static_cast<AVPixelFormat>(impl_->data->format));
}
PlaneView VideoFrame::plane(int i) const {
    return video_plane(impl_->data.get(), i);
}
MutablePlaneView VideoFrame::writable_plane(int i) {
    (void)plane(i);
    detach();
    detail::check(av_frame_make_writable(impl_->data.get()), "Make video writable");
    auto p = plane(i);
    return {const_cast<std::uint8_t *>(p.data), p.stride, p.size, p.row_bytes, p.rows};
}
Duration VideoFrame::duration() const {
    return Duration(std::max(std::int64_t{0}, impl_->data->duration),
                    detail::rational(impl_->data->time_base));
}
void VideoFrame::set_duration(Duration d) {
    detach();
    impl_->data->duration = av_rescale_q(d.ticks, detail::av(d.time_base),
                                         detail::av(detail::rational(impl_->data->time_base)));
}
ColorInfo VideoFrame::color_info() const {
    auto r = impl_->data->color_range;
    return {r == AVCOL_RANGE_MPEG   ? ColorRange::Limited
            : r == AVCOL_RANGE_JPEG ? ColorRange::Full
                                    : ColorRange::Unspecified};
}
int AudioFrame::sample_rate() const {
    return impl_->data->sample_rate;
}
ChannelLayout AudioFrame::channel_layout() const {
    return detail::layout(impl_->data->ch_layout);
}
SampleFormat AudioFrame::format() const {
    return detail::samples(impl_->data->format);
}
int AudioFrame::sample_count() const {
    return impl_->data->nb_samples;
}
int AudioFrame::plane_count() const {
    return av_sample_fmt_is_planar(static_cast<AVSampleFormat>(impl_->data->format))
               ? impl_->data->ch_layout.nb_channels
               : 1;
}
PlaneView AudioFrame::plane(int i) const {
    return audio_plane(impl_->data.get(), i);
}
MutablePlaneView AudioFrame::writable_plane(int i) {
    (void)plane(i);
    detach();
    detail::check(av_frame_make_writable(impl_->data.get()), "Make audio writable");
    auto p = plane(i);
    return {const_cast<std::uint8_t *>(p.data), p.stride, p.size, p.row_bytes, p.rows};
}
Duration AudioFrame::duration() const {
    return Duration(sample_count(), {1, sample_rate()});
}
} // namespace fflite
