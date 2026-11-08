#pragma once
// Application state and background worker for FaceForge, independent of the UI
// toolkit. Direct port of the photo-mode half of gui/src/main.zig: it owns the
// engine, the pristine base image, the identity library, the per-face swap
// patch cache, the composed result and the undo/redo history, and runs all
// inference on a single background thread so the UI never blocks.
//
// Threading contract: `poll()` drives the worker from the UI thread and joins
// it when a job finishes. While `busy()` is true the UI must not read the
// worker-owned arrays (faces, patches, identities, proc); it reads `stage()`,
// `progress()` and `statusText()` instead. `facesSnapshot()` returns a locked
// copy that is safe to read at any time.

#include "config.hpp"
#include "ff.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace app {

constexpr int kMaxFaces = 32;
constexpr int kMaxIdentities = 48;
constexpr int kHistoryCap = 16;
constexpr float kPreviewLongSide = 640.0f;
constexpr int kPatchLevels = 2;
constexpr int kFullLevel = 0;
constexpr int kPreviewLevel = 1;
constexpr int kThumbSize = 72;

enum class Mode { Photo, Video };
enum class Enhancer : int { None = 0, Gfpgan = 1, EsrganX2 = 2, EsrganX4 = 3 };
enum class Stage : uint32_t {
    Idle = 0,
    Loading,
    Detecting,
    Swapping,
    Enhancing,
    Compositing,
    Done,
};
enum class JobKind : uint32_t {
    None = 0,
    Init = 1,
    ReloadTarget = 2,
    AddIdentity = 3,
    ProcessAll = 4,
    ProcessOne = 5,
    Compose = 6,
};
enum class ExportKind { None, Save, Copy, ApplyInput };

const char* stageText(Stage s);

struct Timings {
    double detect_ms = 0;
    double embed_ms = 0;
    double swap_ms = 0;
    double enhance_ms = 0;
    double total_ms = 0;
};

// A cached 128x128 swap result plus the affine matrix that places it back in
// the working image. The metadata records exactly how the patch was produced
// so it can be reused until any of those inputs change.
struct Patch {
    ff::Image img;
    double matrix[6] = {0, 0, 0, 0, 0, 0};
    bool valid = false;
    int identity = -1;
    bool mouth = false;
    uint32_t id_gen = 0;
    int32_t proc_w = 0;
    int32_t proc_h = 0;
};

struct Thumb {
    ff::Image img;
    bool dirty = false;
};

// One selectable identity face: an ArcFace embedding plus a cropped thumbnail
// and a human-readable label ("lena.jpg #0").
struct Identity {
    float embedding[512] = {};
    Thumb thumb;
    std::string label;
};

// A locked snapshot of the per-face state for the UI.
struct FacesSnapshot {
    int count = 0;
    ff_face faces[kMaxFaces]{};
    bool sel[kMaxFaces]{};
    int identity[kMaxFaces]{};   // -1 = use the default identity
    bool mouth_set[kMaxFaces]{};
    bool mouth[kMaxFaces]{};
    bool patch_valid[kMaxFaces]{};
};

// A locked snapshot of the identity library labels (the UI never touches the
// embeddings or thumbnails directly).
struct IdentitiesSnapshot {
    int count = 0;
    int default_identity = 0;
    uint32_t gen = 0;
    std::string labels[kMaxIdentities];
};

// An identity image decoded and waiting to be scanned by the worker.
struct PendingIdentity {
    ff::Image img;
    std::string label;
};

struct App {
    App();
    ~App();

    App(const App&) = delete;
    App& operator=(const App&) = delete;

    // ---- settings & paths ----
    config::Values settings;
    std::string config_path;
    bool config_enabled = false;
    bool config_dirty = false;
    bool save_explicit = false;

    std::string model_dir = "models";
    std::string temp_dir;
    std::string export_dir;
    std::string target_path;
    std::string source_path;
    std::string save_path;

    // ---- options (workspace panels; not persisted) ----
    uint8_t provider = 0;
    Mode mode = Mode::Photo;
    Enhancer enhancer = Enhancer::Gfpgan;
    bool restore_mouth = true;
    bool fast_preview = true;
    bool unload_after_run = false;
    bool show_overlay = true;
    bool photo_view_original = false;
    bool show_settings = false;
    bool show_about = false;
    bool check_updates = true;
    uint32_t update_interval_hours = 24;
    int64_t last_update_check = 0;

    // ---- engine ----
    ff::Engine engine;
    std::string provider_name;

    // ---- pristine base image (owned) ----
    ff::Image base;
    uint64_t base_gen = 0;
    bool base_dirty = false;

    // ---- identity library (worker-owned while busy) ----
    std::vector<Identity> identities;
    int default_identity = 0;
    uint32_t identity_gen = 0;

    // ---- composed result (published to the UI thread by poll) ----
    ff::Image result;
    uint64_t result_sig = 0;
    int32_t result_level = -1;
    bool result_dirty = false;

    // ---- Studio retouch mask (UI thread only) ----
    // The mask is base-resolution and holds the "paint the original back"
    // amount; display is the result with that mask blended in, used by the
    // canvas and by save/copy/apply.
    bool studio = false;
    bool studio_erase = true;
    float studio_size = 60.0f;
    float studio_hard = 0.6f;
    float studio_opac = 0.9f;
    bool studio_show_mask = true;
    bool studio_view_original = false;
    bool mask_used = false;
    uint32_t mask_gen = 0;
    int mask_undo_len = 0;
    int mask_redo_len = 0;
    ff::Image display;
    bool display_dirty = false;

    // ---- working image ----
    ff::Image proc; // owned (preview) or a borrowed view of base
    int32_t proc_w = 0;
    int32_t proc_h = 0;
    int proc_level = kFullLevel;

    Timings timings;

    // ---- per-face state (worker-owned while busy; guard with mu) ----
    ff_face faces[kMaxFaces]{};
    int face_count = 0;
    bool face_sel[kMaxFaces]{};
    int face_identity[kMaxFaces]{};
    bool face_mouth_set[kMaxFaces]{};
    bool face_mouth[kMaxFaces]{};
    Thumb face_thumb[kMaxFaces];
    Patch patches[kPatchLevels][kMaxFaces];

    // ---- undo / redo of the base image ----
    std::vector<ff::Image> undo;
    std::vector<ff::Image> redo;

    // ---- UI-thread published status ----
    std::string status_text = "Ready";
    bool status_is_error = false;
    Timings timings_published;
    std::string provider_published;

    // Image clipboard payload produced by copyResult() for the platform layer.
    std::vector<uint8_t> clipboard_png;

    // ------------------------------------------------------------------
    // Public operations (call from the UI thread).
    // ------------------------------------------------------------------
    void loadConfig(const std::string& path);
    void saveConfig();
    void markConfigDirty();

    void openTarget(const std::string& path);
    void addIdentityFile(const std::string& path);
    void addIdentityImage(ff::Image&& img, const std::string& label);
    void removeIdentity(int index);
    void clearIdentities();
    void selectDefaultIdentity(int index);

    void processAll();
    void processOne(int index);
    void recompose();

    void setFaceSelected(int index, bool on);
    void toggleFace(int index);
    void setFaceIdentity(int index, int identity);
    void setFaceMouth(int index, int mode); // -1 default, 0 off, 1 on

    void saveResult(const std::string& path);
    void copyResult();
    void applyResultAsInput();
    void clearTarget();

    void undoHistory();
    void redoHistory();

    // ---- Studio retouch (call from the UI thread) ----
    // Ensures the mask matches the current base image. Returns false when the
    // base is missing or allocation fails.
    bool studioEnsureMask();
    // Drops the mask, undo/redo and the blended display image. Called whenever
    // the base changes so stale retouches cannot leak into another picture.
    void clearStudio();
    // Clears the paint but keeps the mask allocated.
    void studioReset();
    void studioBegin(float px, float py);
    void studioTo(float px, float py);
    void studioEnd();
    void maskUndo();
    void maskRedo();
    // The image the user sees and exports: result with the mask blended in,
    // or the raw result when nothing is painted. Rebuilds on demand.
    const ff::Image* displayImage();
    // Reduced straight-alpha RGBA preview of the mask (max 900 px long side),
    // or nullptr when no retouch is active. Valid until the next mask change.
    const uint8_t* maskPreviewData(int* out_w, int* out_h);
    // Marks the blended display view stale so the next displayImage() rebuilds
    // it. Call after Studio settings change.
    void invalidateView();

    // Advances the worker from the UI thread; call once per frame.
    void poll();

    // Worker-safe status reporting (touches only atomics).
    void setStage(Stage st, int permille);
    void setProgress(int permille);
    void setStatus(const std::string& text, bool is_error);

    bool busy() const { return working_.load(std::memory_order_acquire); }
    int progressPermille() const {
        return static_cast<int>(progress_value_.load(std::memory_order_relaxed));
    }
    Stage stage() const {
        return static_cast<Stage>(stage_value_.load(std::memory_order_relaxed));
    }

    // UI-thread-only views of the last published job state.
    const std::string& statusText() const { return status_text; }
    bool statusIsError() const { return status_is_error; }
    const Timings& lastTimings() const { return timings_published; }
    const std::string& providerName() const { return provider_published; }
    int undoCount() const { return static_cast<int>(undo.size()); }
    int redoCount() const { return static_cast<int>(redo.size()); }

    FacesSnapshot facesSnapshot() const;
    IdentitiesSnapshot identitiesSnapshot() const;
    // True when at least one identity is available.
    bool hasIdentities() const;
    bool undoEmpty() const { return undo.empty(); }
    bool redoEmpty() const { return redo.empty(); }

    // ---- video workspace bridge (thread-safe) ----
    // The video worker runs on its own thread but shares the engine and the
    // identity library with the photo worker; the two never run concurrently
    // (the video job refuses to start while the photo worker is busy, and
    // spawnJob refuses while a video job is active).
    int identityCount() const;
    bool identityEmbedding(int index, float out[512]) const;
    int defaultIdentity() const;
    bool videoBusy() const { return video_busy_.load(std::memory_order_acquire); }
    void setVideoBusy(bool on) { video_busy_.store(on, std::memory_order_release); }
    // Creates or reuses an engine for the video worker. Returns nullptr on
    // failure and stores the message in videoEngineError().
    ff::Engine* ensureVideoEngine(bool sequential);
    const std::string& videoEngineError() const { return video_engine_error_; }

    // Visits each identity thumbnail while holding the library lock, so the UI
    // can upload textures without racing the worker appending new identities.
    // The image pointers are valid for the duration of the call only.
    template <typename Fn>
    void eachIdentityThumb(Fn&& fn) const {
        std::lock_guard<std::mutex> lk(ident_mu_);
        const int n = std::min(static_cast<int>(identities.size()), kMaxIdentities);
        for (int i = 0; i < n; ++i) fn(i, identities[static_cast<size_t>(i)].thumb.img);
    }

private:
    // worker plumbing
    void spawnJob(JobKind kind, int arg);
    void workerMain(JobKind kind);
    bool runJob(JobKind kind);

    bool ensureEngine();
    bool prepareProc();
    bool drainPendingIdentities(bool* added);
    bool detectTarget();
    bool swapAll();
    bool swapOneFace(int index);
    bool composeResult();

    int effectiveIdentity(int index) const;
    bool effectiveMouth(int index) const;
    bool patchFresh(int index) const;
    ff_face scaledFace(int index) const;
    uint64_t composeSignature() const;

    void takePendingResult();
    void buildFaceThumbs();
    void clearResult();
    void clearPatches();
    void freeProc();
    void pushUndo(ff::Image&& img);
    void clearRedo();

    // Studio retouch internals (UI thread only).
    struct MaskEdit {
        int32_t x = 0;
        int32_t y = 0;
        int32_t w = 0;
        int32_t h = 0;
        std::vector<uint8_t> data;
    };
    void updateDisplay();
    const ff::Image* originalFor(int32_t w, int32_t h);
    void maskStampAt(float cx, float cy, bool erase);
    std::vector<uint8_t> copyMaskRegionVec(const std::vector<uint8_t>& src, int32_t stride,
                                          int32_t x, int32_t y, int32_t w, int32_t h) const;
    void writeMaskRegion(const MaskEdit& edit);
    void pushMaskUndo(MaskEdit&& edit);
    void pushMaskRedo(MaskEdit&& edit);
    void clearMaskRedo();

    std::vector<uint8_t> mask_;
    int32_t mask_w_ = 0;
    int32_t mask_h_ = 0;
    std::vector<MaskEdit> mask_undo_;
    std::vector<MaskEdit> mask_redo_;
    std::vector<uint8_t> stroke_before_;
    bool stroke_active_ = false;
    int32_t stroke_min_x_ = 0;
    int32_t stroke_min_y_ = 0;
    int32_t stroke_max_x_ = -1;
    int32_t stroke_max_y_ = -1;
    float stroke_last_x_ = 0.0f;
    float stroke_last_y_ = 0.0f;
    uint64_t view_gen_ = 1;
    uint64_t display_gen_ = 0;
    ff::Image orig_cache_;
    int32_t orig_cache_w_ = 0;
    int32_t orig_cache_h_ = 0;
    uint64_t orig_cache_gen_ = ~0ull;
    std::vector<uint8_t> mask_preview_;
    int32_t mask_preview_w_ = 0;
    int32_t mask_preview_h_ = 0;
    uint32_t mask_preview_gen_ = ~0u;

    bool needsFullRes() const;
    void beginFullResExport(ExportKind kind, const std::string& path);
    void finishExport();

    void replaceBase(ff::Image&& img);
    void saveResultNow(const std::string& path);
    void copyNow();
    void defaultSavePathFor(const std::string& input);
    bool hasPendingIdentities();

    bool jobFail(const std::string& message);

    // Handoff fields shared with the worker.
    std::thread thread_;
    std::atomic<bool> working_{false};
    std::atomic<bool> job_done_{false};
    std::atomic<bool> job_ok_{false};
    std::atomic<uint32_t> job_kind_{0};
    std::atomic<uint32_t> stage_value_{0};
    std::atomic<uint32_t> progress_value_{0};

    JobKind job_kind_plain_ = JobKind::None; // written before the thread starts
    int job_arg_ = -1;
    std::string job_error_;
    std::string job_model_dir_;
    int job_provider_ = 0;
    int job_enhancer_ = 0;
    bool job_restore_mouth_ = false;
    bool job_fast_preview_ = false;
    bool job_unload_ = false;

    JobKind queued_job_ = JobKind::None;
    int queued_arg_ = -1;

    // Result handoff from the worker; taken on the UI thread in poll().
    std::vector<uint8_t> pending_result_;
    int32_t pending_w_ = 0;
    int32_t pending_h_ = 0;

    // Pending identity images queued from the UI thread.
    std::vector<PendingIdentity> pending_identities_;
    std::mutex pending_mu_;

    // Guards faces/patches/flags against the UI's snapshot/toggles.
    mutable std::mutex mu_;

    // Guards the identity library against the UI's label snapshot while the
    // worker appends to it.
    mutable std::mutex ident_mu_;

    // Set while a video workspace job owns the engine.
    std::atomic<bool> video_busy_{false};
    std::string video_engine_error_;

    ExportKind pending_export_ = ExportKind::None;
    std::string export_path_;
};

} // namespace app
