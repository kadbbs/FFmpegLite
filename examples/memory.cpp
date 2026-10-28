#include <cstring>
#include <fflite/fflite.hpp>
#include <iostream>
int main() {
    try {
        fflite::VideoEncodeOptions options;
        options.fps = {25, 1};
        options.max_b_frames = 2;
        fflite::VideoEncoder encoder(fflite::Codec::MPEG4, {96, 64}, options);
        fflite::VideoDecoder decoder(encoder.parameters());
        int count = 0;
        auto consume = [&](const fflite::VideoFrame &) { ++count; };
        auto deliver = [&](const fflite::Packet &packet) { decoder.decode(packet, consume); };
        for (int i = 0; i < 12; ++i) {
            auto frame = fflite::VideoFrame::allocate(96, 64, fflite::PixelFormat::RGBA);
            auto plane = frame.writable_plane(0);
            for (int row = 0; row < plane.rows; ++row)
                std::memset(plane.data + row * plane.stride, 50 + i * 8, plane.row_bytes);
            encoder.encode(frame, deliver);
        }
        encoder.flush(deliver);
        decoder.flush(consume);
        std::cout << count << " frames encoded and decoded entirely in memory\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
