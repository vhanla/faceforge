#include "internal.hpp"
#include <opencv2/imgcodecs.hpp>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#define FF_X86 1
#include <immintrin.h>
#else
#define FF_X86 0
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace ff {

namespace {

// --- Dynamic library loading (NVML is never a build dependency) ----------
#ifdef _WIN32
using LibHandle = HMODULE;
inline LibHandle lib_open(const char* name) { return ::LoadLibraryA(name); }
inline void* lib_symbol(LibHandle h, const char* name) {
    return reinterpret_cast<void*>(::GetProcAddress(h, name));
}
#else
using LibHandle = void*;
inline LibHandle lib_open(const char* name) { return ::dlopen(name, RTLD_NOW | RTLD_LOCAL); }
inline void* lib_symbol(LibHandle h, const char* name) { return ::dlsym(h, name); }
#endif

// --- NVML without a build-time dependency -------------------------------
typedef struct nvmlMemory_st {
    unsigned long long total;
    unsigned long long free;
    unsigned long long used;
} nvmlMemory_t;

using nvmlInit_t = int (*)();
using nvmlShutdown_t = int (*)();
using nvmlGetHandle_t = int (*)(unsigned int, void**);
using nvmlGetMemory_t = int (*)(void*, nvmlMemory_t*);

struct Nvml {
    LibHandle handle = nullptr;
    nvmlInit_t init = nullptr;
    nvmlShutdown_t shutdown = nullptr;
    nvmlGetHandle_t get_handle = nullptr;
    nvmlGetMemory_t get_memory = nullptr;
    void* device = nullptr;
    bool ok = false;
};

Nvml& nvml() {
    static Nvml n;
    static std::once_flag once;
    std::call_once(once, [] {
#ifdef _WIN32
        n.handle = lib_open("nvml.dll");
        if (!n.handle) {
            n.handle = lib_open("C:\\Program Files\\NVIDIA Corporation\\NVSMI\\nvml.dll");
        }
#else
        n.handle = lib_open("libnvidia-ml.so.1");
#endif
        if (!n.handle) return;
        n.init = reinterpret_cast<nvmlInit_t>(lib_symbol(n.handle, "nvmlInit_v2"));
        n.shutdown = reinterpret_cast<nvmlShutdown_t>(lib_symbol(n.handle, "nvmlShutdown"));
        n.get_handle = reinterpret_cast<nvmlGetHandle_t>(
            lib_symbol(n.handle, "nvmlDeviceGetHandleByIndex_v2"));
        n.get_memory = reinterpret_cast<nvmlGetMemory_t>(
            lib_symbol(n.handle, "nvmlDeviceGetMemoryInfo"));
        if (!n.init || !n.get_handle || !n.get_memory) return;
        if (n.init() != 0) return;
        if (n.get_handle(0, &n.device) != 0) return;
        n.ok = true;
    });
    return n;
}

ff_face pick_best_face(const std::vector<ff_face>& faces) {
    ff_face best = faces.front();
    float best_area = -1.0f;
    for (const auto& f : faces) {
        const float area = (f.bbox[2] - f.bbox[0]) * (f.bbox[3] - f.bbox[1]);
        if (area > best_area) {
            best_area = area;
            best = f;
        }
    }
    return best;
}

}  // namespace

}  // namespace ff

using namespace ff;

namespace {

// ---------------------------------------------------------------------------
// CPU-dispatched hot loops
// ---------------------------------------------------------------------------
// The packaged build stays generic and probes the CPU once at runtime. The
// vector paths are limited to functions where the scalar version is a proven
// bottleneck (see docs/optimization.md) and produce bit-identical bytes.

bool have_avx2_fma() {
#if FF_X86 && (defined(__GNUC__) || defined(__clang__))
    static const bool ok = __builtin_cpu_supports("avx2") &&
                           __builtin_cpu_supports("fma");
    return ok;
#else
    return false;
#endif
}

void blend_mask_same_size_scalar(uint8_t* dst, const uint8_t* src,
                                 const uint8_t* mask, int width, int height) {
    for (int y = 0; y < height; ++y) {
        const size_t row = static_cast<size_t>(y) * width;
        uint8_t* drow = dst + row * 3;
        const uint8_t* srow = src + row * 3;
        const uint8_t* mrow = mask + row;
        for (int x = 0; x < width; ++x) {
            const float a = static_cast<float>(mrow[x]) * (1.0f / 255.0f);
            if (a <= 0.0001f) continue;
            const float ia = 1.0f - a;
            uint8_t* d = drow + static_cast<size_t>(x) * 3;
            const uint8_t* s = srow + static_cast<size_t>(x) * 3;
            for (int c = 0; c < 3; ++c) {
                d[c] = static_cast<uint8_t>(
                    static_cast<float>(d[c]) * ia + static_cast<float>(s[c]) * a + 0.5f);
            }
        }
    }
}

#if FF_X86 && (defined(__GNUC__) || defined(__clang__))
__attribute__((target("avx2,fma"), always_inline)) inline __m256
load8_u8_as_f32(const uint8_t* p) {
    return _mm256_cvtepi32_ps(
        _mm256_cvtepu8_epi32(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(p))));
}

__attribute__((target("avx2,fma"), always_inline)) inline void
store8_f32_as_u8(uint8_t* p, __m256 v) {
    const __m256i iv = _mm256_cvttps_epi32(_mm256_add_ps(v, _mm256_set1_ps(0.5f)));
    const __m128i lo = _mm256_castsi256_si128(iv);
    const __m128i hi = _mm256_extracti128_si256(iv, 1);
    const __m128i p16 = _mm_packus_epi32(lo, hi);
    const __m128i p8 = _mm_packus_epi16(p16, p16);
    _mm_storel_epi64(reinterpret_cast<__m128i*>(p), p8);
}

// Same-size 8-bit mask blend over interleaved BGR. Eight pixels per iteration:
// the mask bytes become eight alpha floats, then three permutes expand them to
// the 24 interleaved channels (RGB of each pixel repeats its alpha).
__attribute__((target("avx2,fma")))
void blend_mask_same_size_avx2(uint8_t* dst, const uint8_t* src,
                               const uint8_t* mask, int width, int height) {
    const __m256 inv255 = _mm256_set1_ps(1.0f / 255.0f);
    const __m256 one = _mm256_set1_ps(1.0f);
    const __m256i idx0 = _mm256_setr_epi32(0, 0, 0, 1, 1, 1, 2, 2);
    const __m256i idx1 = _mm256_setr_epi32(2, 3, 3, 3, 4, 4, 4, 5);
    const __m256i idx2 = _mm256_setr_epi32(5, 5, 6, 6, 6, 7, 7, 7);

    for (int y = 0; y < height; ++y) {
        const size_t row = static_cast<size_t>(y) * width;
        uint8_t* drow = dst + row * 3;
        const uint8_t* srow = src + row * 3;
        const uint8_t* mrow = mask + row;
        int x = 0;
        for (; x + 8 <= width; x += 8) {
            const __m128i mb =
                _mm_loadl_epi64(reinterpret_cast<const __m128i*>(mrow + x));
            if (_mm_testz_si128(mb, mb)) continue;  // untouched area: keep dst
            const __m256 af = _mm256_mul_ps(
                _mm256_cvtepi32_ps(_mm256_cvtepu8_epi32(mb)), inv255);
            uint8_t* d = drow + static_cast<size_t>(x) * 3;
            const uint8_t* s = srow + static_cast<size_t>(x) * 3;

            const __m256 a0 = _mm256_permutevar8x32_ps(af, idx0);
            const __m256 a1 = _mm256_permutevar8x32_ps(af, idx1);
            const __m256 a2 = _mm256_permutevar8x32_ps(af, idx2);

            const __m256 o0 = _mm256_fmadd_ps(load8_u8_as_f32(s + 0), a0,
                                              _mm256_mul_ps(load8_u8_as_f32(d + 0),
                                                           _mm256_sub_ps(one, a0)));
            const __m256 o1 = _mm256_fmadd_ps(load8_u8_as_f32(s + 8), a1,
                                              _mm256_mul_ps(load8_u8_as_f32(d + 8),
                                                           _mm256_sub_ps(one, a1)));
            const __m256 o2 = _mm256_fmadd_ps(load8_u8_as_f32(s + 16), a2,
                                              _mm256_mul_ps(load8_u8_as_f32(d + 16),
                                                           _mm256_sub_ps(one, a2)));
            store8_f32_as_u8(d + 0, o0);
            store8_f32_as_u8(d + 8, o1);
            store8_f32_as_u8(d + 16, o2);
        }
        for (; x < width; ++x) {
            const float a = static_cast<float>(mrow[x]) * (1.0f / 255.0f);
            if (a <= 0.0001f) continue;
            const float ia = 1.0f - a;
            uint8_t* d = drow + static_cast<size_t>(x) * 3;
            const uint8_t* s = srow + static_cast<size_t>(x) * 3;
            for (int c = 0; c < 3; ++c) {
                d[c] = static_cast<uint8_t>(
                    static_cast<float>(d[c]) * ia + static_cast<float>(s[c]) * a + 0.5f);
            }
        }
    }
}
#endif  // FF_X86 && GCC/Clang

}  // namespace

extern "C" {

void ff_options_default(ff_options* opt) {
    if (!opt) return;
    std::memset(opt, 0, sizeof(*opt));
    opt->provider = FF_PROVIDER_AUTO;
    opt->device_id = 0;
    opt->intra_threads = 0;
    opt->sequential = 0;
    opt->det_size = 640;
    opt->det_thresh = 0.5f;
    opt->nms_thresh = 0.4f;
    opt->max_faces = 10;
    opt->enhance = FF_ENHANCE_NONE;
    opt->esrgan_tile = 256;
    opt->gpu_mem_limit = 0;
    opt->restore_mouth = 0;
    opt->pad = 0;
}

ff_engine* ff_engine_create(const char* model_dir, const ff_options* opt,
                            char* err, int32_t err_len) {
    auto fail = [&](const std::string& msg) -> ff_engine* {
        if (err && err_len > 0) {
            std::snprintf(err, static_cast<size_t>(err_len), "%s", msg.c_str());
        }
        return nullptr;
    };

    if (!model_dir) return fail("model_dir is null");
    const std::string dir = model_dir;
    if (!std::filesystem::is_directory(dir)) return fail("not a directory: " + dir);

    const char* required[] = {"det_10g.onnx", "w600k_r50.onnx", "inswapper_128.onnx",
                              "emap.bin"};
    for (const char* name : required) {
        if (!std::filesystem::exists(dir + "/" + name)) {
            return fail(std::string("missing model: ") + dir + "/" + name +
                        " (run scripts/fetch_models.sh)");
        }
    }

    auto engine = std::make_unique<ff_engine>();
    engine->model_dir = dir;
    if (opt) engine->opt = *opt;
    else ff_options_default(&engine->opt);

    if (engine->opt.provider != FF_PROVIDER_AUTO &&
        engine->opt.provider != FF_PROVIDER_CUDA &&
        engine->opt.provider != FF_PROVIDER_CPU) {
        return fail("invalid provider");
    }

    try {
        engine->env = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "faceforge");
    } catch (const std::exception& ex) {
        return fail(std::string("cannot create ONNX Runtime environment: ") + ex.what());
    }

    bool has_cuda = false;
    try {
        for (const auto& p : Ort::GetAvailableProviders()) {
            if (p == "CUDAExecutionProvider") has_cuda = true;
        }
    } catch (...) {
    }

    if (engine->opt.provider == FF_PROVIDER_CPU) {
        engine->using_cuda = false;
        engine->provider_name = "cpu";
    } else if (engine->opt.provider == FF_PROVIDER_CUDA) {
        if (!has_cuda) {
            return fail("CUDAExecutionProvider is not available in this ONNX Runtime "
                        "build (GPU package required)");
        }
        engine->using_cuda = true;
        engine->provider_name = "cuda";
    } else {
        engine->using_cuda = has_cuda;
        engine->provider_name = has_cuda ? "cuda" : "cpu";
    }

    // Validate the provider by actually opening the smallest model. If CUDA is
    // broken in AUTO mode this transparently falls back to CPU.
    engine->clear_error();
    if (!engine->session("det", "det_10g.onnx")) {
        return fail("cannot open detection model: " + engine->error);
    }
    engine->provider_name = engine->using_cuda ? "cuda" : "cpu";

    if (err && err_len > 0) err[0] = '\0';
    return engine.release();
}

void ff_engine_destroy(ff_engine* engine) {
    delete engine;
}

const char* ff_engine_last_error(const ff_engine* engine) {
    return engine ? engine->error.c_str() : "null engine";
}

const char* ff_engine_provider(const ff_engine* engine) {
    return engine ? engine->provider_name.c_str() : "none";
}

int32_t ff_engine_unload(ff_engine* engine) {
    if (!engine) return FF_ERR_INVALID;
    std::lock_guard<std::mutex> lock(engine->mu);
    engine->unload_all();
    return FF_OK;
}

int32_t ff_engine_sessions_loaded(const ff_engine* engine) {
    if (!engine) return 0;
    std::lock_guard<std::mutex> lock(const_cast<ff_engine*>(engine)->mu);
    return static_cast<int32_t>(engine->sessions.size());
}

int32_t ff_engine_set_enhancer(ff_engine* engine, int32_t enhancer) {
    if (!engine) return FF_ERR_INVALID;
    if (enhancer != FF_ENHANCE_NONE && enhancer != FF_ENHANCE_GFPGAN &&
        enhancer != FF_ENHANCE_ESRGAN_X2 && enhancer != FF_ENHANCE_ESRGAN_X4) {
        return FF_ERR_INVALID;
    }
    std::lock_guard<std::mutex> lock(engine->mu);
    engine->opt.enhance = enhancer;
    if (enhancer != FF_ENHANCE_GFPGAN) engine->unload("gfpgan");
    if (enhancer != FF_ENHANCE_ESRGAN_X2) engine->unload("esrgan2");
    if (enhancer != FF_ENHANCE_ESRGAN_X4) engine->unload("esrgan4");
    return FF_OK;
}

int32_t ff_engine_set_restore_mouth(ff_engine* engine, int32_t on) {
    if (!engine) return FF_ERR_INVALID;
    std::lock_guard<std::mutex> lock(engine->mu);
    engine->opt.restore_mouth = on ? 1 : 0;
    return FF_OK;
}

int32_t ff_detect(ff_engine* engine, const uint8_t* bgr, int32_t width, int32_t height,
                  int32_t stride, ff_face* out_faces, int32_t max_faces,
                  int32_t* out_count) {
    if (!engine || !bgr || width <= 0 || height <= 0) return FF_ERR_INVALID;
    std::lock_guard<std::mutex> lock(engine->mu);
    engine->clear_error();
    engine->stats = ff_stats{};
    try {
        cv::Mat img = wrap_bgr(bgr, width, height, stride);
        std::vector<ff_face> faces;
        const auto t0 = Clock::now();
        const int rc = detect_faces(*engine, img, faces);
        engine->stats.detect_ms = ms_since(t0);
        if (rc != FF_OK) return rc;

        const int n = std::min<int32_t>(static_cast<int32_t>(faces.size()),
                                        max_faces > 0 ? max_faces : 0);
        if (out_faces && n > 0) std::memcpy(out_faces, faces.data(), n * sizeof(ff_face));
        if (out_count) *out_count = n;
        engine->stats.faces_detected = n;
        engine->stats.provider = engine->using_cuda ? FF_PROVIDER_CUDA : FF_PROVIDER_CPU;
        engine->stats.sessions_loaded = static_cast<int32_t>(engine->sessions.size());
        engine->stats.total_ms = ms_since(t0);
        return FF_OK;
    } catch (const std::exception& ex) {
        engine->set_error(ex.what());
        return FF_ERR_RUNTIME;
    }
}

int32_t ff_embed(ff_engine* engine, const uint8_t* bgr, int32_t width, int32_t height,
                 int32_t stride, const ff_face* face, float* out_embedding) {
    if (!engine || !bgr || !face || !out_embedding) return FF_ERR_INVALID;
    std::lock_guard<std::mutex> lock(engine->mu);
    engine->clear_error();
    try {
        cv::Mat img = wrap_bgr(bgr, width, height, stride);
        const auto t0 = Clock::now();
        const int rc = embed_face(*engine, img, *face, out_embedding);
        engine->stats.embed_ms = ms_since(t0);
        engine->stats.provider = engine->using_cuda ? FF_PROVIDER_CUDA : FF_PROVIDER_CPU;
        engine->stats.sessions_loaded = static_cast<int32_t>(engine->sessions.size());
        return rc;
    } catch (const std::exception& ex) {
        engine->set_error(ex.what());
        return FF_ERR_RUNTIME;
    }
}

int32_t ff_swap_face(ff_engine* engine, const uint8_t* target_bgr, int32_t width,
                     int32_t height, int32_t stride, const float* source_embedding,
                     const ff_face* face, int32_t paste, uint8_t* out_bgr) {
    if (!engine || !target_bgr || !source_embedding || !face || !out_bgr)
        return FF_ERR_INVALID;
    std::lock_guard<std::mutex> lock(engine->mu);
    engine->clear_error();
    try {
        cv::Mat img = wrap_bgr(target_bgr, width, height, stride);
        const auto t0 = Clock::now();
        cv::Mat patch, matrix;
        int rc = swap_patch(*engine, img, *face, source_embedding, patch, matrix);
        if (rc != FF_OK) return rc;

        cv::Mat result;
        if (paste) {
            paste_back(img, patch, matrix, result);
        } else {
            result = cv::Mat(height, width, CV_8UC3, cv::Scalar(0, 0, 0));
            const int cw = std::min(width, 128);
            const int ch = std::min(height, 128);
            patch(cv::Rect(0, 0, cw, ch)).copyTo(result(cv::Rect(0, 0, cw, ch)));
        }
        const int out_stride = stride > 0 ? stride : width * 3;
        for (int y = 0; y < height; ++y) {
            std::memcpy(out_bgr + static_cast<size_t>(y) * out_stride,
                        result.ptr<uint8_t>(y), static_cast<size_t>(width) * 3);
        }
        engine->stats.swap_ms = ms_since(t0);
        engine->stats.faces_swapped = 1;
        engine->stats.provider = engine->using_cuda ? FF_PROVIDER_CUDA : FF_PROVIDER_CPU;
        engine->stats.sessions_loaded = static_cast<int32_t>(engine->sessions.size());
        return FF_OK;
    } catch (const std::exception& ex) {
        engine->set_error(ex.what());
        return FF_ERR_RUNTIME;
    }
}

int32_t ff_swap_patch(ff_engine* engine, const uint8_t* bgr, int32_t width, int32_t height,
                      int32_t stride, const float* source_embedding, const ff_face* face,
                      uint8_t** out_patch, double* out_matrix) {
    if (!engine || !bgr || !source_embedding || !face || !out_patch || !out_matrix)
        return FF_ERR_INVALID;
    std::lock_guard<std::mutex> lock(engine->mu);
    engine->clear_error();
    try {
        cv::Mat img = wrap_bgr(bgr, width, height, stride);
        cv::Mat patch, matrix;
        const auto t0 = Clock::now();
        int rc = swap_patch(*engine, img, *face, source_embedding, patch, matrix);
        if (rc != FF_OK) return rc;

        const size_t bytes = 128 * 128 * 3;
        auto* buf = static_cast<uint8_t*>(std::malloc(bytes));
        if (!buf) return FF_ERR_NOMEM;
        for (int y = 0; y < 128; ++y) {
            std::memcpy(buf + static_cast<size_t>(y) * 128 * 3, patch.ptr<uint8_t>(y),
                        128 * 3);
        }
        *out_patch = buf;
        for (int i = 0; i < 6; ++i) {
            out_matrix[i] = matrix.at<double>(i / 3, i % 3);
        }
        engine->stats.swap_ms = ms_since(t0);
        engine->stats.faces_swapped = 1;
        engine->stats.provider = engine->using_cuda ? FF_PROVIDER_CUDA : FF_PROVIDER_CPU;
        engine->stats.sessions_loaded = static_cast<int32_t>(engine->sessions.size());
        return FF_OK;
    } catch (const std::exception& ex) {
        engine->set_error(ex.what());
        return FF_ERR_RUNTIME;
    }
}

int32_t ff_paste_patch(const uint8_t* canvas_bgr, int32_t width, int32_t height,
                       int32_t stride, const uint8_t* patch, const double* matrix,
                       uint8_t* out_bgr) {
    if (!canvas_bgr || !patch || !matrix || !out_bgr || width <= 0 || height <= 0)
        return FF_ERR_INVALID;
    try {
        cv::Mat canvas = wrap_bgr(canvas_bgr, width, height, stride);
        cv::Mat patch_mat(128, 128, CV_8UC3, const_cast<uint8_t*>(patch));
        cv::Mat m(2, 3, CV_64F);
        for (int i = 0; i < 6; ++i) m.at<double>(i / 3, i % 3) = matrix[i];
        cv::Mat result;
        paste_back(canvas, patch_mat, m, result);
        const int out_stride = stride > 0 ? stride : width * 3;
        for (int y = 0; y < height; ++y) {
            std::memcpy(out_bgr + static_cast<size_t>(y) * out_stride,
                        result.ptr<uint8_t>(y), static_cast<size_t>(width) * 3);
        }
        return FF_OK;
    } catch (const std::exception&) {
        return FF_ERR_RUNTIME;
    }
}

int32_t ff_enhance(ff_engine* engine, const uint8_t* bgr, int32_t width, int32_t height,
                   int32_t stride, const ff_face* faces, int32_t face_count,
                   uint8_t* out_bgr, ff_progress_fn cb, void* user_data) {
    if (!engine || !bgr || !out_bgr) return FF_ERR_INVALID;
    if (engine->opt.enhance == FF_ENHANCE_NONE) return FF_ERR_UNSUPPORTED;
    std::lock_guard<std::mutex> lock(engine->mu);
    engine->clear_error();
    try {
        cv::Mat img = wrap_bgr(bgr, width, height, stride);
        const auto t0 = Clock::now();

        int rc = FF_OK;
        cv::Mat result;
        if (engine->opt.enhance == FF_ENHANCE_GFPGAN) {
            std::vector<ff_face> detected;
            if (!faces || face_count <= 0) {
                rc = detect_faces(*engine, img, detected);
                if (rc != FF_OK) return rc;
            } else {
                detected.assign(faces, faces + face_count);
            }
            if (engine->opt.sequential) engine->unload("det");
            rc = enhance_gfpgan(*engine, img, detected, result, cb, user_data);
        } else {
            const int scale = engine->opt.enhance == FF_ENHANCE_ESRGAN_X4 ? 4 : 2;
            rc = enhance_esrgan(*engine, img, scale, result, cb, user_data);
        }
        if (rc != FF_OK) return rc;

        const int out_stride = stride > 0 ? stride : width * 3;
        for (int y = 0; y < height; ++y) {
            std::memcpy(out_bgr + static_cast<size_t>(y) * out_stride,
                        result.ptr<uint8_t>(y), static_cast<size_t>(width) * 3);
        }
        engine->stats.enhance_ms = ms_since(t0);
        engine->stats.provider = engine->using_cuda ? FF_PROVIDER_CUDA : FF_PROVIDER_CPU;
        engine->stats.sessions_loaded = static_cast<int32_t>(engine->sessions.size());
        return FF_OK;
    } catch (const std::exception& ex) {
        engine->set_error(ex.what());
        return FF_ERR_RUNTIME;
    }
}

int32_t ff_grain_match(ff_engine* engine, const uint8_t* result_bgr, int32_t result_w,
                       int32_t result_h, int32_t result_stride, uint8_t* out_bgr,
                       const uint8_t* reference_bgr, int32_t reference_w, int32_t reference_h,
                       int32_t reference_stride, const ff_face* faces, int32_t face_count,
                       int32_t strength, ff_progress_fn cb, void* user_data) {
    if (!engine || !result_bgr || !out_bgr || !reference_bgr) return FF_ERR_INVALID;
    if (result_w <= 0 || result_h <= 0 || reference_w <= 0 || reference_h <= 0) {
        return FF_ERR_INVALID;
    }
    if (strength <= 0) {
        // Nothing to do, but still normalise the output.
        std::memcpy(out_bgr, result_bgr, static_cast<size_t>(result_w) * result_h * 3);
        return FF_OK;
    }
    std::lock_guard<std::mutex> lock(engine->mu);
    engine->clear_error();
    try {
        cv::Mat result = wrap_bgr(result_bgr, result_w, result_h, result_stride);
        cv::Mat reference = wrap_bgr(reference_bgr, reference_w, reference_h, reference_stride);
        std::vector<ff_face> f;
        if (faces && face_count > 0) f.assign(faces, faces + face_count);

        // out_bgr is documented as tightly packed (stride == width * 3).
        cv::Mat outimg(result_h, result_w, CV_8UC3, out_bgr);
        const auto t0 = Clock::now();
        const int rc =
            grain_match_faces(*engine, result, reference, f, strength, outimg, cb, user_data);
        if (rc != FF_OK) return rc;
        engine->stats.enhance_ms = ms_since(t0);
        engine->stats.provider = engine->using_cuda ? FF_PROVIDER_CUDA : FF_PROVIDER_CPU;
        engine->stats.sessions_loaded = static_cast<int32_t>(engine->sessions.size());
        return FF_OK;
    } catch (const std::exception& ex) {
        engine->set_error(std::string("ff_grain_match: ") + ex.what());
        return FF_ERR_RUNTIME;
    }
}

int32_t ff_upscale(ff_engine* engine, const uint8_t* bgr, int32_t width, int32_t height,
                   int32_t stride, int32_t scale, uint8_t** out_bgr, int32_t* out_width,
                   int32_t* out_height, ff_progress_fn cb, void* user_data) {
    if (!engine || !bgr || !out_bgr) return FF_ERR_INVALID;
    if (scale != 2 && scale != 4) return FF_ERR_INVALID;
    *out_bgr = nullptr;
    std::lock_guard<std::mutex> lock(engine->mu);
    engine->clear_error();
    try {
        // Keep only the session we are about to use so the memory budget holds
        // when the user alternates between x2 and x4.
        engine->unload(scale == 4 ? "esrgan2" : "esrgan4");

        cv::Mat img = wrap_bgr(bgr, width, height, stride);
        cv::Mat result;
        const auto t0 = Clock::now();
        const int rc = upscale_esrgan(*engine, img, scale, result, cb, user_data);
        if (rc != FF_OK) return rc;

        const int ow = result.cols;
        const int oh = result.rows;
        const size_t bytes = static_cast<size_t>(ow) * static_cast<size_t>(oh) * 3;
        auto* data = static_cast<uint8_t*>(std::malloc(bytes));
        if (!data) return FF_ERR_RUNTIME;
        for (int y = 0; y < oh; ++y) {
            std::memcpy(data + static_cast<size_t>(y) * static_cast<size_t>(ow) * 3,
                        result.ptr<uint8_t>(y), static_cast<size_t>(ow) * 3);
        }
        *out_bgr = data;
        if (out_width) *out_width = ow;
        if (out_height) *out_height = oh;
        engine->stats.enhance_ms = ms_since(t0);
        engine->stats.provider = engine->using_cuda ? FF_PROVIDER_CUDA : FF_PROVIDER_CPU;
        engine->stats.sessions_loaded = static_cast<int32_t>(engine->sessions.size());
        return FF_OK;
    } catch (const std::exception& ex) {
        engine->set_error(std::string("ff_upscale: ") + ex.what());
        return FF_ERR_RUNTIME;
    }
}

int32_t ff_process(ff_engine* engine, const uint8_t* target_bgr, int32_t target_w,
                   int32_t target_h, int32_t target_stride, const uint8_t* source_bgr,
                   int32_t source_w, int32_t source_h, int32_t source_stride,
                   uint8_t* out_bgr, ff_progress_fn cb, void* user_data) {
    if (!engine || !target_bgr || !source_bgr || !out_bgr) return FF_ERR_INVALID;
    std::lock_guard<std::mutex> lock(engine->mu);
    engine->clear_error();
    const auto total_t0 = Clock::now();
    engine->stats = ff_stats{};

    auto progress = [&](int stage, float p, const char* msg) {
        if (cb) cb(stage, p, msg, user_data);
    };

    try {
        cv::Mat target = wrap_bgr(target_bgr, target_w, target_h, target_stride);
        cv::Mat source = wrap_bgr(source_bgr, source_w, source_h, source_stride);

        // 1. Detect target faces and source face.
        progress(FF_STAGE_DETECT, 0.0f, "detecting faces");
        const auto t_det = Clock::now();
        std::vector<ff_face> target_faces, source_faces;
        int rc = detect_faces(*engine, target, target_faces);
        if (rc != FF_OK) return rc;
        engine->stats.detect_ms += ms_since(t_det);

        const auto t_det2 = Clock::now();
        rc = detect_faces(*engine, source, source_faces);
        if (rc != FF_OK) return rc;
        engine->stats.detect_ms += ms_since(t_det2);
        engine->stats.faces_detected = static_cast<int32_t>(target_faces.size());
        if (source_faces.empty()) {
            engine->set_error("no face found in the source image");
            return FF_ERR_RUNTIME;
        }
        progress(FF_STAGE_DETECT, 1.0f, "detecting faces");

        // 2. Embed the most prominent source face.
        progress(FF_STAGE_EMBED, 0.0f, "embedding source");
        const auto t_emb = Clock::now();
        ff_face source_face = pick_best_face(source_faces);
        float embedding[512];
        rc = embed_face(*engine, source, source_face, embedding);
        if (rc != FF_OK) return rc;
        engine->stats.embed_ms = ms_since(t_emb);
        progress(FF_STAGE_EMBED, 1.0f, "embedding source");

        if (engine->opt.sequential) engine->unload("det");

        // 3. Swap every target face.
        cv::Mat swapped = target.clone();
        const auto t_swap = Clock::now();
        for (size_t i = 0; i < target_faces.size(); ++i) {
            progress(FF_STAGE_SWAP, static_cast<float>(i) / target_faces.size(),
                     "swapping faces");
            cv::Mat patch, matrix, next;
            rc = swap_patch(*engine, swapped, target_faces[i], embedding, patch, matrix);
            if (rc != FF_OK) return rc;
            paste_back(swapped, patch, matrix, next);
            swapped = next;
            engine->stats.faces_swapped++;
        }
        engine->stats.swap_ms = ms_since(t_swap);
        progress(FF_STAGE_SWAP, 1.0f, "swapping faces");

        if (engine->opt.sequential) engine->unload("arc");

        // 4. Optional enhancement.
        if (engine->opt.sequential) engine->unload("det");
        if (engine->opt.enhance == FF_ENHANCE_GFPGAN) {
            const auto t_enh = Clock::now();
            cv::Mat enhanced;
            rc = enhance_gfpgan(*engine, swapped, target_faces, enhanced, cb, user_data);
            if (rc != FF_OK) return rc;
            swapped = enhanced;
            engine->stats.enhance_ms = ms_since(t_enh);
        } else if (engine->opt.enhance == FF_ENHANCE_ESRGAN_X2 ||
                   engine->opt.enhance == FF_ENHANCE_ESRGAN_X4) {
            const auto t_enh = Clock::now();
            const int scale = engine->opt.enhance == FF_ENHANCE_ESRGAN_X4 ? 4 : 2;
            cv::Mat enhanced;
            rc = enhance_esrgan(*engine, swapped, scale, enhanced, cb, user_data);
            if (rc != FF_OK) return rc;
            swapped = enhanced;
            engine->stats.enhance_ms = ms_since(t_enh);
        }
        if (engine->opt.sequential) engine->unload_all();

        const int out_stride = target_stride > 0 ? target_stride : target_w * 3;
        for (int y = 0; y < target_h; ++y) {
            std::memcpy(out_bgr + static_cast<size_t>(y) * out_stride,
                        swapped.ptr<uint8_t>(y), static_cast<size_t>(target_w) * 3);
        }

        engine->stats.total_ms = ms_since(total_t0);
        engine->stats.provider = engine->using_cuda ? FF_PROVIDER_CUDA : FF_PROVIDER_CPU;
        engine->stats.sessions_loaded = static_cast<int32_t>(engine->sessions.size());
        progress(FF_STAGE_DONE, 1.0f, "done");
        return FF_OK;
    } catch (const std::exception& ex) {
        engine->set_error(ex.what());
        return FF_ERR_RUNTIME;
    }
}

int32_t ff_last_stats(const ff_engine* engine, ff_stats* out_stats) {
    if (!engine || !out_stats) return FF_ERR_INVALID;
    std::lock_guard<std::mutex> lock(const_cast<ff_engine*>(engine)->mu);
    *out_stats = engine->stats;
    return FF_OK;
}

int32_t ff_image_load(const char* path, uint8_t** out_bgr, int32_t* out_width,
                      int32_t* out_height, int32_t* out_stride) {
    if (!path || !out_bgr || !out_width || !out_height || !out_stride)
        return FF_ERR_INVALID;
    try {
        // Read through std::filesystem so UTF-8 paths work on Windows, where
        // cv::imread's narrow path is interpreted as ANSI.
        std::ifstream file(std::filesystem::u8path(path), std::ios::binary);
        if (!file) return FF_ERR_IO;
        const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                                         std::istreambuf_iterator<char>());
        if (bytes.empty()) return FF_ERR_IO;
        cv::Mat img = cv::imdecode(bytes, cv::IMREAD_COLOR);
        if (img.empty()) return FF_ERR_IO;
        if (!img.isContinuous()) img = img.clone();
        const size_t bytes_out = img.total() * 3;
        auto* data = static_cast<uint8_t*>(std::malloc(bytes_out));
        if (!data) return FF_ERR_NOMEM;
        std::memcpy(data, img.data, bytes_out);
        *out_bgr = data;
        *out_width = img.cols;
        *out_height = img.rows;
        *out_stride = img.cols * 3;
        return FF_OK;
    } catch (const std::exception&) {
        return FF_ERR_RUNTIME;
    }
}

void ff_image_free(uint8_t* data) {
    std::free(data);
}

int32_t ff_image_save(const char* path, const uint8_t* bgr, int32_t width,
                      int32_t height, int32_t stride) {
    if (!path || !bgr || width <= 0 || height <= 0) return FF_ERR_INVALID;
    try {
        cv::Mat img = wrap_bgr(bgr, width, height, stride);
        const std::filesystem::path fspath = std::filesystem::u8path(path);
        std::string ext = fspath.extension().string();
        if (ext.empty()) ext = ".png";
        std::vector<uint8_t> buf;
        if (!cv::imencode(ext, img, buf)) return FF_ERR_IO;
        std::ofstream file(fspath, std::ios::binary);
        if (!file) return FF_ERR_IO;
        file.write(reinterpret_cast<const char*>(buf.data()),
                   static_cast<std::streamsize>(buf.size()));
        return file.good() ? FF_OK : FF_ERR_IO;
    } catch (const std::exception&) {
        return FF_ERR_RUNTIME;
    }
}

int32_t ff_image_decode(const uint8_t* data, int32_t size, uint8_t** out_bgr,
                        int32_t* out_width, int32_t* out_height, int32_t* out_stride) {
    if (!data || size <= 0 || !out_bgr || !out_width || !out_height || !out_stride)
        return FF_ERR_INVALID;
    try {
        const std::vector<uint8_t> bytes(data, data + size);
        cv::Mat img = cv::imdecode(bytes, cv::IMREAD_COLOR);
        if (img.empty()) return FF_ERR_IO;
        if (!img.isContinuous()) img = img.clone();
        const size_t n = img.total() * 3;
        auto* buf = static_cast<uint8_t*>(std::malloc(n));
        if (!buf) return FF_ERR_NOMEM;
        std::memcpy(buf, img.data, n);
        *out_bgr = buf;
        *out_width = img.cols;
        *out_height = img.rows;
        *out_stride = img.cols * 3;
        return FF_OK;
    } catch (const std::exception&) {
        return FF_ERR_RUNTIME;
    }
}

int32_t ff_image_encode_png(const uint8_t* bgr, int32_t width, int32_t height,
                            int32_t stride, uint8_t** out_data, int32_t* out_size) {
    if (!bgr || width <= 0 || height <= 0 || !out_data || !out_size)
        return FF_ERR_INVALID;
    try {
        cv::Mat img = wrap_bgr(bgr, width, height, stride);
        std::vector<uint8_t> buf;
        if (!cv::imencode(".png", img, buf)) return FF_ERR_IO;
        auto* out = static_cast<uint8_t*>(std::malloc(buf.size()));
        if (!out) return FF_ERR_NOMEM;
        std::memcpy(out, buf.data(), buf.size());
        *out_data = out;
        *out_size = static_cast<int32_t>(buf.size());
        return FF_OK;
    } catch (const std::exception&) {
        return FF_ERR_RUNTIME;
    }
}

int32_t ff_image_resize(const uint8_t* bgr, int32_t width, int32_t height,
                        int32_t stride, int32_t new_width, int32_t new_height,
                        uint8_t** out_bgr, int32_t* out_stride) {
    if (!bgr || width <= 0 || height <= 0 || new_width <= 0 || new_height <= 0 ||
        !out_bgr || !out_stride)
        return FF_ERR_INVALID;
    try {
        cv::Mat img = wrap_bgr(bgr, width, height, stride);
        cv::Mat out;
        const int interp = (new_width < width || new_height < height) ? cv::INTER_AREA
                                                                    : cv::INTER_LINEAR;
        cv::resize(img, out, cv::Size(new_width, new_height), 0, 0, interp);
        if (!out.isContinuous()) out = out.clone();
        const size_t n = out.total() * 3;
        auto* buf = static_cast<uint8_t*>(std::malloc(n));
        if (!buf) return FF_ERR_NOMEM;
        std::memcpy(buf, out.data, n);
        *out_bgr = buf;
        *out_stride = new_width * 3;
        return FF_OK;
    } catch (const std::exception&) {
        return FF_ERR_RUNTIME;
    }
}

int32_t ff_image_clone(const uint8_t* bgr, int32_t width, int32_t height,
                       int32_t stride, uint8_t** out_bgr, int32_t* out_stride) {
    if (!bgr || width <= 0 || height <= 0 || !out_bgr || !out_stride)
        return FF_ERR_INVALID;
    try {
        cv::Mat img = wrap_bgr(bgr, width, height, stride);
        const size_t n = static_cast<size_t>(width) * height * 3;
        auto* buf = static_cast<uint8_t*>(std::malloc(n));
        if (!buf) return FF_ERR_NOMEM;
        for (int y = 0; y < height; ++y) {
            std::memcpy(buf + static_cast<size_t>(y) * width * 3,
                        img.ptr<uint8_t>(y), static_cast<size_t>(width) * 3);
        }
        *out_bgr = buf;
        *out_stride = width * 3;
        return FF_OK;
    } catch (const std::exception&) {
        return FF_ERR_RUNTIME;
    }
}

int32_t ff_blend_mask(uint8_t* dst_bgr, const uint8_t* original_bgr,
                      int32_t width, int32_t height,
                      const uint8_t* mask, int32_t mask_w, int32_t mask_h) {
    if (!dst_bgr || !original_bgr || !mask || width <= 0 || height <= 0 ||
        mask_w <= 0 || mask_h <= 0)
        return FF_ERR_INVALID;

    const bool same_size = (mask_w == width && mask_h == height);
#if FF_X86 && (defined(__GNUC__) || defined(__clang__))
    const bool use_simd = have_avx2_fma();
#endif
    if (same_size) {
#if FF_X86 && (defined(__GNUC__) || defined(__clang__))
        if (use_simd) {
            blend_mask_same_size_avx2(dst_bgr, original_bgr, mask, width, height);
            return FF_OK;
        }
#endif
        blend_mask_same_size_scalar(dst_bgr, original_bgr, mask, width, height);
        return FF_OK;
    }
    // A mask at a different resolution (downscaled preview) is resampled once
    // with OpenCV and then blended with the same-size kernel. The half-pixel
    // mapping matches the scalar fallback below.
    try {
        cv::Mat src_mask(mask_h, mask_w, CV_8UC1, const_cast<uint8_t*>(mask));
        cv::Mat scaled;
        cv::resize(src_mask, scaled, cv::Size(width, height), 0, 0, cv::INTER_LINEAR);
        if (!scaled.empty() && scaled.isContinuous()) {
            const uint8_t* m = scaled.ptr<uint8_t>();
#if FF_X86 && (defined(__GNUC__) || defined(__clang__))
            if (use_simd) {
                blend_mask_same_size_avx2(dst_bgr, original_bgr, m, width, height);
            } else
#endif
            {
                blend_mask_same_size_scalar(dst_bgr, original_bgr, m, width, height);
            }
            return FF_OK;
        }
    } catch (const std::exception&) {
    }
    const float sx = static_cast<float>(mask_w) / static_cast<float>(width);
    const float sy = static_cast<float>(mask_h) / static_cast<float>(height);

    for (int y = 0; y < height; ++y) {
        uint8_t* dst = dst_bgr + static_cast<size_t>(y) * width * 3;
        const uint8_t* src = original_bgr + static_cast<size_t>(y) * width * 3;
        int y0 = y;
        int y1 = y;
        float fy = 0.0f;
        if (!same_size) {
            const float my = (static_cast<float>(y) + 0.5f) * sy - 0.5f;
            y0 = static_cast<int>(std::floor(my));
            fy = my - static_cast<float>(y0);
            y1 = y0 + 1;
            y0 = std::clamp(y0, 0, mask_h - 1);
            y1 = std::clamp(y1, 0, mask_h - 1);
        }
        const uint8_t* m0 = mask + static_cast<size_t>(y0) * mask_w;
        const uint8_t* m1 = mask + static_cast<size_t>(y1) * mask_w;

        for (int x = 0; x < width; ++x) {
            float a;
            if (same_size) {
                a = static_cast<float>(m0[x]) * (1.0f / 255.0f);
            } else {
                const float mx = (static_cast<float>(x) + 0.5f) * sx - 0.5f;
                int x0 = static_cast<int>(std::floor(mx));
                const float fx = mx - static_cast<float>(x0);
                int x1 = x0 + 1;
                x0 = std::clamp(x0, 0, mask_w - 1);
                x1 = std::clamp(x1, 0, mask_w - 1);
                const float top = (m0[x0] * (1.0f - fx) + m0[x1] * fx);
                const float bottom = (m1[x0] * (1.0f - fx) + m1[x1] * fx);
                a = (top * (1.0f - fy) + bottom * fy) * (1.0f / 255.0f);
            }
            if (a <= 0.0001f) continue;
            uint8_t* d = dst + static_cast<size_t>(x) * 3;
            const uint8_t* s = src + static_cast<size_t>(x) * 3;
            const float ia = 1.0f - a;
            for (int c = 0; c < 3; ++c) {
                const float v = static_cast<float>(d[c]) * ia + static_cast<float>(s[c]) * a;
                d[c] = static_cast<uint8_t>(v + 0.5f);
            }
        }
    }
    return FF_OK;
}

int32_t ff_memory_info(float* vram_used_mb, float* vram_total_mb, float* rss_mb) {
    if (vram_used_mb) *vram_used_mb = 0.0f;
    if (vram_total_mb) *vram_total_mb = 0.0f;
    if (rss_mb) {
#ifdef _WIN32
        PROCESS_MEMORY_COUNTERS pmc{};
        if (::GetProcessMemoryInfo(::GetCurrentProcess(), &pmc, sizeof(pmc))) {
            *rss_mb = static_cast<float>(pmc.WorkingSetSize) / (1024.0f * 1024.0f);
        }
#else
        std::ifstream f("/proc/self/statm");
        long pages = 0, resident = 0;
        if (f >> pages >> resident) {
            *rss_mb = static_cast<float>(resident) *
                      static_cast<float>(sysconf(_SC_PAGESIZE)) / (1024.0f * 1024.0f);
        }
#endif
    }
    Nvml& n = nvml();
    if (n.ok) {
        nvmlMemory_t mem{};
        if (n.get_memory(n.device, &mem) == 0) {
            if (vram_used_mb) *vram_used_mb = static_cast<float>(mem.used) / (1024.0f * 1024.0f);
            if (vram_total_mb) *vram_total_mb = static_cast<float>(mem.total) / (1024.0f * 1024.0f);
        }
    }
    return FF_OK;
}

int64_t ff_now_ns(void) {
    // Portable monotonic clock (steady on all platforms); used by the GUI so
    // it does not need any OS-specific timing code.
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

}  // extern "C"
