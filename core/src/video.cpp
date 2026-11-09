// On-demand video decoding for the video workspace.
//
// Each handle owns a background worker that decodes a small ordered window of
// frames ahead of the caller (a few MiB of Mat data, capped by cache_limit).
// Sequential playback and analysis hit that window, so the UI thread only pays
// for the final packed BGR copy instead of a seek plus decode; preview and
// scrub requests still work, resetting the window when the caller jumps
// backwards or far forwards. Nothing is extracted to disk and the returned
// buffer is always owned by the caller.

#include "faceforge.h"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define ff_popen _popen
#define ff_pclose _pclose
#define FF_POPEN_WRITE "wb"
#else
// glibc popen rejects any flag other than 'e' after 'w' (EINVAL), so no "b".
#define ff_popen popen
#define ff_pclose pclose
#define FF_POPEN_WRITE "w"
#endif

struct ff_video {
    cv::VideoCapture cap;

    // Decode worker. The capture object is touched only by this thread after
    // open; everything shared with callers is guarded by mu.
    std::mutex mu;
    std::condition_variable cv;
    std::thread worker;
    bool quit = false;

    int64_t pos = -1;   // frame index that the next cap.read() returns (worker)
    int64_t next = -1;  // next frame the worker decodes sequentially
    int64_t want = -1;  // most recently requested frame
    bool failed = false;
    int64_t failed_index = -1;

    // Small ordered window of decoded frames. The Mats are kept as returned by
    // the decoder (no extra copy); ff_video_frame still copies once to the
    // caller-owned buffer.
    struct Frame {
        int64_t index = -1;
        cv::Mat bgr;
    };
    std::deque<Frame> cache;
    size_t cache_bytes = 0;
    size_t cache_limit = 96u << 20;  // 96 MiB per handle

    int32_t width = 0;
    int32_t height = 0;
    double fps = 0.0;
    int64_t frames = 0;
    double duration = 0.0;
};

namespace {

bool open_capture(cv::VideoCapture& cap, const char* path) {
    // OpenCV 5 can hand the FFmpeg decoder more worker threads; on older
    // versions the parameter does not exist and the backend default applies.
#if defined(CV_VERSION_MAJOR) && CV_VERSION_MAJOR >= 5
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    const unsigned want = hw > 1 ? hw - 1 : 1;
    const int threads = static_cast<int>(std::min<unsigned>(want, 8u));
    std::vector<int> params{cv::CAP_PROP_N_THREADS, threads};
    if (cap.open(cv::String(path), cv::CAP_ANY, params)) return true;
    cap.release();
#endif
    return cap.open(path, cv::CAP_ANY);
}

ff_video::Frame* find_frame(ff_video* v, int64_t index) {
    for (auto& f : v->cache) {
        if (f.index == index) return &f;
    }
    return nullptr;
}

int prefetch_ahead(const ff_video* v) {
    if (v->cache.empty()) return 4;
    const auto& m = v->cache.front().bgr;
    const size_t frame_bytes = m.total() * m.elemSize();
    if (frame_bytes == 0) return 4;
    const size_t fit = v->cache_limit / frame_bytes;
    return static_cast<int>(std::clamp<size_t>(fit, 2, 12));
}

size_t frame_bytes(const cv::Mat& m) { return m.total() * m.elemSize(); }

void evict_to_limit(ff_video* v) {
    const size_t cap = static_cast<size_t>(prefetch_ahead(v));
    while (!v->cache.empty() &&
           (v->cache_bytes > v->cache_limit || v->cache.size() > cap)) {
        v->cache_bytes -= frame_bytes(v->cache.front().bgr);
        v->cache.pop_front();
    }
}

// Worker-only: decode `index` into `out`. `pos` tracks what the next
// sequential read will return so seeks only happen when the request is not the
// frame we are already standing on.
bool decode_frame(ff_video* v, int64_t index, ff_video::Frame& out) {
    if (v->pos != index) {
        if (!v->cap.set(cv::CAP_PROP_POS_FRAMES, static_cast<double>(index)))
            return false;
        v->pos = index;
    }
    cv::Mat frame;
    if (!v->cap.read(frame) || frame.empty()) return false;
    v->pos = index + 1;

    cv::Mat bgr;
    if (frame.channels() == 3) {
        bgr = frame;
    } else if (frame.channels() == 4) {
        cv::cvtColor(frame, bgr, cv::COLOR_BGRA2BGR);
    } else {
        cv::cvtColor(frame, bgr, cv::COLOR_GRAY2BGR);
    }
    if (!bgr.isContinuous()) bgr = bgr.clone();

    out.index = index;
    out.bgr = std::move(bgr);
    return true;
}

void prefetch_main(ff_video* v) {
    std::unique_lock<std::mutex> lk(v->mu);
    while (!v->quit) {
        if (v->want < 0) {
            v->cv.wait(lk);
            continue;
        }
        const int ahead = prefetch_ahead(v);
        const int64_t limit = v->frames > 0 ? v->frames - 1 : INT64_MAX;
        const int64_t target = std::clamp<int64_t>(v->want, 0, limit);
        const int64_t window_end =
            std::min<int64_t>(limit, target + static_cast<int64_t>(ahead) - 1);

        // A request behind the decoder (and not already cached) means the
        // caller jumped backwards; a large forward jump is cheaper to seek than
        // to decode every frame in between.
        const bool behind = target < v->next && find_frame(v, target) == nullptr;
        const bool far_ahead =
            target > v->next + static_cast<int64_t>(ahead) * 2;
        if (behind || far_ahead) {
            v->next = target;
            v->pos = -1;
            v->cache.clear();
            v->cache_bytes = 0;
            v->failed = false;
            v->failed_index = -1;
            continue;
        }

        if (v->next > window_end || v->next > limit) {
            v->cv.wait(lk);
            continue;
        }
        if (find_frame(v, v->next)) {
            ++v->next;
            continue;
        }

        const int64_t index = v->next;
        lk.unlock();
        ff_video::Frame out;
        const bool ok = decode_frame(v, index, out);
        lk.lock();
        if (v->quit) break;
        if (!ok) {
            v->failed = true;
            v->failed_index = index;
            // Skip the bad frame instead of spinning on it. A new request for
            // the same index seeks back and retries.
            v->next = index + 1;
            v->pos = -1;
            v->cv.notify_all();
            v->cv.wait(lk);
            continue;
        }
        v->cache_bytes += frame_bytes(out.bgr);
        v->cache.push_back(std::move(out));
        v->next = index + 1;
        evict_to_limit(v);
        v->cv.notify_all();
    }
}

void set_error(char* err, int32_t err_len, const char* msg) {
    if (!err || err_len <= 0) return;
    std::strncpy(err, msg, static_cast<size_t>(err_len) - 1);
    err[err_len - 1] = '\0';
}

// Quote a single argument for the shell used by popen/system. On POSIX single
// quotes protect everything except themselves; on Windows double quotes are
// used and embedded quotes are escaped.
std::string shq(const std::string& s) {
#ifdef _WIN32
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += "\\\"";
        else out += c;
    }
    out += "\"";
    return out;
#else
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
#endif
}

std::string num(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.6g", v);
    return std::string(buf);
}

int run_command(const std::string& cmd) {
    return std::system(cmd.c_str());
}

}  // namespace

struct ff_video_writer {
    FILE* pipe = nullptr;
    int32_t width = 0;
    int32_t height = 0;
};

extern "C" {

ff_video* ff_video_open(const char* path, char* err, int32_t err_len) {
    if (!path || !*path) {
        set_error(err, err_len, "empty video path");
        return nullptr;
    }
    auto v = std::make_unique<ff_video>();
    if (!open_capture(v->cap, path)) {
        set_error(err, err_len, "cannot open video");
        return nullptr;
    }
    v->width = static_cast<int32_t>(v->cap.get(cv::CAP_PROP_FRAME_WIDTH) + 0.5);
    v->height = static_cast<int32_t>(v->cap.get(cv::CAP_PROP_FRAME_HEIGHT) + 0.5);
    v->fps = v->cap.get(cv::CAP_PROP_FPS);
    if (!(v->fps > 0.0) || v->fps > 1000.0) v->fps = 25.0;
    const double count = v->cap.get(cv::CAP_PROP_FRAME_COUNT);
    v->frames = count > 0.0 ? static_cast<int64_t>(count + 0.5) : 0;
    v->duration = v->frames > 0 ? static_cast<double>(v->frames) / v->fps : 0.0;
    if (v->width <= 0 || v->height <= 0) {
        // Some containers expose no metadata until the first decode.
        cv::Mat probe;
        if (!v->cap.read(probe) || probe.empty()) {
            set_error(err, err_len, "video has no decodable frames");
            return nullptr;
        }
        v->width = probe.cols;
        v->height = probe.rows;
        v->pos = 1;
    }
    v->next = 0;
    v->want = -1;
    v->worker = std::thread(prefetch_main, v.get());
    return v.release();
}

void ff_video_close(ff_video* video) {
    if (!video) return;
    {
        std::lock_guard<std::mutex> lock(video->mu);
        video->quit = true;
        video->cv.notify_all();
    }
    if (video->worker.joinable()) video->worker.join();
    delete video;
}

int32_t ff_video_info(const ff_video* video, int32_t* out_width,
                      int32_t* out_height, double* out_fps, int64_t* out_frames,
                      double* out_duration) {
    if (!video) return FF_ERR_INVALID;
    if (out_width) *out_width = video->width;
    if (out_height) *out_height = video->height;
    if (out_fps) *out_fps = video->fps;
    if (out_frames) *out_frames = video->frames;
    if (out_duration) *out_duration = video->duration;
    return FF_OK;
}

int32_t ff_video_frame(ff_video* video, int64_t index, uint8_t** out_bgr,
                       int32_t* out_width, int32_t* out_height,
                       int32_t* out_stride) {
    if (!video || !out_bgr || index < 0) return FF_ERR_INVALID;
    *out_bgr = nullptr;

    std::unique_lock<std::mutex> lock(video->mu);
    if (video->quit) return FF_ERR_IO;
    if (video->frames > 0 && index >= video->frames) return FF_ERR_IO;

    ff_video::Frame* hit = find_frame(video, index);
    if (!hit) {
        video->want = index;
        video->failed = false;
        video->failed_index = -1;
        video->cv.notify_all();
        video->cv.wait(lock, [&] {
            return video->quit || find_frame(video, index) != nullptr ||
                   (video->failed && video->failed_index == index);
        });
        if (video->quit) return FF_ERR_IO;
        hit = find_frame(video, index);
    }
    // Follow the served frame so prefetch keeps looking ahead of the caller.
    video->want = index;
    video->cv.notify_all();
    if (!hit) return FF_ERR_IO;

    // Take a refcounted shallow copy and release the lock before the memcpy so
    // the decode worker can keep inserting prefetched frames meanwhile.
    cv::Mat hold = hit->bgr;
    const int32_t w = hold.cols;
    const int32_t h = hold.rows;
    const int32_t stride = w * 3;
    if (video->width <= 0 || video->height <= 0) {
        video->width = w;
        video->height = h;
        video->frames = 0;
        video->duration = 0.0;
    }
    lock.unlock();

    const size_t bytes = static_cast<size_t>(stride) * static_cast<size_t>(h);
    auto* data = static_cast<uint8_t*>(std::malloc(bytes));
    if (!data) return FF_ERR_NOMEM;
    std::memcpy(data, hold.data, bytes);

    *out_bgr = data;
    if (out_width) *out_width = w;
    if (out_height) *out_height = h;
    if (out_stride) *out_stride = stride;
    return FF_OK;
}

ff_video_writer* ff_video_writer_open(const char* path, int32_t width,
                                      int32_t height, double fps,
                                      const char* encoder, int32_t crf,
                                      const char* preset, const char* ffmpeg,
                                      char* err, int32_t err_len) {
    if (!path || !*path || width <= 0 || height <= 0) {
        set_error(err, err_len, "invalid writer arguments");
        return nullptr;
    }
    const std::string exe = (ffmpeg && *ffmpeg) ? ffmpeg : "ffmpeg";
    const std::string enc = (encoder && *encoder) ? encoder : "libx264";
    const int quality = crf > 0 ? crf : 18;

    std::string cmd = shq(exe) +
                      " -hide_banner -loglevel error -y -f rawvideo"
                      " -pix_fmt bgr24 -s " + std::to_string(width) + "x" +
                      std::to_string(height) + " -r " + num(fps > 0 ? fps : 25.0) +
                      " -i - -an -c:v " + shq(enc) + " -crf " +
                      std::to_string(quality) + " -pix_fmt yuv420p";
    if (preset && *preset) cmd += " -preset " + shq(preset);
    cmd += " -movflags +faststart " + shq(path) + " 2>&1";

    FILE* pipe = ff_popen(cmd.c_str(), FF_POPEN_WRITE);
    if (!pipe) {
        set_error(err, err_len, "cannot start ffmpeg");
        return nullptr;
    }
    auto* w = new ff_video_writer();
    w->pipe = pipe;
    w->width = width;
    w->height = height;
    return w;
}

int32_t ff_video_writer_frame(ff_video_writer* writer, const uint8_t* bgr,
                              int32_t width, int32_t height, int32_t stride) {
    if (!writer || !writer->pipe || !bgr) return FF_ERR_INVALID;
    if (width != writer->width || height != writer->height) return FF_ERR_INVALID;
    const size_t row = static_cast<size_t>(width) * 3;
    for (int32_t y = 0; y < height; ++y) {
        const uint8_t* src = bgr + static_cast<size_t>(y) * static_cast<size_t>(stride);
        if (std::fwrite(src, 1, row, writer->pipe) != row) return FF_ERR_IO;
    }
    return FF_OK;
}

int32_t ff_video_writer_close(ff_video_writer* writer, char* err,
                              int32_t err_len) {
    if (!writer) return FF_ERR_INVALID;
    int32_t rc = FF_OK;
    if (writer->pipe) {
        if (std::fflush(writer->pipe) != 0) rc = FF_ERR_IO;
        const int status = ff_pclose(writer->pipe);
        if (status != 0) rc = FF_ERR_IO;
        writer->pipe = nullptr;
    }
    delete writer;
    if (rc != FF_OK) set_error(err, err_len, "ffmpeg failed while encoding");
    return rc;
}

int32_t ff_video_concat(const char* list_path, const char* out_path,
                        const char* ffmpeg, char* err, int32_t err_len) {
    if (!list_path || !out_path) return FF_ERR_INVALID;
    const std::string exe = (ffmpeg && *ffmpeg) ? ffmpeg : "ffmpeg";
    const std::string cmd = shq(exe) +
                            " -hide_banner -loglevel error -y -f concat -safe 0 -i " +
                            shq(list_path) + " -c copy -movflags +faststart " +
                            shq(out_path) + " 2>&1";
    const int status = run_command(cmd);
    if (status != 0) {
        set_error(err, err_len, "ffmpeg concat failed");
        return FF_ERR_IO;
    }
    return FF_OK;
}

int32_t ff_video_mux_audio(const char* video_path, const char* source_path,
                           double start_sec, double duration,
                           const char* out_path, const char* ffmpeg,
                           char* err, int32_t err_len) {
    if (!video_path || !source_path || !out_path) return FF_ERR_INVALID;
    const std::string exe = (ffmpeg && *ffmpeg) ? ffmpeg : "ffmpeg";
    const std::string cmd = shq(exe) +
                            " -hide_banner -loglevel error -y -i " + shq(video_path) +
                            " -ss " + num(start_sec) + " -t " + num(duration) +
                            " -i " + shq(source_path) +
                            " -map 0:v:0 -map 1:a? -c:v copy -c:a aac -b:a 192k"
                            " -shortest -movflags +faststart " +
                            shq(out_path) + " 2>&1";
    const int status = run_command(cmd);
    if (status != 0) {
        set_error(err, err_len, "ffmpeg audio mux failed");
        return FF_ERR_IO;
    }
    return FF_OK;
}

int32_t ff_ffmpeg_probe(const char* ffmpeg, char* out, int32_t out_len) {
    const std::string exe = (ffmpeg && *ffmpeg) ? ffmpeg : "ffmpeg";
    const std::string cmd = shq(exe) + " -version 2>&1";
    FILE* p = ff_popen(cmd.c_str(), "r");
    if (!p) return FF_ERR_IO;
    char buf[512] = {0};
    const char* line = std::fgets(buf, sizeof(buf), p);
    const int status = ff_pclose(p);
    if (line && out && out_len > 0) {
        std::strncpy(out, line, static_cast<size_t>(out_len) - 1);
        out[out_len - 1] = '\0';
    }
    return status == 0 ? FF_OK : FF_ERR_IO;
}

int32_t ff_fs_mkdirs(const char* path) {
    if (!path || !*path) return FF_ERR_INVALID;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path), ec);
    return ec ? FF_ERR_IO : FF_OK;
}

int32_t ff_file_read(const char* path, uint8_t** out_data, int32_t* out_size) {
    if (!path || !out_data) return FF_ERR_INVALID;
    *out_data = nullptr;
    if (out_size) *out_size = 0;
    FILE* f = std::fopen(path, "rb");
    if (!f) return FF_ERR_IO;
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    if (size < 0) {
        std::fclose(f);
        return FF_ERR_IO;
    }
    std::fseek(f, 0, SEEK_SET);
    auto* data = static_cast<uint8_t*>(std::malloc(static_cast<size_t>(size)));
    if (!data && size > 0) {
        std::fclose(f);
        return FF_ERR_NOMEM;
    }
    const size_t got = size > 0
                           ? std::fread(data, 1, static_cast<size_t>(size), f)
                           : 0;
    std::fclose(f);
    if (got != static_cast<size_t>(size)) {
        std::free(data);
        return FF_ERR_IO;
    }
    *out_data = data;
    if (out_size) *out_size = static_cast<int32_t>(size);
    return FF_OK;
}

int32_t ff_file_write(const char* path, const uint8_t* data, int32_t size) {
    if (!path || (!data && size > 0)) return FF_ERR_INVALID;
    FILE* f = std::fopen(path, "wb");
    if (!f) return FF_ERR_IO;
    const size_t wrote = size > 0
                             ? std::fwrite(data, 1, static_cast<size_t>(size), f)
                             : 0;
    const bool ok = wrote == static_cast<size_t>(size);
    if (std::fclose(f) != 0 || !ok) return FF_ERR_IO;
    return FF_OK;
}

int32_t ff_file_exists(const char* path) {
    if (!path) return 0;
    std::error_code ec;
    return std::filesystem::is_regular_file(std::filesystem::path(path), ec) ? 1 : 0;
}

int32_t ff_file_remove(const char* path) {
    if (!path) return FF_ERR_INVALID;
    std::error_code ec;
    std::filesystem::remove(std::filesystem::path(path), ec);
    return ec ? FF_ERR_IO : FF_OK;
}

int32_t ff_file_copy(const char* src, const char* dst) {
    if (!src || !dst) return FF_ERR_INVALID;
    std::error_code ec;
    std::filesystem::copy_file(std::filesystem::path(src),
                               std::filesystem::path(dst),
                               std::filesystem::copy_options::overwrite_existing,
                               ec);
    return ec ? FF_ERR_IO : FF_OK;
}

}  // extern "C"
