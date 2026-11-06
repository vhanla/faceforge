#include "app.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace app {

namespace {

std::string baseName(const std::string& path) {
    const size_t idx = path.find_last_of("/\\");
    return idx == std::string::npos ? path : path.substr(idx + 1);
}

double nsToMs(int64_t ns) { return static_cast<double>(ns) / 1e6; }

std::string stemOf(const std::string& name) {
    const size_t dot = name.find_last_of('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

// Crops a face bbox (with margin) out of `img` and resizes it to a thumbnail.
ff::Image makeThumb(const ff::Image& img, const float bbox[4]) {
    if (!img.valid()) return {};
    const float w = std::max(1.0f, bbox[2] - bbox[0]);
    const float h = std::max(1.0f, bbox[3] - bbox[1]);
    const float cx = (bbox[0] + bbox[2]) * 0.5f;
    const float cy = (bbox[1] + bbox[3]) * 0.5f;
    const float side = std::max(w, h) * 1.25f;
    const float W = static_cast<float>(img.width);
    const float H = static_cast<float>(img.height);
    const float x0f = std::clamp(cx - side * 0.5f, 0.0f, W - 1.0f);
    const float y0f = std::clamp(cy - side * 0.5f, 0.0f, H - 1.0f);
    const float x1f = std::clamp(x0f + side, 1.0f, W);
    const float y1f = std::clamp(y0f + side, 1.0f, H);
    const int32_t rw = static_cast<int32_t>(std::max(1.0f, x1f - x0f));
    const int32_t rh = static_cast<int32_t>(std::max(1.0f, y1f - y0f));
    const int32_t rx = static_cast<int32_t>(x0f);
    const int32_t ry = static_cast<int32_t>(y0f);

    const uint8_t* src = img.data + static_cast<size_t>(ry) * img.stride +
                         static_cast<size_t>(rx) * 3;
    uint8_t* out = nullptr;
    int32_t out_stride = 0;
    if (ff_image_resize(src, rw, rh, img.stride, kThumbSize, kThumbSize, &out,
                        &out_stride) != FF_OK ||
        !out) {
        return {};
    }
    return ff::Image::fromOwned(out, kThumbSize, kThumbSize);
}

void progressCb(int32_t stage_raw, float progress, const char* /*message*/, void* user) {
    auto* a = static_cast<App*>(user);
    if (!a) return;
    const int p = static_cast<int>(std::clamp(progress, 0.0f, 1.0f) * 100.0f);
    switch (stage_raw) {
        case 0:
            a->setStage(Stage::Detecting, 400 + p * 2);
            break;
        case 2:
            a->setStage(Stage::Swapping, 650 + p);
            break;
        case 3:
            a->setStage(Stage::Enhancing, 900 + p);
            break;
        default:
            break;
    }
}

} // namespace

const char* stageText(Stage s) {
    switch (s) {
        case Stage::Idle: return "idle";
        case Stage::Loading: return "loading models";
        case Stage::Detecting: return "detecting faces";
        case Stage::Swapping: return "swapping";
        case Stage::Enhancing: return "enhancing";
        case Stage::Compositing: return "compositing";
        case Stage::Done: return "ready";
    }
    return "idle";
}

App::App() {
    for (int i = 0; i < kMaxFaces; ++i) {
        face_sel[i] = false;
        face_identity[i] = -1;
        face_mouth_set[i] = false;
        face_mouth[i] = false;
    }
}

App::~App() {
    if (thread_.joinable()) thread_.join();
}

// ---- settings --------------------------------------------------------------

void App::loadConfig(const std::string& path) {
    config_path = path;
    config_enabled = !path.empty();
    if (!config_enabled) return;
    config::Values v;
    if (config::load(path, v)) settings = v;
    provider = settings.provider;
    if (!settings.models_dir.empty()) model_dir = settings.models_dir;
    temp_dir = settings.temp_dir;
    export_dir = settings.export_dir;
    unload_after_run = settings.unload_after_run;
    check_updates = settings.check_updates;
    update_interval_hours = settings.update_interval_hours;
    last_update_check = settings.last_update_check;
}

void App::saveConfig() {
    if (!config_enabled) return;
    settings.provider = provider;
    settings.models_dir = model_dir;
    settings.temp_dir = temp_dir;
    settings.export_dir = export_dir;
    settings.unload_after_run = unload_after_run;
    settings.check_updates = check_updates;
    settings.update_interval_hours = update_interval_hours;
    settings.last_update_check = last_update_check;
    if (config::save(config_path, settings)) config_dirty = false;
}

void App::markConfigDirty() { config_dirty = true; }

// ---- status ----------------------------------------------------------------

void App::setStatus(const std::string& text, bool is_error) {
    status_text = text;
    status_is_error = is_error;
}

void App::setStage(Stage st, int permille) {
    stage_value_.store(static_cast<uint32_t>(st), std::memory_order_relaxed);
    setProgress(permille);
}

void App::setProgress(int permille) {
    progress_value_.store(static_cast<uint32_t>(std::clamp(permille, 0, 1000)),
                          std::memory_order_relaxed);
}

bool App::jobFail(const std::string& message) {
    job_error_ = message;
    return false;
}

// ---- public operations -----------------------------------------------------

void App::openTarget(const std::string& path) {
    if (busy()) return;
    std::string err;
    ff::Image img = ff::Image::load(path.c_str(), &err);
    if (!img.valid()) {
        setStatus("Cannot load target image", true);
        return;
    }
    const int w = img.width;
    const int h = img.height;
    target_path = path;
    replaceBase(std::move(img));
    defaultSavePathFor(path);
    char buf[512];
    std::snprintf(buf, sizeof(buf), "Loaded target - %dx%d - %s", w, h,
                  baseName(path).c_str());
    setStatus(buf, false);
}

void App::addIdentityFile(const std::string& path) {
    std::string err;
    ff::Image img = ff::Image::load(path.c_str(), &err);
    if (!img.valid()) {
        setStatus("Cannot load identity image", true);
        return;
    }
    const std::string name = baseName(path);
    addIdentityImage(std::move(img), name);
    setStatus("Adding identity - " + name, false);
}

void App::addIdentityImage(ff::Image&& img, const std::string& label) {
    {
        std::lock_guard<std::mutex> lk(pending_mu_);
        PendingIdentity p;
        p.img = std::move(img);
        p.label = label;
        pending_identities_.push_back(std::move(p));
    }
    if (!busy()) spawnJob(JobKind::AddIdentity, -1);
}

void App::removeIdentity(int index) {
    if (busy()) return;
    if (index < 0 || index >= static_cast<int>(identities.size())) return;
    {
        std::lock_guard<std::mutex> lk(ident_mu_);
        identities.erase(identities.begin() + index);
    }
    if (default_identity >= static_cast<int>(identities.size())) default_identity = 0;
    identity_gen++;
    spawnJob(JobKind::ProcessAll, -1);
}

void App::clearIdentities() {
    if (busy()) return;
    {
        std::lock_guard<std::mutex> lk(ident_mu_);
        identities.clear();
    }
    default_identity = 0;
    identity_gen++;
    clearPatches();
    clearResult();
    setStatus("Identities cleared", false);
}

void App::selectDefaultIdentity(int index) {
    if (identities.empty()) return;
    default_identity = std::clamp(index, 0, static_cast<int>(identities.size()) - 1);
    spawnJob(JobKind::ProcessAll, -1);
}

void App::processAll() { spawnJob(JobKind::ProcessAll, -1); }
void App::processOne(int index) { spawnJob(JobKind::ProcessOne, index); }
void App::recompose() { spawnJob(JobKind::Compose, -1); }

void App::setFaceSelected(int index, bool on) {
    if (index < 0 || index >= kMaxFaces) return;
    {
        std::lock_guard<std::mutex> lk(mu_);
        face_sel[index] = on;
    }
    if (busy() || (on && !patchFresh(index))) {
        spawnJob(JobKind::ProcessAll, -1);
    } else {
        spawnJob(JobKind::Compose, -1);
    }
}

void App::toggleFace(int index) {
    if (index < 0 || index >= kMaxFaces) return;
    bool cur;
    {
        std::lock_guard<std::mutex> lk(mu_);
        cur = face_sel[index];
    }
    setFaceSelected(index, !cur);
}

void App::setFaceIdentity(int index, int identity) {
    if (index < 0 || index >= kMaxFaces) return;
    {
        std::lock_guard<std::mutex> lk(mu_);
        face_identity[index] = identity;
    }
    spawnJob(JobKind::ProcessOne, index);
}

void App::setFaceMouth(int index, int mode) {
    if (index < 0 || index >= kMaxFaces) return;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (mode < 0) {
            face_mouth_set[index] = false;
        } else {
            face_mouth_set[index] = true;
            face_mouth[index] = mode != 0;
        }
    }
    spawnJob(JobKind::ProcessOne, index);
}

void App::saveResult(const std::string& path) {
    if (busy()) {
        setStatus("Busy - try again in a moment", true);
        return;
    }
    if (path.empty()) {
        setStatus("Choose an output path first", true);
        return;
    }
    if (needsFullRes()) {
        export_path_ = path;
        beginFullResExport(ExportKind::Save, path);
        return;
    }
    saveResultNow(path);
}

void App::copyResult() {
    if (busy()) {
        setStatus("Busy - try again in a moment", true);
        return;
    }
    if (!result.valid()) {
        setStatus("Process an image first", true);
        return;
    }
    if (needsFullRes()) {
        beginFullResExport(ExportKind::Copy, "");
        return;
    }
    copyNow();
}

void App::applyResultAsInput() {
    if (busy()) return;
    if (needsFullRes()) {
        export_path_.clear();
        beginFullResExport(ExportKind::ApplyInput, "");
        return;
    }
    if (!result.valid()) {
        setStatus("Process an image first", true);
        return;
    }
    if (base.valid()) {
        ff::Image snap = base.clone();
        if (snap.valid()) pushUndo(std::move(snap));
    }
    clearRedo();
    ff::Image next = result.clone();
    if (!next.valid()) {
        setStatus("Out of memory", true);
        return;
    }
    base = std::move(next);
    base_dirty = true;
    base_gen++;
    clearResult();
    clearPatches();
    freeProc();
    face_count = 0;
    if (!busy()) spawnJob(JobKind::ReloadTarget, -1);
    setStatus("Result applied as input", false);
}

void App::clearTarget() {
    if (busy()) return;
    base.reset();
    base_gen++;
    base_dirty = true;
    clearResult();
    clearPatches();
    freeProc();
    face_count = 0;
    undo.clear();
    redo.clear();
    target_path.clear();
    setStatus("Canvas cleared", false);
}

void App::undoHistory() {
    if (busy() || undo.empty()) {
        if (undo.empty()) setStatus("Nothing to undo", true);
        return;
    }
    ff::Image snap = std::move(undo.back());
    undo.pop_back();
    if (base.valid()) {
        ff::Image cur = base.clone();
        if (cur.valid()) {
            if (redo.size() >= static_cast<size_t>(kHistoryCap)) redo.erase(redo.begin());
            redo.push_back(std::move(cur));
        }
    }
    base = std::move(snap);
    base_dirty = true;
    base_gen++;
    clearResult();
    clearPatches();
    freeProc();
    face_count = 0;
    spawnJob(JobKind::ReloadTarget, -1);
    setStatus("Undo", false);
}

void App::redoHistory() {
    if (busy() || redo.empty()) {
        if (redo.empty()) setStatus("Nothing to redo", true);
        return;
    }
    ff::Image snap = std::move(redo.back());
    redo.pop_back();
    if (base.valid()) {
        ff::Image cur = base.clone();
        if (cur.valid()) {
            if (undo.size() >= static_cast<size_t>(kHistoryCap)) undo.erase(undo.begin());
            undo.push_back(std::move(cur));
        }
    }
    base = std::move(snap);
    base_dirty = true;
    base_gen++;
    clearResult();
    clearPatches();
    freeProc();
    face_count = 0;
    spawnJob(JobKind::ReloadTarget, -1);
    setStatus("Redo", false);
}

// ---- helpers ---------------------------------------------------------------

void App::replaceBase(ff::Image&& img) {
    base = std::move(img);
    base_dirty = true;
    base_gen++;
    clearResult();
    clearPatches();
    freeProc();
    face_count = 0;
    undo.clear();
    redo.clear();
    spawnJob(JobKind::ReloadTarget, -1);
}

bool App::needsFullRes() const {
    if (!fast_preview) return false;
    if (!base.valid()) return false;
    if (proc_w <= 0 || proc_h <= 0) return true;
    return std::max(base.width, base.height) > std::max(proc_w, proc_h);
}

void App::beginFullResExport(ExportKind kind, const std::string& path) {
    if (busy()) {
        setStatus("Busy - try again in a moment", true);
        return;
    }
    if (!needsFullRes()) {
        // Nothing to render; run the export immediately.
        pending_export_ = kind;
        export_path_ = path;
        finishExport();
        return;
    }
    export_path_ = path;
    pending_export_ = kind;
    setStatus("Rendering full resolution for export...", false);
    spawnJob(JobKind::ProcessAll, -1);
}

void App::finishExport() {
    const ExportKind kind = pending_export_;
    pending_export_ = ExportKind::None;
    switch (kind) {
        case ExportKind::Save:
            if (!export_path_.empty()) {
                saveResultNow(export_path_);
                export_path_.clear();
            }
            break;
        case ExportKind::Copy:
            copyNow();
            break;
        case ExportKind::ApplyInput:
            applyResultAsInput();
            break;
        case ExportKind::None:
            break;
    }
}

void App::saveResultNow(const std::string& path) {
    if (!result.valid()) {
        setStatus("Nothing to save", true);
        return;
    }
    if (result.save(path.c_str())) {
        save_path = path;
        setStatus("Saved - " + path, false);
    } else {
        setStatus("Could not save - " + path, true);
    }
}

void App::copyNow() {
    if (!result.valid()) {
        setStatus("Nothing to copy", true);
        return;
    }
    clipboard_png.clear();
    if (result.encodePng(clipboard_png)) {
        setStatus("Copied to clipboard", false);
    } else {
        clipboard_png.clear();
        setStatus("Could not encode image", true);
    }
}

void App::defaultSavePathFor(const std::string& input) {
    if (save_explicit) return;
    const std::string name = baseName(input);
    std::string stem = stemOf(name);
    if (stem.empty()) stem = "output";
    const std::string dir = !export_dir.empty() ? export_dir : config::dirName(input);
    const std::string file = stem + "_faceforge.png";
    save_path = dir.empty() ? file : config::join(dir, file);
}

bool App::hasPendingIdentities() {
    std::lock_guard<std::mutex> lk(pending_mu_);
    return !pending_identities_.empty();
}

bool App::hasIdentities() const { return !identities.empty(); }

FacesSnapshot App::facesSnapshot() const {
    FacesSnapshot s;
    std::lock_guard<std::mutex> lk(mu_);
    const int n = std::min(face_count, kMaxFaces);
    s.count = n;
    for (int i = 0; i < n; ++i) {
        s.faces[i] = faces[i];
        s.sel[i] = face_sel[i];
        s.identity[i] = face_identity[i];
        s.mouth_set[i] = face_mouth_set[i];
        s.mouth[i] = face_mouth[i];
        s.patch_valid[i] = patches[proc_level][i].valid;
    }
    return s;
}

IdentitiesSnapshot App::identitiesSnapshot() const {
    IdentitiesSnapshot s;
    std::lock_guard<std::mutex> lk(ident_mu_);
    const int n = std::min(static_cast<int>(identities.size()), kMaxIdentities);
    s.count = n;
    s.default_identity = default_identity;
    s.gen = identity_gen;
    for (int i = 0; i < n; ++i) s.labels[i] = identities[static_cast<size_t>(i)].label;
    return s;
}

// ---- worker ----------------------------------------------------------------

void App::spawnJob(JobKind kind, int arg) {
    if (busy()) {
        switch (kind) {
            case JobKind::None:
                break;
            case JobKind::Init:
            case JobKind::ReloadTarget:
            case JobKind::AddIdentity:
                if (queued_job_ == JobKind::None) {
                    queued_job_ = kind;
                    queued_arg_ = arg;
                }
                break;
            case JobKind::ProcessAll:
            case JobKind::ProcessOne:
            case JobKind::Compose:
                if (queued_job_ != JobKind::Init && queued_job_ != JobKind::ReloadTarget &&
                    queued_job_ != JobKind::AddIdentity) {
                    queued_job_ = JobKind::ProcessAll;
                    queued_arg_ = -1;
                }
                break;
        }
        return;
    }
    if (thread_.joinable()) thread_.join();

    job_model_dir_ = model_dir;
    job_arg_ = arg;
    job_provider_ = static_cast<int>(provider);
    job_enhancer_ = static_cast<int>(enhancer);
    job_restore_mouth_ = restore_mouth;
    job_fast_preview_ = fast_preview && pending_export_ == ExportKind::None;
    job_unload_ = unload_after_run;

    job_error_.clear();
    job_done_.store(false, std::memory_order_release);
    job_ok_.store(false, std::memory_order_release);
    job_kind_plain_ = kind;
    job_kind_.store(static_cast<uint32_t>(kind), std::memory_order_relaxed);
    progress_value_.store(0, std::memory_order_relaxed);
    stage_value_.store(static_cast<uint32_t>(Stage::Loading), std::memory_order_relaxed);
    working_.store(true, std::memory_order_release);

    thread_ = std::thread(&App::workerMain, this, kind);
    setStatus("Working...", false);
}

void App::workerMain(JobKind kind) {
    const bool ok = runJob(kind);
    job_ok_.store(ok, std::memory_order_release);
    job_done_.store(true, std::memory_order_release);
}

bool App::runJob(JobKind kind) {
    const int64_t t0 = ff::nowNs();
    timings = Timings{};

    bool added = false;
    if (!drainPendingIdentities(&added)) return false;

    switch (kind) {
        case JobKind::Init:
        case JobKind::ReloadTarget:
            if (!ensureEngine()) return false;
            if (!prepareProc()) return false;
            if (!detectTarget()) return false;
            if (!swapAll()) return false;
            if (!composeResult()) return false;
            break;
        case JobKind::AddIdentity:
            if (!ensureEngine()) return false;
            if (!prepareProc()) return false;
            if (face_count == 0 && !detectTarget()) return false;
            if (!swapAll()) return false;
            if (!composeResult()) return false;
            break;
        case JobKind::ProcessAll:
            if (!ensureEngine()) return false;
            if (!prepareProc()) return false;
            if (face_count == 0 && !detectTarget()) return false;
            if (!swapAll()) return false;
            if (!composeResult()) return false;
            break;
        case JobKind::ProcessOne:
            if (!ensureEngine()) return false;
            if (!prepareProc()) return false;
            if (face_count == 0 && !detectTarget()) return false;
            if (job_arg_ >= 0 && job_arg_ < face_count) {
                const int64_t ts = ff::nowNs();
                if (!swapOneFace(job_arg_)) return false;
                timings.swap_ms = nsToMs(ff::nowNs() - ts);
            }
            if (!composeResult()) return false;
            break;
        case JobKind::Compose:
            if (!ensureEngine()) return false;
            if (added) {
                if (!prepareProc()) return false;
                if (face_count == 0 && !detectTarget()) return false;
                if (!swapAll()) return false;
            }
            if (!composeResult()) return false;
            break;
        case JobKind::None:
            break;
    }

    timings.total_ms = nsToMs(ff::nowNs() - t0);
    stage_value_.store(static_cast<uint32_t>(Stage::Done), std::memory_order_relaxed);
    progress_value_.store(1000, std::memory_order_relaxed);
    return true;
}

void App::poll() {
    if (!working_.load(std::memory_order_acquire)) return;
    if (!job_done_.load(std::memory_order_acquire)) return;

    const bool ok = job_ok_.load(std::memory_order_acquire);
    const JobKind kind = job_kind_plain_;

    if (ok) {
        takePendingResult();
        buildFaceThumbs();
        timings_published = timings;
        if (!provider_name.empty()) provider_published = provider_name;
        stage_value_.store(static_cast<uint32_t>(Stage::Done), std::memory_order_relaxed);
        progress_value_.store(1000, std::memory_order_relaxed);
        setStatus("Ready", false);
    } else {
        setStatus(job_error_.empty() ? "Processing failed" : job_error_, true);
        pending_export_ = ExportKind::None;
        export_path_.clear();
    }

    if (ok && job_unload_) engine.unload();

    working_.store(false, std::memory_order_release);
    if (thread_.joinable()) thread_.join();

    if (ok) finishExport();

    // Pick up coalesced / dropped work now that the worker is free.
    if (ok && hasPendingIdentities()) {
        queued_job_ = JobKind::None;
        spawnJob(JobKind::AddIdentity, -1);
    } else if (queued_job_ != JobKind::None) {
        const JobKind qk = queued_job_;
        const int qa = queued_arg_;
        queued_job_ = JobKind::None;
        queued_arg_ = -1;
        spawnJob(qk, qa);
    }
    (void)kind;
}

bool App::ensureEngine() {
    if (engine.ptr) {
        if (engine.sequential == job_unload_) {
            engine.setRestoreMouth(job_restore_mouth_);
            return true;
        }
        engine.destroy();
    }
    setStage(Stage::Loading, 50);
    if (job_model_dir_.empty()) return jobFail("Model directory is empty");
    std::string err;
    if (!engine.create(job_model_dir_.c_str(), job_provider_, job_unload_, &err)) {
        return jobFail(err);
    }
    provider_name = engine.provider();
    engine.setRestoreMouth(job_restore_mouth_);
    setProgress(200);
    return true;
}

bool App::prepareProc() {
    if (!base.valid()) return jobFail("Load a target image first");
    freeProc();
    const bool use_preview =
        job_fast_preview_ && std::max(base.width, base.height) > kPreviewLongSide;
    if (use_preview) {
        const float longest = static_cast<float>(std::max(base.width, base.height));
        const float f = kPreviewLongSide / longest;
        const int32_t nw =
            std::max(1, static_cast<int32_t>(static_cast<float>(base.width) * f));
        const int32_t nh =
            std::max(1, static_cast<int32_t>(static_cast<float>(base.height) * f));
        std::string err;
        ff::Image im = base.resized(nw, nh, &err);
        if (!im.valid()) return jobFail("Cannot downscale target");
        proc = std::move(im);
        proc_level = kPreviewLevel;
    } else {
        proc = ff::Image::borrow(base.data, base.width, base.height, base.stride);
        proc_level = kFullLevel;
    }
    proc_w = proc.width;
    proc_h = proc.height;
    return true;
}

bool App::drainPendingIdentities(bool* added) {
    *added = false;
    if (!hasPendingIdentities()) return true;
    if (!ensureEngine()) return false;
    setStage(Stage::Detecting, 300);
    const int64_t t0 = ff::nowNs();
    while (true) {
        PendingIdentity p;
        {
            std::lock_guard<std::mutex> lk(pending_mu_);
            if (pending_identities_.empty()) break;
            p = std::move(pending_identities_.front());
            pending_identities_.erase(pending_identities_.begin());
        }
        if (static_cast<int>(identities.size()) >= kMaxIdentities) continue;
        ff_face faces[kMaxFaces];
        int n = 0;
        if (!engine.detect(p.img, faces, kMaxFaces, &n)) continue;
        for (int k = 0; k < n && static_cast<int>(identities.size()) < kMaxIdentities; ++k) {
            float emb[512] = {};
            if (!engine.embed(p.img, faces[k], emb)) continue;
            Identity id;
            std::memcpy(id.embedding, emb, sizeof(emb));
            id.thumb.img = makeThumb(p.img, faces[k].bbox);
            id.thumb.dirty = true;
            char buf[96];
            std::snprintf(buf, sizeof(buf), "%s #%d", p.label.c_str(), k);
            id.label = buf;
            {
                std::lock_guard<std::mutex> lk(ident_mu_);
                identities.push_back(std::move(id));
            }
            *added = true;
        }
    }
    if (*added) {
        identity_gen++;
        if (default_identity >= static_cast<int>(identities.size())) default_identity = 0;
    }
    timings.detect_ms = nsToMs(ff::nowNs() - t0);
    setProgress(400);
    return true;
}

int App::effectiveIdentity(int index) const {
    if (identities.empty()) return 0;
    const int v = face_identity[index];
    if (v >= 0 && v < static_cast<int>(identities.size())) return v;
    return std::min(default_identity, static_cast<int>(identities.size()) - 1);
}

bool App::effectiveMouth(int index) const {
    if (face_mouth_set[index]) return face_mouth[index];
    return restore_mouth;
}

bool App::patchFresh(int index) const {
    std::lock_guard<std::mutex> lk(mu_);
    const Patch& p = patches[proc_level][index];
    if (!p.valid) return false;
    if (p.proc_w != proc_w || p.proc_h != proc_h) return false;
    if (p.id_gen != identity_gen) return false;
    if (p.identity != effectiveIdentity(index)) return false;
    if (p.mouth != effectiveMouth(index)) return false;
    return true;
}

ff_face App::scaledFace(int index) const {
    ff_face f = faces[index];
    if (proc_w != base.width || proc_h != base.height) {
        const float fx = static_cast<float>(proc_w) / static_cast<float>(base.width);
        const float fy = static_cast<float>(proc_h) / static_cast<float>(base.height);
        f.bbox[0] *= fx;
        f.bbox[1] *= fy;
        f.bbox[2] *= fx;
        f.bbox[3] *= fy;
        for (int k = 0; k < 5; ++k) {
            f.kps[k * 2] *= fx;
            f.kps[k * 2 + 1] *= fy;
        }
    }
    return f;
}

bool App::detectTarget() {
    if (!base.valid()) return jobFail("Load a target image first");
    setStage(Stage::Detecting, 500);
    const int64_t t0 = ff::nowNs();
    ff_face faces[kMaxFaces];
    int n = 0;
    if (!engine.detect(base, faces, kMaxFaces, &n)) return jobFail(engine.lastError());
    {
        std::lock_guard<std::mutex> lk(mu_);
        std::memcpy(this->faces, faces, sizeof(ff_face) * static_cast<size_t>(n));
        face_count = n;
        for (int i = 0; i < kMaxFaces; ++i) {
            face_sel[i] = i < n;
            face_identity[i] = -1;
            face_mouth_set[i] = false;
            face_mouth[i] = false;
        }
    }
    clearPatches();
    timings.detect_ms = nsToMs(ff::nowNs() - t0);
    if (n == 0) setProgress(600);
    return true;
}

bool App::swapAll() {
    if (identities.empty() || face_count == 0) return true;
    std::vector<char> sel(face_count);
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (int i = 0; i < face_count; ++i) sel[i] = face_sel[i];
    }
    int total = 0;
    for (int i = 0; i < face_count; ++i) {
        if (sel[i] && !patchFresh(i)) total++;
    }
    if (total == 0) return true;

    setStage(Stage::Swapping, 650);
    const int64_t t0 = ff::nowNs();
    int done = 0;
    for (int i = 0; i < face_count; ++i) {
        if (!sel[i] || patchFresh(i)) continue;
        if (!swapOneFace(i)) return false;
        done++;
        setProgress(650 + static_cast<int>(static_cast<float>(done) /
                                            static_cast<float>(total) * 180.0f));
    }
    timings.swap_ms = nsToMs(ff::nowNs() - t0);
    return true;
}

bool App::swapOneFace(int index) {
    if (identities.empty()) return jobFail("Add an identity image first");
    const ff_face face = scaledFace(index);
    const int id = effectiveIdentity(index);
    const bool mouth = effectiveMouth(index);
    engine.setRestoreMouth(mouth);
    double matrix[6] = {0, 0, 0, 0, 0, 0};
    bool ok = false;
    ff::Image patch =
        engine.swapPatch(proc, identities[id].embedding, face, matrix, &ok);
    if (!ok) return jobFail(engine.lastError());

    Patch& slot = patches[proc_level][index];
    {
        std::lock_guard<std::mutex> lk(mu_);
        slot.img = std::move(patch);
        std::memcpy(slot.matrix, matrix, sizeof(matrix));
        slot.valid = true;
        slot.identity = id;
        slot.mouth = mouth;
        slot.id_gen = identity_gen;
        slot.proc_w = proc_w;
        slot.proc_h = proc_h;
    }
    return true;
}

uint64_t App::composeSignature() const {
    uint64_t h = 1469598103934665603ULL; // FNV-1a offset
    auto mix = [&h](const void* data, size_t n) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < n; ++i) {
            h ^= p[i];
            h *= 1099511628211ULL;
        }
    };
    mix(&base_gen, sizeof(base_gen));
    mix(&proc_w, sizeof(proc_w));
    mix(&proc_h, sizeof(proc_h));
    const int enh = job_enhancer_;
    mix(&enh, sizeof(enh));
    std::lock_guard<std::mutex> lk(mu_);
    for (int i = 0; i < face_count; ++i) {
        const bool active = face_sel[i] && patches[proc_level][i].valid;
        mix(&active, sizeof(active));
        if (active) {
            const int id = effectiveIdentity(i);
            mix(&id, sizeof(id));
            const bool m = effectiveMouth(i);
            mix(&m, sizeof(m));
        }
    }
    return h;
}

bool App::composeResult() {
    const int32_t w = proc_w;
    const int32_t h = proc_h;
    if (w <= 0 || h <= 0) return jobFail("No target image to compose");

    const uint64_t sig = composeSignature();
    if (result_level == proc_w && result_sig == sig) {
        setProgress(1000);
        return true;
    }

    const size_t bytes = static_cast<size_t>(w) * static_cast<size_t>(h) * 3;
    uint8_t* ping = static_cast<uint8_t*>(std::malloc(bytes));
    uint8_t* pong = static_cast<uint8_t*>(std::malloc(bytes));
    if (!ping || !pong) {
        std::free(ping);
        std::free(pong);
        return jobFail("Out of memory");
    }
    std::memcpy(ping, proc.data, bytes);

    ff::Image cur = ff::Image::fromOwned(ping, w, h);
    ff::Image next = ff::Image::fromOwned(pong, w, h);

    setStage(Stage::Compositing, 850);

    std::vector<int> active;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (int i = 0; i < face_count; ++i) {
            if (face_sel[i] && patches[proc_level][i].valid) active.push_back(i);
        }
    }

    for (int i : active) {
        const Patch& p = patches[proc_level][i];
        if (!ff::pastePatch(cur, p.img, p.matrix, &next)) {
            return jobFail("Patch paste failed");
        }
        std::swap(cur, next);
    }

    std::vector<ff_face> active_faces;
    active_faces.reserve(active.size());
    for (int i : active) active_faces.push_back(scaledFace(i));

    if (job_enhancer_ != 0 && !active_faces.empty()) {
        engine.setEnhancer(job_enhancer_);
        setStage(Stage::Enhancing, 900);
        const int64_t t0 = ff::nowNs();
        if (!engine.enhance(cur, active_faces.data(),
                            static_cast<int>(active_faces.size()), &next, progressCb, this)) {
            return jobFail(engine.lastError());
        }
        std::swap(cur, next);
        timings.enhance_ms = nsToMs(ff::nowNs() - t0);
    } else {
        engine.setEnhancer(0);
    }

    pending_result_.assign(cur.data, cur.data + bytes);
    pending_w_ = w;
    pending_h_ = h;
    result_sig = sig;
    result_level = proc_w;
    setProgress(1000);
    return true;
}

void App::takePendingResult() {
    if (pending_result_.empty()) return;
    uint8_t* buf = static_cast<uint8_t*>(std::malloc(pending_result_.size()));
    if (!buf) {
        pending_result_.clear();
        return;
    }
    std::memcpy(buf, pending_result_.data(), pending_result_.size());
    result = ff::Image::fromOwned(buf, pending_w_, pending_h_);
    pending_result_.clear();
    result_dirty = true;
}

void App::buildFaceThumbs() {
    if (!base.valid()) {
        for (int i = 0; i < kMaxFaces; ++i) {
            face_thumb[i].img.reset();
            face_thumb[i].dirty = false;
        }
        return;
    }
    std::lock_guard<std::mutex> lk(mu_);
    for (int i = 0; i < kMaxFaces; ++i) {
        if (i >= face_count) {
            face_thumb[i].img.reset();
            continue;
        }
        face_thumb[i].img = makeThumb(base, faces[i].bbox);
        face_thumb[i].dirty = true;
    }
}

void App::clearResult() {
    result.reset();
    result_sig = 0;
    result_level = -1;
    result_dirty = true;
    timings = Timings{};
}

void App::clearPatches() {
    std::lock_guard<std::mutex> lk(mu_);
    for (int l = 0; l < kPatchLevels; ++l) {
        for (int i = 0; i < kMaxFaces; ++i) {
            patches[l][i].img.reset();
            patches[l][i].valid = false;
        }
    }
}

void App::freeProc() {
    proc.reset();
    proc_w = 0;
    proc_h = 0;
    proc_level = kFullLevel;
}

void App::pushUndo(ff::Image&& img) {
    if (undo.size() >= static_cast<size_t>(kHistoryCap)) undo.erase(undo.begin());
    undo.push_back(std::move(img));
}

void App::clearRedo() { redo.clear(); }

} // namespace app
