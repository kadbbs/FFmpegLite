#include <fflite/fflite.hpp>
#include <iostream>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: encode_file OUTPUT.mkv\n";
        return 2;
    }
    try {
        fflite::VideoEncodeOptions options;
        options.fps = {25, 1};
        options.max_b_frames = 2;
        fflite::VideoEncoder encoder(fflite::Codec::MPEG4, {96, 64}, options);
        fflite::Muxer output(argv[1]);
        const auto stream = output.add_stream(encoder.parameters());
        auto write = [&](const fflite::Packet& packet) { output.write(stream, packet); };
        for (int i = 0; i < 12; ++i) {
            auto frame = fflite::VideoFrame::allocate(96, 64, fflite::PixelFormat::RGBA);
            auto plane = frame.writable_plane(0);
            for (int y = 0; y < plane.rows; ++y) {
                auto* row = plane.data + y * plane.stride;
                for (int x = 0; x < frame.width(); ++x) {
                    row[4*x] = static_cast<std::uint8_t>(30 + x + i);
                    row[4*x + 1] = static_cast<std::uint8_t>(40 + y);
                    row[4*x + 2] = 80;
                    row[4*x + 3] = 255;
                }
            }
            encoder.encode(frame, write);
        }
        encoder.flush(write);
        output.finish();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
