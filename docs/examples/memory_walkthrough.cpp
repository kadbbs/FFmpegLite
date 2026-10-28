#include <fflite/fflite.hpp>
#include <iostream>

int main() {
    try {
        // 1. 无输入 PTS 时，编码器按 25 fps 生成时间。
        fflite::VideoEncodeOptions options;
        options.fps = {25, 1};
        options.max_b_frames = 2; // 故意启用延迟，验证 flush。
        fflite::VideoEncoder encoder(fflite::Codec::MPEG4, {96, 64}, options);

        // 2. 用编码器的完整配置创建配套解码器。
        fflite::VideoDecoder decoder(encoder.parameters());
        int decoded_count = 0;

        // 3. 最终收到原始图片时计数。创建回调不会立即执行它。
        auto on_decoded_frame = [&](const fflite::VideoFrame& frame) {
            ++decoded_count;
            if (frame.width() != 96 || frame.height() != 64) {
                throw std::runtime_error("Unexpected decoded size");
            }
        };

        // 4. 编码器产生压缩包时，将其送进解码器。
        auto on_encoded_packet = [&](const fflite::Packet& packet) {
            decoder.decode(packet, on_decoded_frame);
        };

        for (int i = 0; i < 12; ++i) {
            // 5. 创建原始 RGBA 图片，填满有效像素。
            auto frame = fflite::VideoFrame::allocate(96, 64, fflite::PixelFormat::RGBA);
            auto plane = frame.writable_plane(0);
            for (int y = 0; y < plane.rows; ++y) {
                auto* row = plane.data + y * plane.stride;
                for (int x = 0; x < frame.width(); ++x) {
                    row[4*x] = static_cast<std::uint8_t>(50 + i*8);
                    row[4*x + 1] = row[4*x];
                    row[4*x + 2] = row[4*x];
                    row[4*x + 3] = 255;
                }
            }
            // 6. 产生包时调用 on_encoded_packet，不保证每次立即产生包。
            encoder.encode(frame, on_encoded_packet);
        }

        // 7. 先把全部编码包交付，再通知解码器输入结束。
        encoder.flush(on_encoded_packet);
        decoder.flush(on_decoded_frame);
        std::cout << decoded_count << " decoded frames\n";
        return decoded_count == 12 ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
