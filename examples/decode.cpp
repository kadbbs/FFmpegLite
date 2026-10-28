#include <fflite/fflite.hpp>
#include <iostream>
int main(int argc, char **argv) {
    if (argc != 2) {
        std::cerr << "Usage: fflite_decode INPUT\n";
        return 2;
    }
    try {
        fflite::VideoReader reader(argv[1]);
        fflite::VideoProcessor video;
        video.resize(640, 480).format(fflite::PixelFormat::RGBA);
        int count = 0;
        while (auto frame = reader.read()) {
            auto rgba = video.process(*frame);
            // Pass rgba.plane(0).data and stride to your renderer/algorithm.
            ++count;
        }
        std::cout << count << " frames\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
