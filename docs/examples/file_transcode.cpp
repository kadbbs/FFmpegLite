#include <fflite/fflite.hpp>
#include <iostream>

// 只转码首选视频流，不保留音频/原字幕。输出需为不存在的 MP4/MKV 路径。
int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "Usage: file_transcode INPUT OUTPUT\n";
        return 2;
    }
    try {
        fflite::Demuxer input(argv[1]);
        const auto source = input.video();
        fflite::VideoDecoder decoder(source);
        fflite::VideoProcessor video;
        video.resize(640, 480); // 默认 Fit，保持显示比例并填黑。

        fflite::VideoEncodeOptions options;
        options.fps = {25, 1}; // 仅为缺失 PTS 的帧提供生成策略，不覆盖有效 PTS。
        fflite::VideoEncoder encoder(fflite::Codec::MPEG4, {640, 480}, options);
        fflite::Muxer output(argv[2]);
        const auto target = output.add_stream(encoder.parameters());

        auto write_packet = [&](const fflite::Packet& packet) { output.write(target, packet); };
        auto process_frame = [&](const fflite::VideoFrame& frame) {
            auto resized = video.process(frame);
            encoder.encode(resized, write_packet);
        };

        while (auto packet = input.read()) {
            if (packet->stream_id() == source.id()) decoder.decode(*packet, process_frame);
        }
        decoder.flush(process_frame);
        encoder.flush(write_packet);
        output.finish();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
