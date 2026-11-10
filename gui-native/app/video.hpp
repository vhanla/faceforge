#pragma once
// Video workspace: a purpose-built face-swap editor for clips, ported from
// gui/src/video.zig. It is deliberately independent from the still-image
// editor: it owns its timeline, project file, analysis data, frame caches and
// jobs, and borrows the engine, the identity library and the settings through
// app::App ("host").
//
// Threading contract mirrors the photo worker: all inference runs on one
// background thread, `poll()` drives it from the UI thread and joins it when
// the job finishes. The shared arrays (samples, chunks, assigns, overrides,
// segments, edits) are guarded by data_mu_. The UI reads them through locked
// accessors; status/progress live in atomics plus a job message written before
// the completion flag is set.
//
// A video job and a photo job never run at the same time: the video refuses to
// spawn while App::busy(), and App::spawnJob refuses while a video job is
// active.

#include "app.hpp"
#include "ff.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace app {

constexpr int kVideoMaxFaces = 8;
// Assignment sentinel for "leave this face untouched". Identity ids are >= 0
// and -1 means "use the default identity", so -2 is free for skip.
constexpr int32_t kVideoSkipId = -2;
constexpr size_t kMaxAssigns = 4096;
constexpr size_t kMaxOverrides = 4096;
constexpr size_t kMaxChunks = 20000;
constexpr size_t kMaxSegments = 240;
constexpr size_t kMaxEdits = 4096;
constexpr size_t kMaxStrokesPerEdit = 512;

enum class VideoJob { None, Analyze, Process, Encode };
enum class VideoQuality { Low, High };
enum class VideoTestState { Off, Start, WaitAnalyze, WaitProcess, WaitExport, WaitPlay, Done };

struct VidSample {
    int64_t f = 0;
    uint8_t n = 0;
    float b[kVideoMaxFaces][4] = {};
};

struct VidChunk {
    int64_t start = 0;
    int64_t end = 0;
    bool done = false;
};

// Identity assignment range. ids[i] == -1 uses the default identity and
// ids[i] == kVideoSkipId leaves face i exactly as it is in the source frame.
struct VidAssign {
    int64_t a = 0;
    int64_t b = 0;
    int32_t ids[kVideoMaxFaces];
    VidAssign() {
        for (int i = 0; i < kVideoMaxFaces; ++i) ids[i] = -1;
    }
};

struct VidOverride {
    int64_t f = 0;
    int32_t ids[kVideoMaxFaces];
    VidOverride() {
        for (int i = 0; i < kVideoMaxFaces; ++i) ids[i] = -1;
    }
};

// A contiguous run of frames with the same face count and stable positions.
struct VidSegment {
    int64_t start = 0;
    int64_t end = 0;
    uint8_t count = 0;
    float boxes[kVideoMaxFaces][4] = {};
};

struct VidStrokePt {
    float x = 0;
    float y = 0;
};

// A painted retouch is stored as geometry (source-frame pixels) so one edit can
// be re-applied at any resolution and transformed when copied onto a similar
// face in another frame.
struct VidStroke {
    std::vector<VidStrokePt> pts;
    float radius = 30;
    bool reveal = true;
};

struct VidFrameEdit {
    int64_t f = 0;
    bool bypassed = false;
    std::vector<VidStroke> strokes;
};

// Small face crop shown next to a sequence assignment.
struct VidFaceThumb {
    ff::Image img;
    bool dirty = false;
};

class Video {
public:
    explicit Video(App* host);
    ~Video();

    Video(const Video&) = delete;
    Video& operator=(const Video&) = delete;

    // ---- lifecycle ----
    void setAutotest(const std::string& path);
    bool testDone() const { return test_done_.load(std::memory_order_acquire); }
    void close();

    bool isOpen() const { return open_; }
    bool hasProject() const;
    bool active() const { return job != VideoJob::None; }

    const std::string& path() const { return path_; }
    const std::string& name() const { return name_; }
    const std::string& projectPath() const { return proj_path_; }
    const std::string& cacheRoot() const { return cache_root_; }
    const ff::Video::Info& info() const { return info_; }

    void openPath(const std::string& path);
    // Recompute the cache root after the Temp folder changes in Settings.
    void refreshPaths();

    // ---- per-frame UI tick (advance playback, join job, decode/ensure) ----
    void poll();

    // ---- frame navigation & playback ----
    void seekTo(int64_t frame);
    void stepBy(int64_t d);
    void startPlayback();
    void stopPlayback();
    void togglePlayback() { if (playing_) stopPlayback(); else startPlayback(); }
    bool playing() const { return playing_; }
    void selectSegmentAt(int64_t frame);
    void selectSegment(int index);

    // ---- analysis / processing / export jobs ----
    void startAnalyze();
    void startProcessFrame();
    void startProcessSequence();
    void startProcessChunk();
    void startProcessRange(int64_t from, int64_t to);
    // Job control. cancel() hard-stops the worker and also resumes a paused job.
    void cancel();
    void pause();
    void resume();
    void togglePause();
    bool paused() const { return paused_.load(std::memory_order_acquire); }
    // Blocks the worker while paused; returns immediately when resumed/cancelled.
    void waitWhilePaused();
    void clearAnalysis();
    // Starts an export to the current default output path.
    void startExport(bool whole);
    // Starts an export to an explicit path (from the save dialog).
    void startExportTo(const std::string& out, int64_t from, int64_t to, bool audio,
                       bool chunks);

    // ---- sequence assignments ----
    void setSegId(int k, int32_t id);
    void setAllSegDefault();
    void setAllSegSkip();
    void applyAssignment();
    void propagateForward();
    void applyToFrame();
    void clearFrameOverride();

    // ---- Studio retouch ----
    void studioBegin(float sx, float sy);
    void studioTo(float sx, float sy);
    void studioEnd();
    void undoStroke();
    void deleteCurrentEdit();
    void clearAllEdits();
    void setCurrentBypass(bool on);
    void toggleCurrentBypass();
    // Returns the number of frames a copy would overwrite (0 = no prompt).
    int countCopyConflicts();
    void copyToSimilar(bool overwrite);
    struct EditInfo {
        int strokes = 0;
        bool bypassed = false;
    };
    EditInfo currentEditInfo() const;

    // ---- cache / project ----
    uint64_t cacheSig() const;
    bool frameCachedLow(int64_t frame) const;
    bool frameCachedHigh(int64_t frame) const;
    void forgetFrameCache();
    void saveProject();

    // ---- locked UI accessors ----
    int segmentCount() const;
    VidSegment segment(int index) const;
    int sampleCount() const;
    void chunkStats(int* done, int* total) const;
    // Bounds of the chunk that contains `frame` inside [in_frame, out_frame].
    // `index` is 0-based and `count` is the number of chunks in the range.
    bool chunkInfo(int64_t frame, int64_t* start, int64_t* end, int* index,
                   int* count) const;
    // Moves the playhead to the start of the previous/next analyzed chunk.
    void stepChunk(int delta);
    void idsForFrame(int64_t frame, int count, int32_t out[kVideoMaxFaces]) const;
    int boxesForFrame(int64_t frame, float out[kVideoMaxFaces][4]) const;
    int editsInRange(int64_t a, int64_t b) const;
    bool selectedSegmentBounds(int64_t* start, int64_t* end, int* count) const;

    // ---- public state read by the UI ----
    int64_t in_frame = 0;
    int64_t out_frame = 0;
    int64_t cur = 0;
    float chunk_f = 300.0f;
    float stride_f = 1.0f;
    bool scrubbing = false;
    bool loop_play = true;
    bool playing_ = false; // kept private below; see playing()
    bool play_sequence = false;

    bool show_original = false;
    bool show_boxes = true;

    // Studio
    bool studio = false;
    bool studio_erase = true;
    float studio_size = 60.0f;
    float studio_hard = 0.6f;
    float studio_opac = 0.9f;
    bool studio_show_mask = true;
    bool studio_view_original = false;
    bool stroke_active = false;
    int copy_conflicts = 0;
    bool copy_pending = false;

    bool low_quality = true;
    bool export_audio = true;
    bool export_chunks = false;
    bool process_use_cache = true;
    bool default_skip = false;
    bool enhancer_failed = false;

    int sel_seg = -1;
    int32_t seg_ids[kVideoMaxFaces];

    // Face crops for the selected sequence; the entry point uploads dirty ones.
    VidFaceThumb face_thumbs[kVideoMaxFaces];

    // Current decoded frame / processed result; uploaded by the entry point.
    ff::Image cur_img;
    ff::Image res_img;
    bool cur_dirty = false;
    bool res_dirty = false;

    VideoJob job = VideoJob::None;
    uint32_t progress = 0;
    uint32_t job_done_frames = 0;
    uint32_t job_total_frames = 1;

    std::string status;
    bool status_err = false;
    std::string err;

    // Set by the panel Close button; acted on once any running job stops.
    bool close_requested = false;
    bool show_timeline = true;

    // Provider reported by the last finished video job (UI-thread read).
    std::string provider;

private:
    // ---- paths & project ----
    void setCacheRootFor(const std::string& video_path);
    void projectPathFor(const std::string& video_path);
    bool loadProject();
    static std::string baseName(const std::string& p);
    static std::string stripExt(const std::string& n);

    // ---- decode / preview (UI thread) ----
    void ensureFrame();
    void ensureResult();
    void rebuildEditedResult();
    void freeFaceThumbs();
    void ensureFaceThumbs();
    void advancePlayback();
    void rebuildSegments();

    // ---- job plumbing ----
    bool spawn(VideoJob kind);
    void jobMain(VideoJob kind);
    bool runAnalyze();
    bool runProcess();
    bool runExport();
    ff::Video openJobVideo();
    void ensureCacheDirs();
    void setJobError(const std::string& message);

    // ---- worker helpers (data_mu_ held where noted) ----
    // All of these assume data_mu_ is held by the caller.
    int frameEditIndexLocked(int64_t frame) const;
    int ensureEditLocked(int64_t frame);
    int segmentIndexAtLocked(int64_t frame) const;
    int boxesForFrameLocked(int64_t frame, float out[kVideoMaxFaces][4]) const;
    void idsForLocked(int64_t frame, int count, int32_t out[kVideoMaxFaces]) const;
    int referenceFaceIndexLocked(const VidFrameEdit& edit, const float boxes[kVideoMaxFaces][4],
                                 int count) const;
    std::vector<uint8_t> buildEditMaskLocked(const VidFrameEdit& edit, int32_t w, int32_t h,
                                             float sx, float sy) const;

    // ---- edit application (locks internally) ----
    bool hasActiveEdit(int64_t frame);
    void applyEditToImage(ff::Image& img, const ff::Image& orig, int32_t src_w, int32_t src_h,
                          int64_t frame);

    // ---- frame processing ----
    struct RenderResult {
        ff::Image img;
        float scale = 1.0f;
    };
    RenderResult renderFrame(ff::Engine& engine, const ff::Image& src, int64_t frame,
                             VideoQuality quality);
    bool processAndCache(ff::Engine& engine, ff::Video& video, int64_t frame, VideoQuality q,
                         uint64_t sig, bool use_cache);
    ff::Image renderExportFrame(ff::Engine& engine, ff::Video& video, int64_t frame, uint64_t sig);

    // ---- cache paths ----
    std::string cacheDirInto(VideoQuality q, uint64_t sig) const;
    std::string framePathInto(VideoQuality q, uint64_t sig, int64_t frame) const;
    ff::Image cachedFrame(int64_t frame, uint64_t sig);

    // ---- autotest ----
    void autotestStep();
    void testFinish();

    void mergeSamples(const std::vector<VidSample>& batch, int64_t from, int64_t to);
    bool chunkDone(int64_t start, int64_t end);
    void markChunk(int64_t start, int64_t end);
    void setStatus(const std::string& text, bool is_error);

    App* host_ = nullptr;

    std::string path_;
    std::string name_;
    std::string proj_path_;
    std::string cache_root_;
    bool open_ = false;
    ff::Video::Info info_;

    // UI-thread decoder and displayed images.
    ff::Video decoder_;
    int64_t cur_loaded_ = -1;
    ff::Image res_base_;
    int64_t res_frame_ = -1;
    uint64_t res_sig_ = ~0ull;
    uint64_t res_edit_gen_ = ~0ull;
    int64_t play_t0_ = 0;
    int64_t play_from_ = 0;

    // Painted stroke in progress (UI thread).
    float stroke_last_x = 0;
    float stroke_last_y = 0;

    // Guarded data.
    mutable std::mutex data_mu_;
    std::vector<VidSample> samples_;
    std::vector<VidChunk> chunks_;
    std::vector<VidAssign> assigns_;
    std::vector<VidOverride> overrides_;
    std::vector<VidSegment> segments_;
    std::vector<VidFrameEdit> edits_;
    uint64_t edit_gen_ = 1;
    int face_thumb_seg_ = -1;
    bool face_thumbs_stale_ = false;

    // Job handoff fields (written by the UI thread before the worker starts).
    std::thread thread_;
    std::atomic<bool> cancel_{false};
    std::atomic<bool> paused_{false};
    std::atomic<bool> done_{false};
    std::atomic<bool> ok_{false};
    std::atomic<uint32_t> progress_atomic_{0};
    std::atomic<uint32_t> job_done_atomic_{0};
    std::atomic<uint32_t> job_total_atomic_{1};
    std::string job_msg_;
    int64_t job_from_ = 0;
    int64_t job_to_ = 0;
    int64_t job_chunk_ = 300;
    int64_t job_stride_ = 1;
    VideoQuality job_quality_ = VideoQuality::Low;
    bool job_audio_ = false;
    bool job_chunks_ = false;
    bool job_use_cache_ = true;
    bool job_restore_mouth_ = true;
    int job_enhancer_ = 0;
    int job_grain_match_ = 0;
    std::string job_out_;

    // Autotest.
    VideoTestState test_state_ = VideoTestState::Off;
    std::string test_path_;
    bool test_keep_ = false;
    std::atomic<bool> test_done_{false};
    int test_play_moves_ = 0;
    int64_t test_play_last_ = -1;
    int test_play_budget_ = 0;
};

} // namespace app
