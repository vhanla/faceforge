#pragma once
// RAII C++ wrappers over the faceforge C ABI (core/include/faceforge.h).
// Direct port of gui/src/ff.zig: thin, exception-free helpers that keep the
// malloc'd image buffers and opaque handles owned by value so the app logic
// never leaks. Errors are reported through a boolean return plus an optional
// std::string describing the failure.

#include "faceforge.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ff {

// 8-bit BGR image. Owns its buffer when it came from load/decode/clone/patch.
struct Image {
    uint8_t* data = nullptr;
    int32_t width = 0;
    int32_t height = 0;
    int32_t stride = 0;
    // False for views onto a buffer someone else owns (e.g. the pristine base
    // passed to the worker); reset() must not free those.
    bool owned = true;

    Image() = default;
    ~Image() { reset(); }
    Image(Image&& o) noexcept
        : data(o.data), width(o.width), height(o.height), stride(o.stride), owned(o.owned) {
        o.data = nullptr;
        o.width = o.height = o.stride = 0;
    }
    Image& operator=(Image&& o) noexcept {
        if (this != &o) {
            reset();
            data = o.data;
            width = o.width;
            height = o.height;
            stride = o.stride;
            owned = o.owned;
            o.data = nullptr;
            o.width = o.height = o.stride = 0;
        }
        return *this;
    }
    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;

    bool valid() const { return data != nullptr; }
    size_t byteLen() const {
        return static_cast<size_t>(stride) * static_cast<size_t>(height);
    }
    void reset() {
        if (data && owned) ff_image_free(data);
        data = nullptr;
        width = height = stride = 0;
        owned = true;
    }

    // Take ownership of a tightly packed malloc'd BGR buffer.
    static Image fromOwned(uint8_t* d, int32_t w, int32_t h) {
        Image im;
        im.data = d;
        im.width = w;
        im.height = h;
        im.stride = w * 3;
        im.owned = true;
        return im;
    }

    // Non-owning view over an existing buffer.
    static Image borrow(uint8_t* d, int32_t w, int32_t h, int32_t s) {
        Image im;
        im.data = d;
        im.width = w;
        im.height = h;
        im.stride = s;
        im.owned = false;
        return im;
    }

    static Image load(const char* path, std::string* err = nullptr);
    static Image decode(const uint8_t* bytes, int32_t size, std::string* err = nullptr);

    Image clone(std::string* err = nullptr) const;
    Image resized(int32_t nw, int32_t nh, std::string* err = nullptr) const;
    // Resample a sub-rectangle without an intermediate copy (used for face
    // thumbnails in the video workspace).
    Image resizeRegion(int32_t x, int32_t y, int32_t w, int32_t h, int32_t nw, int32_t nh,
                       std::string* err = nullptr) const;
    bool save(const char* path) const;
    bool encodePng(std::vector<uint8_t>& out) const;
};

struct Engine {
    ff_engine* ptr = nullptr;
    bool sequential = false;

    Engine() = default;
    ~Engine() { destroy(); }
    Engine(Engine&& o) noexcept : ptr(o.ptr), sequential(o.sequential) { o.ptr = nullptr; }
    Engine& operator=(Engine&& o) noexcept {
        if (this != &o) {
            destroy();
            ptr = o.ptr;
            sequential = o.sequential;
            o.ptr = nullptr;
        }
        return *this;
    }
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    void destroy() {
        if (ptr) ff_engine_destroy(ptr);
        ptr = nullptr;
    }
    bool create(const char* model_dir, int provider, bool seq, std::string* err);

    std::string provider() const;
    std::string lastError() const;
    void unload();
    void setEnhancer(int enhancer);
    void setRestoreMouth(bool on);
    int sessionsLoaded() const;

    bool detect(const Image& img, ff_face* out, int max_faces, int* out_count) const;
    bool embed(const Image& img, const ff_face& face, float out[512]) const;
    bool swapFace(const Image& img, const float embedding[512], const ff_face& face,
                  Image* out) const;
    // Runs the swapper for one face; returns the raw 128x128 patch plus the
    // affine matrix that places it in the source image.
    Image swapPatch(const Image& img, const float embedding[512], const ff_face& face,
                    double matrix[6], bool* ok) const;
    bool enhance(const Image& img, const ff_face* faces, int face_count, Image* out,
                 ff_progress_fn cb = nullptr, void* user = nullptr) const;
};

// On-demand video decoder. Not thread-safe; one handle per thread.
struct Video {
    ff_video* ptr = nullptr;
    struct Info {
        int32_t width = 0;
        int32_t height = 0;
        double fps = 25.0;
        int64_t frames = 0;
        double duration = 0.0;
    };

    Video() = default;
    ~Video() { close(); }
    Video(Video&& o) noexcept : ptr(o.ptr) { o.ptr = nullptr; }
    Video& operator=(Video&& o) noexcept {
        if (this != &o) {
            close();
            ptr = o.ptr;
            o.ptr = nullptr;
        }
        return *this;
    }
    Video(const Video&) = delete;
    Video& operator=(const Video&) = delete;

    void close() {
        if (ptr) ff_video_close(ptr);
        ptr = nullptr;
    }
    bool open(const char* path, std::string* err = nullptr);
    Info info() const;
    Image frame(int64_t index, std::string* err = nullptr);
};

// Streams raw BGR frames into ffmpeg (no intermediate image sequence).
struct VideoWriter {
    ff_video_writer* ptr = nullptr;

    VideoWriter() = default;
    ~VideoWriter() { close(nullptr); }
    VideoWriter(VideoWriter&& o) noexcept : ptr(o.ptr) { o.ptr = nullptr; }
    VideoWriter& operator=(VideoWriter&& o) noexcept {
        if (this != &o) {
            close(nullptr);
            ptr = o.ptr;
            o.ptr = nullptr;
        }
        return *this;
    }
    VideoWriter(const VideoWriter&) = delete;
    VideoWriter& operator=(const VideoWriter&) = delete;

    bool open(const char* path, int32_t width, int32_t height, double fps,
              const char* encoder, int32_t crf, const char* preset, const char* ffmpeg,
              std::string* err = nullptr);
    bool write(const Image& img);
    // Closes the pipe; returns false when ffmpeg exited unsuccessfully.
    bool close(std::string* err);
};

bool concatVideos(const char* list_path, const char* out_path, const char* ffmpeg);
bool muxAudio(const char* video_path, const char* source_path, double start_sec,
              double duration, const char* out_path, const char* ffmpeg);
bool ffmpegProbe(const char* ffmpeg, std::string* out = nullptr);

bool mkdirs(const char* path);
bool fileExists(const char* path);
bool readFile(const char* path, std::vector<uint8_t>& out);
bool writeFile(const char* path, const uint8_t* data, size_t size);
void removeFile(const char* path);
bool copyFile(const char* src, const char* dst);

// Blend a 128x128 patch (from Engine::swapPatch) into `canvas`.
bool pastePatch(const Image& canvas, const Image& patch, const double matrix[6], Image* out);
// Blend `original` over `dst` in place using a retouch mask (mask is sampled
// bilinearly when its resolution differs from the images).
bool blendMask(Image& dst, const Image& original, const uint8_t* mask, int32_t mask_w,
               int32_t mask_h);

struct Memory {
    float vram_used_mb = 0.0f;
    float vram_total_mb = 0.0f;
    float rss_mb = 0.0f;
};
Memory memoryInfo();

// Portable monotonic clock in nanoseconds.
int64_t nowNs();

} // namespace ff
