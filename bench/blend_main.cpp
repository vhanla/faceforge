// Micro-benchmark for ff_blend_mask (no models or ONNX Runtime needed).
//   build/ffblend [width height iterations]
#include <faceforge.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

uint32_t rng = 123456789u;

uint8_t next_byte() {
    rng = rng * 1664525u + 1013904223u;
    return static_cast<uint8_t>(rng >> 24);
}

double median_ms(int w, int h, int mw, int mh, uint8_t* dst, const uint8_t* src,
                 const uint8_t* mask, int iters) {
    ff_blend_mask(dst, src, w, h, mask, mw, mh);  // warm up
    std::vector<double> times;
    times.reserve(static_cast<size_t>(iters));
    for (int i = 0; i < iters; ++i) {
        const auto t0 = Clock::now();
        ff_blend_mask(dst, src, w, h, mask, mw, mh);
        const auto t1 = Clock::now();
        times.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    std::sort(times.begin(), times.end());
    return times[times.size() / 2];
}

}  // namespace

int main(int argc, char** argv) {
    int w = 1532, h = 1027, iters = 200;
    if (argc > 2) {
        w = std::atoi(argv[1]);
        h = std::atoi(argv[2]);
    }
    if (argc > 3) iters = std::atoi(argv[3]);
    if (w <= 0 || h <= 0 || iters <= 0) {
        std::fprintf(stderr, "usage: ffblend [width height iterations]\n");
        return 1;
    }

    std::vector<uint8_t> dst(static_cast<size_t>(w) * h * 3);
    std::vector<uint8_t> src(static_cast<size_t>(w) * h * 3);
    std::vector<uint8_t> mask(static_cast<size_t>(w) * h);
    for (auto& b : dst) b = next_byte();
    for (auto& b : src) b = next_byte();
    // A soft brush blob in the middle; the rest stays untouched, like a real
    // retouch stroke.
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const float dx = static_cast<float>(x - w / 2) / (w * 0.25f);
            const float dy = static_cast<float>(y - h / 2) / (h * 0.25f);
            const float d = std::sqrt(dx * dx + dy * dy);
            mask[static_cast<size_t>(y) * w + x] =
                d >= 1.0f ? 0 : static_cast<uint8_t>(255.0f * (1.0f - d));
        }
    }

    const double mp = static_cast<double>(w) * h / 1e6;
    const double same = median_ms(w, h, w, h, dst.data(), src.data(), mask.data(), iters);
    std::printf("same-size %dx%d (%.2f MP): %.3f ms median, %.0f MP/s\n", w, h, mp, same,
                mp / (same / 1000.0));

    const int mw = std::max(1, w / 4), mh = std::max(1, h / 4);
    std::vector<uint8_t> small(static_cast<size_t>(mw) * mh);
    for (auto& b : small) b = next_byte();
    const double scaled =
        median_ms(w, h, mw, mh, dst.data(), src.data(), small.data(), iters);
    std::printf("scaled  %dx%d mask: %.3f ms median, %.0f MP/s\n", mw, mh, scaled,
                mp / (scaled / 1000.0));

    unsigned long long sum = 0;
    for (size_t i = 0; i < dst.size(); i += 997) sum += dst[i];
    std::printf("checksum %llu\n", sum);
    return 0;
}
