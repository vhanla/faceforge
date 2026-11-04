// faceforge benchmark: real latency, VRAM and RSS measurements per stage.
#include <faceforge.h>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

struct Args {
    std::string models = "models";
    std::string target = "assets/target_t1.jpg";
    std::string source = "assets/source_lena.jpg";
    std::string stage = "all";
    std::string provider = "cuda";
    std::string enhancer = "gfpgan";
    std::string csv;
    int repeats = 5;
    int warmup = 2;
    int det_size = 640;
    int threads = 0;
    int sequential = 0;
    long long gpu_mem_limit = 0;
};

void usage() {
    std::cout
        << "ffbench [options]\n"
        << "  --models DIR        model directory (default models)\n"
        << "  --target IMG        target/source-of-swap image\n"
        << "  --source IMG        identity image\n"
        << "  --stage NAME        detect|embed|swap|enhance|full|all (default all)\n"
        << "  --provider NAME     cuda|cpu|auto (default cuda)\n"
        << "  --enhancer NAME     none|gfpgan|esrgan2|esrgan4 (default gfpgan)\n"
        << "  --repeats N         warm repetitions (default 5)\n"
        << "  --warmup N          warmup runs (default 2)\n"
        << "  --det-size N        SCRFD input side (default 640)\n"
        << "  --threads N         CPU intra-op threads (0 = ORT default)\n"
        << "  --sequential        unload sessions between stages\n"
        << "  --gpu-mem-limit N   CUDA arena limit in MiB (0 = default)\n"
        << "  --csv PATH          append results as CSV\n";
}

bool parse(int argc, char** argv, Args& a) {
    for (int i = 1; i < argc; ++i) {
        const std::string k = argv[i];
        auto next = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "missing value for " << name << "\n";
                return {};
            }
            return argv[++i];
        };
        if (k == "--models") a.models = next("--models");
        else if (k == "--target") a.target = next("--target");
        else if (k == "--source") a.source = next("--source");
        else if (k == "--stage") a.stage = next("--stage");
        else if (k == "--provider") a.provider = next("--provider");
        else if (k == "--enhancer") a.enhancer = next("--enhancer");
        else if (k == "--csv") a.csv = next("--csv");
        else if (k == "--repeats") a.repeats = std::stoi(next("--repeats"));
        else if (k == "--warmup") a.warmup = std::stoi(next("--warmup"));
        else if (k == "--det-size") a.det_size = std::stoi(next("--det-size"));
        else if (k == "--threads") a.threads = std::stoi(next("--threads"));
        else if (k == "--sequential") a.sequential = 1;
        else if (k == "--gpu-mem-limit") a.gpu_mem_limit = std::stoll(next("--gpu-mem-limit"));
        else if (k == "-h" || k == "--help") { usage(); return false; }
        else { std::cerr << "unknown option: " << k << "\n"; usage(); return false; }
    }
    return true;
}

struct VramSampler {
    std::atomic<bool> stop{false};
    std::atomic<float> peak{0.0f};
    std::thread thread;

    void start() {
        float used = 0, total = 0, rss = 0;
        ff_memory_info(&used, &total, &rss);
        baseline = used;
        peak = used;
        thread = std::thread([this] {
            float u = 0, t = 0, r = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                ff_memory_info(&u, &t, &r);
                const float p = peak.load(std::memory_order_relaxed);
                if (u > p) peak.store(u, std::memory_order_relaxed);
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        });
    }
    void finish() {
        stop = true;
        if (thread.joinable()) thread.join();
        float u = 0, t = 0, r = 0;
        ff_memory_info(&u, &t, &r);
        peak = std::max(peak.load(), u);
        end = u;
        end_rss = r;
    }
    ~VramSampler() {
        if (thread.joinable()) {
            stop = true;
            thread.join();
        }
    }
    float baseline = 0;
    float end = 0;
    float end_rss = 0;
};

struct Row {
    std::string stage;
    double cold_ms = 0;
    double min_ms = 0;
    double median_ms = 0;
    double mean_ms = 0;
    double p95_ms = 0;
    float vram_base = 0;
    float vram_peak = 0;
    float vram_end = 0;
    float rss = 0;
    int faces = 0;
};

double percentile(std::vector<double> v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    const double idx = p * (v.size() - 1);
    const size_t lo = static_cast<size_t>(std::floor(idx));
    const size_t hi = static_cast<size_t>(std::ceil(idx));
    return v[lo] + (v[hi] - v[lo]) * (idx - lo);
}

void print_row(const Row& r, const std::string& provider) {
    std::printf("%-8s %-4s cold %8.1f ms | warm min %8.1f | med %8.1f | mean %8.1f | "
                "p95 %8.1f ms | VRAM %7.0f -> %7.0f MiB | RSS %6.0f | faces %d\n",
                r.stage.c_str(), provider.c_str(), r.cold_ms, r.min_ms, r.median_ms,
                r.mean_ms, r.p95_ms, r.vram_base, r.vram_peak, r.rss, r.faces);
}

void write_csv(const std::string& path, const Args& args, const Row& r,
               const std::string& provider, const std::string& model_version) {
    if (path.empty()) return;
    const bool exists = std::ifstream(path).good();
    std::ofstream f(path, std::ios::app);
    if (!exists) {
        f << "stage,provider,enhancer,cold_ms,min_ms,median_ms,mean_ms,p95_ms,"
             "vram_baseline_mb,vram_peak_mb,vram_end_mb,rss_mb,faces,"
             "det_size,threads,sequential,onnxruntime\n";
    }
    // Detect/embed/swap never use an enhancer; keep the CSV honest.
    const bool uses_enhancer = r.stage == "enhance" || r.stage == "full";
    f << r.stage << ',' << provider << ','
      << (uses_enhancer ? args.enhancer : std::string("none")) << ','
      << r.cold_ms << ',' << r.min_ms << ',' << r.median_ms << ',' << r.mean_ms << ','
      << r.p95_ms << ',' << r.vram_base << ',' << r.vram_peak << ','
      << r.vram_end << ',' << r.rss << ',' << r.faces << ',' << args.det_size << ','
      << args.threads << ',' << args.sequential << ',' << model_version << '\n';
}

struct CroppedFace {
    cv::Mat image;
    ff_face face{};
};

CroppedFace crop_first_face(const cv::Mat& img, const ff_face& face, int out_size) {
    const float bw = face.bbox[2] - face.bbox[0];
    const float bh = face.bbox[3] - face.bbox[1];
    const float side = std::max(bw, bh) * 1.8f;
    const float cx = (face.bbox[0] + face.bbox[2]) * 0.5f;
    const float cy = (face.bbox[1] + face.bbox[3]) * 0.5f;
    int x = static_cast<int>(cx - side * 0.5f);
    int y = static_cast<int>(cy - side * 0.5f);
    int w = static_cast<int>(side);
    int h = static_cast<int>(side);
    x = std::max(0, std::min(x, img.cols - 1));
    y = std::max(0, std::min(y, img.rows - 1));
    w = std::min(w, img.cols - x);
    h = std::min(h, img.rows - y);

    CroppedFace out;
    cv::resize(img(cv::Rect(x, y, w, h)), out.image,
               cv::Size(out_size, out_size), 0, 0, cv::INTER_AREA);
    const float sx = static_cast<float>(out_size) / w;
    const float sy = static_cast<float>(out_size) / h;
    out.face = face;
    out.face.bbox[0] = (face.bbox[0] - x) * sx;
    out.face.bbox[1] = (face.bbox[1] - y) * sy;
    out.face.bbox[2] = (face.bbox[2] - x) * sx;
    out.face.bbox[3] = (face.bbox[3] - y) * sy;
    for (int i = 0; i < 5; ++i) {
        out.face.kps[i * 2 + 0] = (face.kps[i * 2 + 0] - x) * sx;
        out.face.kps[i * 2 + 1] = (face.kps[i * 2 + 1] - y) * sy;
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    Args args;
    if (!parse(argc, argv, args)) return 0;

    cv::Mat target = cv::imread(args.target, cv::IMREAD_COLOR);
    cv::Mat source = cv::imread(args.source, cv::IMREAD_COLOR);
    if (target.empty()) {
        std::cerr << "cannot read target: " << args.target << "\n";
        return 1;
    }
    if (source.empty()) {
        std::cerr << "cannot read source: " << args.source << "\n";
        return 1;
    }
    if (!target.isContinuous()) target = target.clone();
    if (!source.isContinuous()) source = source.clone();

    ff_options opt;
    ff_options_default(&opt);
    if (args.provider == "cpu") opt.provider = FF_PROVIDER_CPU;
    else if (args.provider == "cuda") opt.provider = FF_PROVIDER_CUDA;
    else opt.provider = FF_PROVIDER_AUTO;
    opt.intra_threads = args.threads;
    opt.sequential = args.sequential;
    opt.det_size = args.det_size;
    opt.gpu_mem_limit = args.gpu_mem_limit;
    if (args.enhancer == "gfpgan") opt.enhance = FF_ENHANCE_GFPGAN;
    else if (args.enhancer == "esrgan2") opt.enhance = FF_ENHANCE_ESRGAN_X2;
    else if (args.enhancer == "esrgan4") opt.enhance = FF_ENHANCE_ESRGAN_X4;
    else opt.enhance = FF_ENHANCE_NONE;

    char err[1024] = {0};
    const auto t_create = Clock::now();
    ff_engine* engine = ff_engine_create(args.models.c_str(), &opt, err, sizeof(err));
    const double create_ms = ms_since(t_create);
    if (!engine) {
        std::cerr << "engine creation failed: " << err << "\n";
        return 1;
    }
    const std::string provider = ff_engine_provider(engine);

    float vram_used = 0, vram_total = 0, rss = 0;
    ff_memory_info(&vram_used, &vram_total, &rss);

    std::printf("faceforge benchmark\n");
    std::printf("  provider      : %s\n", provider.c_str());
    std::printf("  target        : %s (%dx%d)\n", args.target.c_str(), target.cols, target.rows);
    std::printf("  source        : %s (%dx%d)\n", args.source.c_str(), source.cols, source.rows);
    std::printf("  enhancer      : %s\n", args.enhancer.c_str());
    std::printf("  engine create : %.1f ms\n", create_ms);
    std::printf("  GPU total     : %.0f MiB, used before: %.0f MiB, RSS %.0f MiB\n\n",
                vram_total, vram_used, rss);

    ff_face faces[16];
    int32_t face_count = 0;
    if (ff_detect(engine, target.data, target.cols, target.rows,
                  static_cast<int32_t>(target.step), faces, 16, &face_count) != FF_OK) {
        std::cerr << "detection failed: " << ff_engine_last_error(engine) << "\n";
        return 1;
    }
    if (face_count == 0) {
        std::cerr << "no faces in target\n";
        return 1;
    }
    const ff_face best = faces[0];

    // The identity embedding must come from a face detected in the source
    // image; landmarks from the target cannot be reused.
    ff_face src_faces[16];
    int32_t src_count = 0;
    if (ff_detect(engine, source.data, source.cols, source.rows,
                  static_cast<int32_t>(source.step), src_faces, 16, &src_count) != FF_OK ||
        src_count == 0) {
        std::cerr << "no face in source: " << ff_engine_last_error(engine) << "\n";
        return 1;
    }
    const ff_face source_best = src_faces[0];
    float embedding[512];
    if (ff_embed(engine, source.data, source.cols, source.rows,
                 static_cast<int32_t>(source.step), &source_best, embedding) != FF_OK) {
        std::cerr << "embedding failed: " << ff_engine_last_error(engine) << "\n";
        return 1;
    }

    CroppedFace crop = crop_first_face(target, best, 512);

    const bool all = args.stage == "all";
    const bool want_detect = all || args.stage == "detect";
    const bool want_embed = all || args.stage == "embed";
    const bool want_swap = all || args.stage == "swap";
    const bool want_enhance = (all || args.stage == "enhance") && opt.enhance != FF_ENHANCE_NONE;
    const bool want_full = all || args.stage == "full";

    std::vector<Row> rows;

    auto measure = [&](const std::string& name, int faces_n, auto&& fn) {
        ff_engine_unload(engine);
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        VramSampler sampler;
        sampler.start();

        auto t0 = Clock::now();
        if (fn() != FF_OK) {
            sampler.finish();
            std::cerr << name << " failed: " << ff_engine_last_error(engine) << "\n";
            return false;
        }
        Row r;
        r.stage = name;
        r.cold_ms = ms_since(t0);
        for (int i = 0; i < args.warmup; ++i) {
            if (fn() != FF_OK) {
                sampler.finish();
                std::cerr << name << " warmup failed: " << ff_engine_last_error(engine) << "\n";
                return false;
            }
        }
        std::vector<double> times;
        times.reserve(args.repeats);
        for (int i = 0; i < args.repeats; ++i) {
            const auto tw = Clock::now();
            if (fn() != FF_OK) {
                sampler.finish();
                std::cerr << name << " failed: " << ff_engine_last_error(engine) << "\n";
                return false;
            }
            times.push_back(ms_since(tw));
        }
        sampler.finish();

        r.min_ms = *std::min_element(times.begin(), times.end());
        r.median_ms = percentile(times, 0.5);
        r.mean_ms = std::accumulate(times.begin(), times.end(), 0.0) / times.size();
        r.p95_ms = percentile(times, 0.95);
        r.vram_base = sampler.baseline;
        r.vram_peak = sampler.peak;
        r.vram_end = sampler.end;
        r.rss = sampler.end_rss;
        r.faces = faces_n;

        ff_stats st{};
        ff_last_stats(engine, &st);
        print_row(r, provider);
        write_csv(args.csv, args, r, provider, "1.31.1");
        rows.push_back(r);
        return true;
    };

    int out_faces = 0;
    std::vector<uint8_t> swap_out(static_cast<size_t>(target.rows) * target.step);
    std::vector<uint8_t> enh_out(static_cast<size_t>(crop.image.rows) * crop.image.step);
    std::vector<uint8_t> full_out(static_cast<size_t>(target.rows) * target.step);

    if (want_detect) {
        measure("detect", face_count, [&] {
            return ff_detect(engine, target.data, target.cols, target.rows,
                             static_cast<int32_t>(target.step), faces, 16, &out_faces);
        });
    }
    if (want_embed) {
        measure("embed", 1, [&] {
            return ff_embed(engine, source.data, source.cols, source.rows,
                            static_cast<int32_t>(source.step), &source_best, embedding);
        });
    }
    if (want_swap) {
        measure("swap", 1, [&] {
            return ff_swap_face(engine, target.data, target.cols, target.rows,
                                static_cast<int32_t>(target.step), embedding, &best, 1,
                                swap_out.data());
        });
    }
    if (want_enhance) {
        measure("enhance", 1, [&] {
            return ff_enhance(engine, crop.image.data, crop.image.cols, crop.image.rows,
                              static_cast<int32_t>(crop.image.step), &crop.face, 1,
                              enh_out.data(), nullptr, nullptr);
        });
    }
    if (want_full) {
        measure("full", face_count, [&] {
            return ff_process(engine, target.data, target.cols, target.rows,
                              static_cast<int32_t>(target.step), source.data, source.cols,
                              source.rows, static_cast<int32_t>(source.step),
                              full_out.data(), nullptr, nullptr);
        });
        cv::Mat result(target.rows, target.cols, CV_8UC3, full_out.data(), target.step);
        cv::imwrite("bench/results/last_full.png", result);
        cv::Mat swapped(target.rows, target.cols, CV_8UC3, swap_out.data(), target.step);
        cv::imwrite("bench/results/last_swap.png", swapped);
    }

    ff_engine_destroy(engine);
    return 0;
}
