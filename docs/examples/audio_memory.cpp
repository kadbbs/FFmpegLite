#include <fflite/fflite.hpp>
#include <cstring>
#include <iostream>

int main() {
    try {
        // 输入：48 kHz 单声道 S16。输出：44.1 kHz 双声道 AAC。
        fflite::AudioProcessor audio;
        audio.resample(44100, 2).volume(0.8);
        fflite::AudioEncoder encoder(fflite::Codec::AAC, fflite::AudioFormat::stereo(44100));
        std::size_t encoded_bytes = 0;
        auto on_packet = [&](const fflite::Packet& packet) { encoded_bytes += packet.size(); };
        auto on_audio = [&](const fflite::AudioFrame& frame) { encoder.encode(frame, on_packet); };

        for (int i = 0; i < 12; ++i) {
            auto frame = fflite::AudioFrame::allocate(
                fflite::AudioFormat::mono(48000, fflite::SampleFormat::S16), 1920);
            auto plane = frame.writable_plane(0);
            std::memset(plane.data, 0, plane.size); // S16 静音。实际项目替换为采集数据。
            frame.set_pts(fflite::Timestamp{static_cast<std::int64_t>(i) * 1920, {1, 48000}});
            audio.process(frame, on_audio);
        }
        audio.flush(on_audio); // 先送完重采样残留。
        encoder.flush(on_packet);
        std::cout << encoded_bytes << " encoded bytes\n";
        return encoded_bytes > 0 ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
