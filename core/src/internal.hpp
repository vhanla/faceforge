// Internal C++ helpers for the faceforge core. Not part of the public ABI.
#ifndef FF_INTERNAL_HPP
#define FF_INTERNAL_HPP

#include <onnxruntime_cxx_api.h>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#if __has_include(<opencv2/geometry.hpp>)
#include <opencv2/geometry.hpp> // OpenCV 5
#else
#include <opencv2/calib3d.hpp> // OpenCV 4
#endif

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "faceforge.h"

namespace ff {

using Clock = std::chrono::steady_clock;

inline double ms_since(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

// ---------------------------------------------------------------------------
// ORT session wrapper
// ---------------------------------------------------------------------------

class ModelSession {
public:
    ModelSession() = default;
    ModelSession(ModelSession&&) = default;
    ModelSession& operator=(ModelSession&&) = default;

    bool open(Ort::Env& env, const std::filesystem::path& path, const ff_options& opt,
              bool use_cuda, std::string* error);
    bool valid() const { return session_ != nullptr; }

    const std::vector<std::string>& input_names() const { return input_names_; }
    const std::vector<std::string>& output_names() const { return output_names_; }
    const std::vector<std::vector<int64_t>>& input_shapes() const { return input_shapes_; }
    const std::vector<std::vector<int64_t>>& output_shapes() const { return output_shapes_; }

    // Runs with the supplied float tensors, returns outputs as float tensors.
    std::vector<Ort::Value> run(std::vector<Ort::Value>& inputs);

    Ort::MemoryInfo& mem_info() { return mem_info_; }

private:
    std::unique_ptr<Ort::Session> session_;
    std::vector<std::string> input_names_, output_names_;
    std::vector<std::vector<int64_t>> input_shapes_, output_shapes_;
    Ort::MemoryInfo mem_info_{Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault)};
};

// ---------------------------------------------------------------------------
// Engine
// ---------------------------------------------------------------------------

struct Engine {
    ff_options opt{};
    std::string model_dir;
    std::string error;
    std::string provider_name = "cpu";

    std::unique_ptr<Ort::Env> env;
    bool using_cuda = false;
    bool cuda_available = false;

    std::mutex mu;
    std::map<std::string, std::unique_ptr<ModelSession>> sessions;

    ff_stats stats{};

    // Loads (or returns cached) session for `key` from `filename`.
    ModelSession* session(const std::string& key, const std::string& filename);

    // Unload a single session / all sessions.
    void unload(const std::string& key);
    void unload_all();

    void set_error(const std::string& msg) { error = msg; }
    void clear_error() { error.clear(); }

    // Stage timing helper.
    Clock::time_point stage_start;
    void begin_stage() { stage_start = Clock::now(); }
};

// ---------------------------------------------------------------------------
// Image / math utilities
// ---------------------------------------------------------------------------

cv::Mat wrap_bgr(const uint8_t* data, int width, int height, int stride);

// Similarity fit (Umeyama, same as skimage SimilarityTransform.estimate) that
// maps the five landmarks to the ArcFace template scaled to `size`.
cv::Mat arcface_matrix(const float kps[10], int size);

// Similarity fit to an arbitrary normalized 5-point template (x,y in 0..1).
cv::Mat template_matrix(const float kps[10], const float tmpl[10], int width, int height);

cv::Mat warp_face(const cv::Mat& image, const cv::Mat& matrix, int size,
                  int interpolation, int border_mode);

// NCHW float blob, RGB channel order, (x - mean) / std.
void to_blob(const cv::Mat& bgr, float mean, float scale, std::vector<float>& out);
void to_blob_rect(const cv::Mat& bgr, int x, int y, int w, int h,
                  float mean, float scale, std::vector<float>& out);

// L2 normalization in place.
void l2_normalize(float* v, int n);

// ---------------------------------------------------------------------------
// Model stages
// ---------------------------------------------------------------------------

// SCRFD
int detect_faces(Engine& e, const cv::Mat& bgr, std::vector<ff_face>& faces);

// ArcFace: 112x112 aligned crop -> normalized 512-float embedding.
int embed_face(Engine& e, const cv::Mat& bgr, const ff_face& face, float* out512);

// inswapper: returns the 128x128 BGR patch and the affine matrix used.
int swap_patch(Engine& e, const cv::Mat& bgr, const ff_face& face,
               const float* embedding, cv::Mat& fake_bgr, cv::Mat& matrix);

// Paste the aligned patch back into `target`, returning a new image.
void paste_back(const cv::Mat& target, const cv::Mat& patch, const cv::Mat& matrix,
                cv::Mat& out);

// Enhancers
int enhance_gfpgan(Engine& e, const cv::Mat& bgr, const std::vector<ff_face>& faces,
                   cv::Mat& out, ff_progress_fn cb, void* user);
int enhance_esrgan(Engine& e, const cv::Mat& bgr, int scale, cv::Mat& out,
                   ff_progress_fn cb, void* user);

// emap.bin: 512*512 little-endian float32, row-major.
bool load_emap(const std::string& path, std::vector<float>& emap);

}  // namespace ff

// The opaque C handle is the C++ engine itself.
struct ff_engine : ff::Engine {};

#endif  // FF_INTERNAL_HPP
