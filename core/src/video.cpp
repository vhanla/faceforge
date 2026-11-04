// On-demand video decoding for the video workspace.
//
// Frames are decoded straight from the container and copied into tightly
// packed BGR buffers owned by the caller. Nothing is extracted to disk. A
// handle remembers the next expected frame so playback and analysis read
// sequentially instead of seeking for every frame.

#include "faceforge.h"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
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
    std::mutex mu;
    int64_t next = -1;
    int32_t width = 0;
    int32_t height = 0;
    double fps = 0.0;
    int64_t frames = 0;
    double duration = 0.0;
};

namespace {

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
    if (!v->cap.open(path, cv::CAP_ANY)) {
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
        v->next = 1;
    }
    return v.release();
}

void ff_video_close(ff_video* video) { delete video; }

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

    std::lock_guard<std::mutex> lock(video->mu);
    if (video->next != index) {
        if (!video->cap.set(cv::CAP_PROP_POS_FRAMES, static_cast<double>(index))) {
            return FF_ERR_IO;
        }
        video->next = index;
    }

    cv::Mat frame;
    if (!video->cap.read(frame) || frame.empty()) return FF_ERR_IO;
    video->next = index + 1;

    cv::Mat bgr;
    if (frame.channels() == 3) {
        bgr = frame;
    } else if (frame.channels() == 4) {
        cv::cvtColor(frame, bgr, cv::COLOR_BGRA2BGR);
    } else {
        cv::cvtColor(frame, bgr, cv::COLOR_GRAY2BGR);
    }
    if (!bgr.isContinuous()) bgr = bgr.clone();

    const int32_t w = bgr.cols;
    const int32_t h = bgr.rows;
    const int32_t stride = w * 3;
    const size_t bytes = static_cast<size_t>(stride) * static_cast<size_t>(h);
    auto* data = static_cast<uint8_t*>(std::malloc(bytes));
    if (!data) return FF_ERR_NOMEM;
    std::memcpy(data, bgr.data, bytes);

    if (video->width <= 0 || video->height <= 0) {
        video->width = w;
        video->height = h;
        video->frames = 0;
        video->duration = 0;
    }

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
