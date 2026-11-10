#include "ff.hpp"

#include <cstring>

namespace ff {

namespace {
void setErr(std::string* err, const char* msg) {
    if (err) *err = msg ? msg : "";
}
} // namespace

// ---- Image -----------------------------------------------------------------

Image Image::load(const char* path, std::string* err) {
    uint8_t* data = nullptr;
    int32_t w = 0, h = 0, stride = 0;
    if (ff_image_load(path, &data, &w, &h, &stride) != FF_OK || !data) {
        setErr(err, "could not load image");
        return {};
    }
    Image im;
    im.data = data;
    im.width = w;
    im.height = h;
    im.stride = stride;
    return im;
}

Image Image::decode(const uint8_t* bytes, int32_t size, std::string* err) {
    uint8_t* data = nullptr;
    int32_t w = 0, h = 0, stride = 0;
    if (ff_image_decode(bytes, size, &data, &w, &h, &stride) != FF_OK || !data) {
        setErr(err, "could not decode image");
        return {};
    }
    Image im;
    im.data = data;
    im.width = w;
    im.height = h;
    im.stride = stride;
    return im;
}

Image Image::clone(std::string* err) const {
    uint8_t* out = nullptr;
    int32_t out_stride = 0;
    if (ff_image_clone(data, width, height, stride, &out, &out_stride) != FF_OK || !out) {
        setErr(err, "could not clone image");
        return {};
    }
    return Image::fromOwned(out, width, height);
}

Image Image::resized(int32_t nw, int32_t nh, std::string* err) const {
    uint8_t* out = nullptr;
    int32_t out_stride = 0;
    if (ff_image_resize(data, width, height, stride, nw, nh, &out, &out_stride) != FF_OK || !out) {
        setErr(err, "could not resize image");
        return {};
    }
    return Image::fromOwned(out, nw, nh);
}

bool Image::save(const char* path) const {
    return ff_image_save(path, data, width, height, stride) == FF_OK;
}

Image Image::resizeRegion(int32_t x, int32_t y, int32_t w, int32_t h, int32_t nw, int32_t nh,
                          std::string* err) const {
    if (!data) {
        setErr(err, "could not resize image region");
        return {};
    }
    const size_t off = static_cast<size_t>(y) * static_cast<size_t>(stride) +
                       static_cast<size_t>(x) * 3u;
    uint8_t* out = nullptr;
    int32_t out_stride = 0;
    if (ff_image_resize(data + off, w, h, stride, nw, nh, &out, &out_stride) != FF_OK || !out) {
        setErr(err, "could not resize image region");
        return {};
    }
    return Image::fromOwned(out, nw, nh);
}

bool Image::encodePng(std::vector<uint8_t>& out) const {
    uint8_t* out_data = nullptr;
    int32_t size = 0;
    if (ff_image_encode_png(data, width, height, stride, &out_data, &size) != FF_OK || !out_data) {
        return false;
    }
    out.assign(out_data, out_data + size);
    ff_image_free(out_data);
    return true;
}

// ---- Engine ----------------------------------------------------------------

bool Engine::create(const char* model_dir, int provider, bool seq, std::string* err) {
    ff_options opt;
    ff_options_default(&opt);
    opt.provider = provider;
    opt.sequential = seq ? 1 : 0;

    char msg[1024] = {0};
    ptr = ff_engine_create(model_dir, &opt, msg, static_cast<int32_t>(sizeof(msg)));
    if (!ptr) {
        setErr(err, msg);
        return false;
    }
    sequential = seq;
    return true;
}

std::string Engine::provider() const {
    return ptr ? ff_engine_provider(ptr) : "";
}

std::string Engine::lastError() const {
    return ptr ? ff_engine_last_error(ptr) : "";
}

void Engine::unload() {
    if (ptr) ff_engine_unload(ptr);
}

void Engine::setEnhancer(int enhancer) {
    if (ptr) ff_engine_set_enhancer(ptr, enhancer);
}

void Engine::setRestoreMouth(bool on) {
    if (ptr) ff_engine_set_restore_mouth(ptr, on ? 1 : 0);
}

int Engine::sessionsLoaded() const {
    return ptr ? ff_engine_sessions_loaded(ptr) : 0;
}

bool Engine::detect(const Image& img, ff_face* out, int max_faces, int* out_count) const {
    return ptr && ff_detect(ptr, img.data, img.width, img.height, img.stride, out,
                            max_faces, out_count) == FF_OK;
}

bool Engine::embed(const Image& img, const ff_face& face, float out[512]) const {
    return ptr && ff_embed(ptr, img.data, img.width, img.height, img.stride, &face, out) == FF_OK;
}

bool Engine::swapFace(const Image& img, const float embedding[512], const ff_face& face,
                      Image* out) const {
    return ptr && out &&
           ff_swap_face(ptr, img.data, img.width, img.height, img.stride, embedding, &face, 1,
                        out->data) == FF_OK;
}

Image Engine::swapPatch(const Image& img, const float embedding[512], const ff_face& face,
                        double matrix[6], bool* ok) const {
    uint8_t* patch = nullptr;
    const int rc = ptr ? ff_swap_patch(ptr, img.data, img.width, img.height, img.stride,
                                       embedding, &face, &patch, matrix)
                       : FF_ERR_INVALID;
    if (rc != FF_OK || !patch) {
        if (ok) *ok = false;
        return {};
    }
    if (ok) *ok = true;
    return Image::fromOwned(patch, 128, 128);
}

bool Engine::enhance(const Image& img, const ff_face* faces, int face_count, Image* out,
                     ff_progress_fn cb, void* user) const {
    return ptr && out &&
           ff_enhance(ptr, img.data, img.width, img.height, img.stride, faces, face_count,
                      out->data, cb, user) == FF_OK;
}

bool Engine::upscale(const Image& img, int scale, Image* out, ff_progress_fn cb,
                     void* user) const {
    if (!ptr || !out) return false;
    uint8_t* data = nullptr;
    int32_t ow = 0;
    int32_t oh = 0;
    const int rc = ff_upscale(ptr, img.data, img.width, img.height, img.stride, scale, &data,
                              &ow, &oh, cb, user);
    if (rc != FF_OK || !data) {
        if (data) ff_image_free(data);
        return false;
    }
    *out = Image::fromOwned(data, ow, oh);
    return true;
}

bool Engine::grainMatch(const Image& result, const Image& reference, const ff_face* faces,
                        int face_count, int strength, Image* out, ff_progress_fn cb,
                        void* user) const {
    return ptr && out && out->data &&
           ff_grain_match(ptr, result.data, result.width, result.height, result.stride,
                          out->data, reference.data, reference.width, reference.height,
                          reference.stride, faces, face_count, strength, cb, user) == FF_OK;
}

// ---- Video -----------------------------------------------------------------

bool Video::open(const char* path, std::string* err) {
    char msg[512] = {0};
    ptr = ff_video_open(path, msg, static_cast<int32_t>(sizeof(msg)));
    if (!ptr) {
        setErr(err, msg);
        return false;
    }
    return true;
}

Video::Info Video::info() const {
    Info o;
    if (ptr) ff_video_info(ptr, &o.width, &o.height, &o.fps, &o.frames, &o.duration);
    return o;
}

Image Video::frame(int64_t index, std::string* err) {
    uint8_t* data = nullptr;
    int32_t w = 0, h = 0, stride = 0;
    if (!ptr || ff_video_frame(ptr, index, &data, &w, &h, &stride) != FF_OK || !data) {
        setErr(err, "could not decode video frame");
        return {};
    }
    Image im;
    im.data = data;
    im.width = w;
    im.height = h;
    im.stride = stride;
    return im;
}

// ---- VideoWriter -----------------------------------------------------------

bool VideoWriter::open(const char* path, int32_t width, int32_t height, double fps,
                       const char* encoder, int32_t crf, const char* preset,
                       const char* ffmpeg, std::string* err) {
    char msg[512] = {0};
    ptr = ff_video_writer_open(path, width, height, fps, encoder ? encoder : "libx264",
                               crf, preset ? preset : "", ffmpeg ? ffmpeg : "ffmpeg",
                               msg, static_cast<int32_t>(sizeof(msg)));
    if (!ptr) {
        setErr(err, msg);
        return false;
    }
    return true;
}

bool VideoWriter::write(const Image& img) {
    return ptr && ff_video_writer_frame(ptr, img.data, img.width, img.height, img.stride) == FF_OK;
}

bool VideoWriter::close(std::string* err) {
    if (!ptr) return true;
    char msg[512] = {0};
    const int rc = ff_video_writer_close(ptr, msg, static_cast<int32_t>(sizeof(msg)));
    ptr = nullptr;
    if (rc != FF_OK) {
        setErr(err, msg);
        return false;
    }
    return true;
}

// ---- Free functions --------------------------------------------------------

bool concatVideos(const char* list_path, const char* out_path, const char* ffmpeg) {
    return ff_video_concat(list_path, out_path, ffmpeg, nullptr, 0) == FF_OK;
}

bool muxAudio(const char* video_path, const char* source_path, double start_sec,
              double duration, const char* out_path, const char* ffmpeg) {
    return ff_video_mux_audio(video_path, source_path, start_sec, duration, out_path, ffmpeg,
                              nullptr, 0) == FF_OK;
}

bool ffmpegProbe(const char* ffmpeg, std::string* out) {
    char buf[512] = {0};
    const bool ok = ff_ffmpeg_probe(ffmpeg, buf, static_cast<int32_t>(sizeof(buf))) == FF_OK;
    if (out) *out = buf;
    return ok;
}

bool mkdirs(const char* path) { return ff_fs_mkdirs(path) == FF_OK; }
bool fileExists(const char* path) { return ff_file_exists(path) != 0; }

bool readFile(const char* path, std::vector<uint8_t>& out) {
    uint8_t* data = nullptr;
    int32_t size = 0;
    if (ff_file_read(path, &data, &size) != FF_OK || !data) return false;
    out.assign(data, data + size);
    ff_image_free(data);
    return true;
}

bool writeFile(const char* path, const uint8_t* data, size_t size) {
    return ff_file_write(path, data, static_cast<int32_t>(size)) == FF_OK;
}

void removeFile(const char* path) { ff_file_remove(path); }
bool copyFile(const char* src, const char* dst) { return ff_file_copy(src, dst) == FF_OK; }

bool pastePatch(const Image& canvas, const Image& patch, const double matrix[6], Image* out) {
    return out && ff_paste_patch(canvas.data, canvas.width, canvas.height, canvas.stride,
                                 patch.data, matrix, out->data) == FF_OK;
}

bool blendMask(Image& dst, const Image& original, const uint8_t* mask, int32_t mask_w,
               int32_t mask_h) {
    return ff_blend_mask(dst.data, original.data, dst.width, dst.height, mask, mask_w,
                         mask_h) == FF_OK;
}

Memory memoryInfo() {
    Memory m;
    ff_memory_info(&m.vram_used_mb, &m.vram_total_mb, &m.rss_mb);
    return m;
}

int64_t nowNs() { return static_cast<int64_t>(ff_now_ns()); }

} // namespace ff
