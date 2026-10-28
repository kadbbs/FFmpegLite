#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fflite/fflite.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>

using namespace fflite;
namespace {
void expect(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
template <class F> void error(ErrorCode code, F &&fn) {
    bool caught = false;
    try {
        fn();
    } catch (const Error &e) {
        caught = true;
        expect(e.code() == code, "Wrong error category");
    }
    expect(caught, "Expected fflite error");
}
VideoFrame image(int w = 96, int h = 64) {
    auto out = VideoFrame::allocate(w, h, PixelFormat::RGBA);
    auto p = out.writable_plane(0);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            auto *pixel = p.data + y * p.stride + x * 4;
            pixel[0] = 30 + x;
            pixel[1] = 40 + y;
            pixel[2] = 80;
            pixel[3] = 255;
        }
    return out;
}
AudioFrame sound(int count = 1920, int rate = 48000) {
    auto out = AudioFrame::allocate(AudioFormat::mono(rate, SampleFormat::S16), count);
    auto p = out.writable_plane(0);
    for (int i = 0; i < count; ++i) {
        const std::int16_t value = static_cast<std::int16_t>((i % 200 - 100) * 200);
        std::memcpy(p.data + i * 2, &value, 2);
    }
    return out;
}
struct Encoded {
    CodecParameters parameters;
    std::vector<Packet> packets;
};
Encoded video_packets(int count = 12) {
    VideoEncodeOptions options;
    options.fps = {25, 1};
    options.max_b_frames = 2;
    VideoEncoder encoder(Codec::MPEG4, {96, 64}, options);
    Encoded out{encoder.parameters(), {}};
    auto collect = [&](const Packet &p) { out.packets.push_back(p); };
    for (int i = 0; i < count; ++i) {
        auto f = image();
        f.set_pts(Timestamp(200 + i * 40, {1, 1000}));
        encoder.encode(f, collect);
    }
    expect(out.packets.size() < std::size_t(count), "Fixture must exercise delayed encoding");
    encoder.flush(collect);
    encoder.flush(collect);
    expect(out.packets.size() == std::size_t(count), "Encoder lost delayed frames");
    return out;
}
Encoded audio_packets() {
    AudioEncoder encoder(Codec::PCM_S16LE, AudioFormat::mono(48000, SampleFormat::S16));
    Encoded out{encoder.parameters(), {}};
    auto collect = [&](const Packet &p) { out.packets.push_back(p); };
    for (int i = 0; i < 12; ++i) {
        auto f = sound();
        f.set_pts(Timestamp(9600 + i * 1920, {1, 48000}));
        encoder.encode(f, collect);
    }
    encoder.flush(collect);
    return out;
}

void memory_video() {
    auto encoded = video_packets();
    VideoDecoder decoder(encoded.parameters);
    std::vector<VideoFrame> frames;
    auto consume = [&](const VideoFrame &f) { frames.push_back(f); };
    for (const auto &p : encoded.packets) {
        auto from_network = Packet::copy(p.data(), p.size());
        from_network.set_pts(p.pts());
        from_network.set_dts(p.dts());
        from_network.set_duration(p.duration());
        decoder.decode(from_network, consume);
    }
    expect(frames.size() < 12, "Fixture must exercise delayed decoding");
    decoder.flush(consume);
    decoder.flush(consume);
    expect(frames.size() == 12, "Decoder lost frames");
    for (int i = 0; i < 12; ++i) {
        expect(frames[i].width() == 96 && frames[i].height() == 64, "Wrong decoded dimensions");
        expect(frames[i].pts()->rescaled({1, 1000}).ticks == 200 + i * 40, "PTS changed");
    }
    const auto first = frames.front().plane(0).data[0];
    decoder.reset();
    for (const auto &p : encoded.packets)
        decoder.decode(p, consume);
    decoder.flush(consume);
    expect(frames.size() == 24 && frames.front().plane(0).data[0] == first,
           "Reset or retained frame ownership failed");
}

void ownership() {
    auto original = image();
    auto copy = original;
    auto changed = copy.writable_plane(0);
    changed.data[0] = 255;
    expect(original.plane(0).data[0] == 30 && copy.plane(0).data[0] == 255,
           "Video copy-on-write failed");
    copy.set_pts(Timestamp(10, {1, 25}));
    expect(!original.pts(), "Frame metadata must detach");
    auto deep = original.clone();
    expect(deep.plane(0).data != original.plane(0).data, "clone must copy data");
    auto audio = sound();
    auto other = audio;
    other.writable_plane(0).data[0] ^= 0xff;
    expect(audio.plane(0).data[0] != other.plane(0).data[0], "Audio copy-on-write failed");
    const std::uint8_t bytes[] = {1, 2, 3};
    auto p = Packet::copy(bytes, 3);
    p.set_pts(Timestamp(90, {1, 90000}));
    p.set_dts(Timestamp(0, {1, 1000}));
    auto q = p;
    q.set_pts(Timestamp(2, {1, 1000}));
    expect(p.pts()->rescaled({1, 1000}).ticks == 1 && q.pts()->ticks == 2,
           "Packet metadata copy-on-write failed");
    expect(p.clone().data() != p.data(), "Packet deep clone failed");
    auto owner = std::make_shared<std::vector<std::uint8_t>>(4 * 4 * 3, 17);
    std::weak_ptr<void> lifetime = owner;
    auto wrapped =
        VideoFrame::wrap({4, 3, PixelFormat::RGBA}, {{owner->data() + 32, -16, 48, 16, 3}}, owner);
    owner.reset();
    expect(!lifetime.expired() && wrapped.plane(0).stride == -16,
           "External owner or negative stride lost");
    auto held = wrapped;
    wrapped.writable_plane(0).data[0] = 42;
    expect(held.plane(0).data[0] == 17, "Read-only wrapped frame must copy on write");
    auto aowner = std::make_shared<std::vector<std::uint8_t>>(20, 12);
    auto awrap = AudioFrame::wrap(AudioFormat::mono(48000, SampleFormat::S16), 10,
                                  {{aowner->data(), 20, 20, 20, 1}}, aowner);
    aowner.reset();
    expect(awrap.plane(0).size == 20, "External audio wrapping failed");
    expect(Timestamp::from(std::chrono::milliseconds(100)).rescaled({1, 48000}).ticks == 4800,
           "Chrono conversion failed");
    error(ErrorCode::InvalidArgument, [] { Rational invalid(0, 1); });
}

void video_processor() {
    auto source = image();
    source.set_pts(Timestamp(5, {1, 25}));
    VideoProcessor crop;
    crop.crop(16, 8, 48, 32);
    auto out = crop.process(source);
    expect(out.width() == 48 && out.height() == 32 && out.plane(0).data[0] == 46 &&
               out.plane(0).data[1] == 48,
           "Crop selected wrong pixels");
    expect(out.pts()->ticks == 5, "Processor changed timestamp");
    VideoProcessor fit;
    fit.resize(64, 64);
    auto letterboxed = fit.process(source);
    expect(letterboxed.plane(0).data[0] == 0, "Fit must add black borders");
    expect(letterboxed.plane(0).data[20 * letterboxed.plane(0).stride] > 0,
           "Fit lost image content");
    VideoProcessor fill;
    fill.resize(64, 64, ResizeMode::Fill);
    expect(fill.process(source).plane(0).data[0] > 0, "Fill must cover output");
    VideoProcessor ordered;
    ordered.resize(48, 32, ResizeMode::Stretch).crop(8, 8, 32, 16).format(PixelFormat::YUV420P);
    auto yuv = ordered.process(source);
    expect(yuv.width() == 32 && yuv.height() == 16 && yuv.plane_count() == 3,
           "Ordered processing or format conversion failed");
    auto odd = image(17, 13);
    VideoProcessor rgb;
    rgb.crop(1, 1, 15, 11);
    expect(rgb.process(odd).width() == 15, "RGB must allow odd crop coordinates");
    VideoProcessor bad;
    bad.crop(100, 0, 10, 10);
    error(ErrorCode::InvalidArgument, [&] { bad.process(source); });
    error(ErrorCode::InvalidState, [&] { crop.resize(2, 2); });
    auto saved = letterboxed;
    fit.process(source);
    expect(saved.plane(0).data[0] == 0, "Processor overwrote retained output");
}

void audio_processor() {
    auto original = sound();
    AudioProcessor volume;
    volume.volume(0.5).format(SampleFormat::S16);
    std::vector<AudioFrame> quiet;
    auto collect = [&](const AudioFrame &f) { quiet.push_back(f); };
    volume.process(original, collect);
    volume.flush(collect);
    volume.flush(collect);
    expect(quiet.size() == 1 && quiet[0].sample_count() == 1920, "Volume changed sample count");
    for (int i = 0; i < 1920; ++i) {
        std::int16_t a, b;
        std::memcpy(&a, original.plane(0).data + i * 2, 2);
        std::memcpy(&b, quiet[0].plane(0).data + i * 2, 2);
        expect(std::abs(int(b) - int(a) / 2) <= 1, "Wrong volume gain");
    }
    AudioProcessor resample;
    resample.resample(24000, 2).format(SampleFormat::S16);
    int count = 0;
    std::optional<Timestamp> first;
    auto consume = [&](const AudioFrame &f) {
        count += f.sample_count();
        expect(f.sample_rate() == 24000 && f.channel_layout().channels() == 2,
               "Wrong audio target");
        if (!first)
            first = f.pts();
    };
    for (int i = 0; i < 12; ++i) {
        auto f = sound();
        f.set_pts(Timestamp(9600 + i * 1920, {1, 48000}));
        resample.process(f, consume);
    }
    resample.flush(consume);
    expect(count == 11520 && first->rescaled({1, 24000}).ticks == 4800,
           "Resampling lost tail or changed start");
    resample.reset();
    count = 0;
    resample.process(sound(), consume);
    resample.flush(consume);
    expect(count == 960, "Reset failed");
    AudioProcessor changing;
    std::vector<AudioFrame> changed;
    auto retain = [&](const AudioFrame &f) { changed.push_back(f); };
    changing.process(sound(480, 48000), retain);
    changing.process(sound(240, 24000), retain);
    changing.flush(retain);
    expect(changed.size() == 2 && changed[1].pts()->rescaled({1, 24000}).ticks == 240,
           "Dynamic sample rate changed the generated timeline");
}

void audio_codec() {
    auto encoded = audio_packets();
    AudioDecoder decoder(encoded.parameters);
    int samples = 0;
    auto collect = [&](const AudioFrame &f) {
        samples += f.sample_count();
        expect(f.sample_rate() == 48000 && f.channel_layout().channels() == 1,
               "Audio decoding failed");
    };
    for (const auto &p : encoded.packets)
        decoder.decode(p, collect);
    decoder.flush(collect);
    expect(samples == 23040, "PCM roundtrip sample count changed");
    AudioEncoder aac(Codec::AAC, AudioFormat::stereo(44100));
    AudioProcessor resample;
    resample.resample(44100, 2);
    AudioDecoder aac_decoder(aac.parameters());
    int decoded = 0;
    auto receive = [&](const AudioFrame &f) { decoded += f.sample_count(); };
    auto send = [&](const Packet &p) { aac_decoder.decode(p, receive); };
    auto encode = [&](const AudioFrame &f) { aac.encode(f, send); };
    for (int i = 0; i < 12; ++i)
        resample.process(sound(), encode);
    resample.flush(encode);
    aac.flush(send);
    aac_decoder.flush(receive);
    expect(decoded >= 21168 && decoded <= 21168 + 3072,
           "AAC resample/encode/decode pipeline failed");
}

void containers(const std::filesystem::path &directory) {
    auto video = video_packets(), audio = audio_packets();
    const auto file = directory / "streams.mkv";
    SubtitleTrack subtitles;
    subtitles.add({Timestamp(200, {1, 1000}), Duration(200, {1, 1000}), "Hello, 开发者!"});
    {
        Muxer output(file.string());
        auto v = output.add_stream(video.parameters);
        auto a = output.add_stream(audio.parameters);
        output.add_subtitles(subtitles, {"eng", "Test"});
        for (std::size_t i = 0; i < video.packets.size(); ++i) {
            output.write(v, video.packets[i]);
            output.write(a, audio.packets[i]);
        }
        output.finish();
        output.finish();
    }
    CodecParameters parameters;
    StreamId input_video;
    std::vector<Packet> retained;
    bool text = false;
    int audio_count = 0;
    {
        Demuxer input(file.string());
        auto stream = input.video();
        parameters = stream.parameters();
        input_video = stream.id();
        expect(input.streams().size() == 3, "Muxer lost streams");
        while (auto p = input.read()) {
            if (p->stream_id() == stream.id())
                retained.push_back(*p);
            else if (p->stream_id() == input.audio().id())
                ++audio_count;
            else {
                std::string cue(reinterpret_cast<const char *>(p->data()), p->size());
                text |= cue == "Hello, 开发者!";
            }
        }
        expect(!input.read(), "EOF must be stable");
    }
    expect(retained.size() == 12 && audio_count == 12 && text, "Demux lost packets or subtitles");
    expect(!parameters.extradata().empty() && retained.front().stream_id() == input_video,
           "Metadata/packets did not survive demuxer destruction");
    {
        VideoDecoder decoder(parameters);
        int n = 0;
        auto sink = [&](const VideoFrame &) { ++n; };
        for (const auto &p : retained)
            decoder.decode(p, sink);
        decoder.flush(sink);
        expect(n == 12, "Retained parameters cannot decode");
    }
    {
        VideoReader reader(file.string());
        int n = 0;
        while (reader.read())
            ++n;
        expect(n == 12 && !reader.read(), "VideoReader lost delayed frames");
    }
    {
        Demuxer input(file.string());
        input.seek(Timestamp(200, {1, 1000}));
        expect(bool(input.read()), "Seek failed");
    }
    const auto mp4 = directory / "captions.mp4";
    {
        Muxer output(mp4.string());
        auto v = output.add_stream(video.parameters);
        output.add_subtitles(subtitles, {"eng", "Captions"});
        for (const auto &p : video.packets)
            output.write(v, p);
        output.finish();
    }
    {
        Demuxer input(mp4.string());
        bool found = false;
        for (const auto &s : input.streams())
            if (s.media_type() == MediaType::Subtitle)
                expect(s.parameters().codec() == Codec::MovText, "MP4 subtitle format wrong");
        while (auto p = input.read()) {
            std::string data(reinterpret_cast<const char *>(p->data()), p->size());
            if (data.find("Hello, 开发者!") != std::string::npos) {
                found = true;
                expect(p->pts()->rescaled({1, 1000}).ticks == 200, "Subtitle time changed");
            }
        }
        expect(found, "MP4 subtitle content lost");
    }
    const auto srt = directory / "captions 'with': punctuation.srt";
    {
        std::ofstream out(srt);
        out << "1\n00:00:00,200 --> 00:00:00,400\nHello, 开发者!\n\n";
    }
    auto loaded = SubtitleTrack::load(srt.string());
    expect(loaded.cues().size() == 1 && loaded.cues()[0].text == "Hello, 开发者!",
           "SRT loading failed");
}

void failures(const std::filesystem::path &directory) {
    auto encoded = video_packets();
    VideoDecoder decoder(encoded.parameters);
    bool original_exception = false;
    struct CallbackFailure {};
    try {
        for (const auto &p : encoded.packets)
            decoder.decode(p, [](const VideoFrame &) { throw CallbackFailure{}; });
        decoder.flush([](const VideoFrame &) { throw CallbackFailure{}; });
    } catch (const CallbackFailure &) {
        original_exception = true;
    }
    expect(original_exception, "Callback exception was swallowed");
    error(ErrorCode::InvalidState, [&] { decoder.reset(); });
    VideoDecoder drained(encoded.parameters);
    for (const auto &p : encoded.packets)
        drained.decode(p, [](const VideoFrame &) {});
    drained.flush([](const VideoFrame &) {});
    error(ErrorCode::InvalidState,
          [&] { drained.decode(encoded.packets[0], [](const VideoFrame &) {}); });
    error(ErrorCode::InvalidArgument, [&] { VideoDecoder wrong(Codec::AAC); });
    error(ErrorCode::Unavailable, [] {
        VideoEncodeOptions o;
        o.implementation = "missing_encoder";
        VideoEncoder e(Codec::MPEG4, {96, 64}, o);
    });
    error(ErrorCode::InvalidArgument, [] {
        VideoEncoder e(Codec::MPEG4, {96, 64});
        e.encode(image(), [](const Packet &) {});
    });
    Muxer one((directory / "one.mkv").string()), two((directory / "two.mkv").string());
    auto id = one.add_stream(encoded.parameters);
    two.add_stream(encoded.parameters);
    error(ErrorCode::InvalidArgument, [&] { two.write(id, encoded.packets.front()); });
    VideoProcessor video;
    error(ErrorCode::Unavailable, [&] { video.subtitles(SubtitleTrack{}); });
    AudioProcessor audio;
    error(ErrorCode::InvalidArgument,
          [&] { audio.volume(std::numeric_limits<double>::quiet_NaN()); });
}
int function_frames = 0;
void plain_callback(const VideoFrame &) {
    ++function_frames;
}
void callback_forms() {
    auto encoded = video_packets();
    VideoDecoder decoder(encoded.parameters);
    for (const auto &p : encoded.packets)
        decoder.decode(p, plain_callback);
    decoder.flush(plain_callback);
    expect(function_frames == 12, "Plain function callbacks failed");
    VideoDecoder other(encoded.parameters);
    int count = 0;
    const auto callable = [&](const VideoFrame &) { ++count; };
    for (const auto &p : encoded.packets)
        other.decode(p, callable);
    other.flush(callable);
    expect(count == 12, "Const callbacks failed");
}
} // namespace
int main(int argc, char **argv) {
    const auto directory =
        std::filesystem::temp_directory_path() /
        ("fflite-core-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        std::filesystem::create_directories(directory);
        expect(argc == 2, "Specify a test case");
        const std::string name = argv[1];
        if (name == "memory_video")
            memory_video();
        else if (name == "ownership")
            ownership();
        else if (name == "video_processor")
            video_processor();
        else if (name == "audio_processor")
            audio_processor();
        else if (name == "audio_codec")
            audio_codec();
        else if (name == "containers")
            containers(directory);
        else if (name == "failures")
            failures(directory);
        else if (name == "callback_forms")
            callback_forms();
        else
            throw std::runtime_error("Unknown test case");
        std::filesystem::remove_all(directory);
        std::cout << name << " passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << "\nArtifacts: " << directory << '\n';
        return 1;
    }
}
