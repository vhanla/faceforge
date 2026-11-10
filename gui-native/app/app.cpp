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
    acrylic = settings.acrylic;
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
    settings.acrylic = acrylic;
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

void App::openTargetImage(ff::Image&& img, const std::string& label) {
    if (busy()) return;
    if (!img.valid()) {
        setStatus("Cannot load target image", true);
        return;
    }
    const int w = img.width;
    const int h = img.height;
    target_path.clear();
    replaceBase(std::move(img));
    defaultSavePathFor(label.empty() ? "pasted.png" : label);
    char buf[512];
    std::snprintf(buf, sizeof(buf), "Loaded target - %dx%d - %s", w, h,
                  label.empty() ? "pasted image" : label.c_str());
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
    const ff::Image* shown = displayImage();
    if (!shown || !shown->valid()) {
        setStatus("Process an image first", true);
        return;
    }
    if (base.valid()) {
        ff::Image snap = base.clone();
        if (snap.valid()) pushUndo(std::move(snap));
    }
    clearRedo();
    ff::Image next = shown->clone();
    if (!next.valid()) {
        setStatus("Out of memory", true);
        return;
    }
    base = std::move(next);
    base_dirty = true;
    base_gen++;
    clearResult();
    clearStudio();
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
    clearStudio();
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
    clearStudio();
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
    clearStudio();
    clearPatches();
    freeProc();
    face_count = 0;
    spawnJob(JobKind::ReloadTarget, -1);
    setStatus("Redo", false);
}

// ---- Studio retouch --------------------------------------------------------

void App::invalidateView() {
    view_gen_ += 1;
    result_dirty = true;
}

void App::clearStudio() {
    stroke_active_ = false;
    stroke_before_.clear();
    mask_.clear();
    mask_w_ = 0;
    mask_h_ = 0;
    mask_used = false;
    mask_undo_.clear();
    mask_redo_.clear();
    mask_undo_len = 0;
    mask_redo_len = 0;
    display.reset();
    orig_cache_.reset();
    orig_cache_w_ = 0;
    orig_cache_h_ = 0;
    orig_cache_gen_ = ~0ull;
    mask_preview_.clear();
    mask_preview_w_ = 0;
    mask_preview_h_ = 0;
    mask_preview_gen_ = ~0u;
    mask_gen += 1;
    invalidateView();
}

bool App::studioEnsureMask() {
    if (!base.valid() || base.width <= 0 || base.height <= 0) return false;
    if (!mask_.empty() && mask_w_ == base.width && mask_h_ == base.height) return true;

    mask_.clear();
    mask_w_ = 0;
    mask_h_ = 0;
    mask_undo_.clear();
    mask_redo_.clear();
    mask_undo_len = 0;
    mask_redo_len = 0;
    display.reset();
    mask_preview_.clear();
    mask_preview_w_ = 0;
    mask_preview_h_ = 0;
    mask_preview_gen_ = ~0u;

    try {
        mask_.assign(static_cast<size_t>(base.width) * static_cast<size_t>(base.height), 0);
    } catch (...) {
        return false;
    }
    mask_w_ = base.width;
    mask_h_ = base.height;
    mask_used = false;
    mask_gen += 1;
    invalidateView();
    return true;
}

void App::studioReset() {
    if (!mask_.empty()) std::fill(mask_.begin(), mask_.end(), 0);
    mask_used = false;
    mask_undo_.clear();
    mask_redo_.clear();
    mask_undo_len = 0;
    mask_redo_len = 0;
    mask_gen += 1;
    invalidateView();
    mask_preview_gen_ = ~0u;
    setStatus("Retouch cleared", false);
}

void App::maskStampAt(float cx, float cy, bool erase) {
    const int32_t mw = mask_w_;
    const int32_t mh = mask_h_;
    if (mw <= 0 || mh <= 0 || mask_.empty()) return;

    const float radius = std::max(0.6f, studio_size * 0.5f);
    const float hard = std::clamp(studio_hard, 0.0f, 0.98f);
    const float opac = std::clamp(studio_opac, 0.01f, 1.0f);
    const float inner = radius * hard;
    const float falloff = 1.0f / std::max(0.0001f, radius - inner);

    const int32_t x0 = std::max(0, static_cast<int32_t>(std::floor(cx - radius)));
    const int32_t y0 = std::max(0, static_cast<int32_t>(std::floor(cy - radius)));
    const int32_t x1 = std::min(mw - 1, static_cast<int32_t>(std::floor(cx + radius)));
    const int32_t y1 = std::min(mh - 1, static_cast<int32_t>(std::floor(cy + radius)));
    if (x1 < x0 || y1 < y0) return;

    const size_t mwu = static_cast<size_t>(mw);
    for (int32_t yy = y0; yy <= y1; ++yy) {
        uint8_t* row = mask_.data() + static_cast<size_t>(yy) * mwu;
        const float dy = static_cast<float>(yy) + 0.5f - cy;
        for (int32_t xx = x0; xx <= x1; ++xx) {
            const float dx = static_cast<float>(xx) + 0.5f - cx;
            const float d = std::sqrt(dx * dx + dy * dy);
            float t = 1.0f;
            if (d > inner) t = 1.0f - (d - inner) * falloff;
            if (t <= 0.0f) continue;
            const float a = opac * t;
            const float cur = static_cast<float>(row[xx]);
            const float nv = erase ? cur + (255.0f - cur) * a : cur * (1.0f - a);
            row[xx] = static_cast<uint8_t>(std::clamp(nv, 0.0f, 255.0f));
        }
    }

    stroke_min_x_ = std::min(stroke_min_x_, x0);
    stroke_min_y_ = std::min(stroke_min_y_, y0);
    stroke_max_x_ = std::max(stroke_max_x_, x1);
    stroke_max_y_ = std::max(stroke_max_y_, y1);
}

void App::studioBegin(float px, float py) {
    if (!studioEnsureMask() || mask_.empty()) return;
    stroke_before_ = mask_;
    stroke_active_ = true;
    stroke_min_x_ = mask_w_;
    stroke_min_y_ = mask_h_;
    stroke_max_x_ = -1;
    stroke_max_y_ = -1;
    stroke_last_x_ = px;
    stroke_last_y_ = py;

    maskStampAt(px, py, studio_erase);
    mask_used = true;
    mask_gen += 1;
    invalidateView();
}

void App::studioTo(float px, float py) {
    if (!stroke_active_) return;
    const float spacing = std::max(1.0f, studio_size * 0.18f);
    const float dx = px - stroke_last_x_;
    const float dy = py - stroke_last_y_;
    const float dist = std::sqrt(dx * dx + dy * dy);
    const int steps = static_cast<int>(std::max(1.0f, std::ceil(dist / spacing)));
    for (int k = 1; k <= steps; ++k) {
        const float t = static_cast<float>(k) / static_cast<float>(steps);
        maskStampAt(stroke_last_x_ + dx * t, stroke_last_y_ + dy * t, studio_erase);
    }
    stroke_last_x_ = px;
    stroke_last_y_ = py;
    mask_gen += 1;
    invalidateView();
}

void App::studioEnd() {
    if (!stroke_active_) return;
    stroke_active_ = false;
    if (stroke_max_x_ < stroke_min_x_ || stroke_max_y_ < stroke_min_y_ ||
        stroke_before_.size() != mask_.size()) {
        stroke_before_.clear();
        return;
    }
    const int32_t x = stroke_min_x_;
    const int32_t y = stroke_min_y_;
    const int32_t w = stroke_max_x_ - x + 1;
    const int32_t h = stroke_max_y_ - y + 1;
    MaskEdit edit;
    edit.x = x;
    edit.y = y;
    edit.w = w;
    edit.h = h;
    edit.data = copyMaskRegionVec(stroke_before_, mask_w_, x, y, w, h);
    if (!edit.data.empty()) pushMaskUndo(std::move(edit));
    stroke_before_.clear();
    setStatus("Retouched - " + std::to_string(mask_undo_len) + " stroke(s)", false);
}

std::vector<uint8_t> App::copyMaskRegionVec(const std::vector<uint8_t>& src, int32_t stride,
                                            int32_t x, int32_t y, int32_t w, int32_t h) const {
    std::vector<uint8_t> out;
    if (stride <= 0 || w <= 0 || h <= 0) return out;
    out.resize(static_cast<size_t>(w) * static_cast<size_t>(h));
    for (int32_t row = 0; row < h; ++row) {
        const size_t off = static_cast<size_t>(y + row) * static_cast<size_t>(stride) +
                           static_cast<size_t>(x);
        if (off + static_cast<size_t>(w) > src.size()) {
            out.clear();
            return out;
        }
        std::memcpy(out.data() + static_cast<size_t>(row) * static_cast<size_t>(w),
                    src.data() + off, static_cast<size_t>(w));
    }
    return out;
}

void App::writeMaskRegion(const MaskEdit& edit) {
    if (mask_w_ <= 0 || edit.w <= 0 || edit.h <= 0) return;
    for (int32_t row = 0; row < edit.h; ++row) {
        const size_t off = static_cast<size_t>(edit.y + row) * static_cast<size_t>(mask_w_) +
                           static_cast<size_t>(edit.x);
        if (off + static_cast<size_t>(edit.w) > mask_.size()) return;
        std::memcpy(mask_.data() + off,
                    edit.data.data() + static_cast<size_t>(row) * static_cast<size_t>(edit.w),
                    static_cast<size_t>(edit.w));
    }
}

void App::pushMaskUndo(MaskEdit&& edit) {
    constexpr size_t kCap = 32;
    if (mask_undo_.size() >= kCap) mask_undo_.erase(mask_undo_.begin());
    mask_undo_.push_back(std::move(edit));
    mask_undo_len = static_cast<int>(mask_undo_.size());
    clearMaskRedo();
}

void App::pushMaskRedo(MaskEdit&& edit) {
    constexpr size_t kCap = 32;
    if (mask_redo_.size() >= kCap) mask_redo_.erase(mask_redo_.begin());
    mask_redo_.push_back(std::move(edit));
    mask_redo_len = static_cast<int>(mask_redo_.size());
}

void App::clearMaskRedo() {
    mask_redo_.clear();
    mask_redo_len = 0;
}

void App::maskUndo() {
    if (mask_undo_.empty()) {
        setStatus("Nothing to undo", true);
        return;
    }
    MaskEdit edit = std::move(mask_undo_.back());
    mask_undo_.pop_back();
    mask_undo_len = static_cast<int>(mask_undo_.size());

    std::vector<uint8_t> after = copyMaskRegionVec(mask_, mask_w_, edit.x, edit.y, edit.w,
                                                   edit.h);
    if (!after.empty()) {
        MaskEdit redo = edit;
        redo.data = std::move(after);
        pushMaskRedo(std::move(redo));
    }
    writeMaskRegion(edit);
    mask_used = true;
    mask_gen += 1;
    invalidateView();
    setStatus("Retouch undo", false);
}

void App::maskRedo() {
    if (mask_redo_.empty()) {
        setStatus("Nothing to redo", true);
        return;
    }
    MaskEdit edit = std::move(mask_redo_.back());
    mask_redo_.pop_back();
    mask_redo_len = static_cast<int>(mask_redo_.size());

    std::vector<uint8_t> before = copyMaskRegionVec(mask_, mask_w_, edit.x, edit.y, edit.w,
                                                    edit.h);
    if (!before.empty()) {
        MaskEdit undo = edit;
        undo.data = std::move(before);
        if (mask_undo_.size() >= 32) mask_undo_.erase(mask_undo_.begin());
        mask_undo_.push_back(std::move(undo));
        mask_undo_len = static_cast<int>(mask_undo_.size());
    }
    writeMaskRegion(edit);
    mask_used = true;
    mask_gen += 1;
    invalidateView();
    setStatus("Retouch redo", false);
}

void App::updateDisplay() {
    if (display_gen_ == view_gen_) return;
    display_gen_ = view_gen_;
    display.reset();
    if (!mask_used || mask_.empty() || !result.valid()) return;
    const ff::Image* orig = originalFor(result.width, result.height);
    if (!orig || !orig->valid()) return;
    ff::Image disp = result.clone();
    if (!disp.valid()) return;
    if (!ff::blendMask(disp, *orig, mask_.data(), mask_w_, mask_h_)) return;
    display = std::move(disp);
}

const ff::Image* App::originalFor(int32_t w, int32_t h) {
    if (w <= 0 || h <= 0) return nullptr;
    if (proc_w == w && proc_h == h && proc.valid() && proc.width == w && proc.height == h) {
        return &proc;
    }
    if (orig_cache_.valid() && orig_cache_w_ == w && orig_cache_h_ == h &&
        orig_cache_gen_ == base_gen) {
        return &orig_cache_;
    }
    if (!base.valid()) return nullptr;
    orig_cache_.reset();
    orig_cache_ = base.resized(w, h);
    orig_cache_w_ = w;
    orig_cache_h_ = h;
    orig_cache_gen_ = base_gen;
    return orig_cache_.valid() ? &orig_cache_ : nullptr;
}

const ff::Image* App::displayImage() {
    updateDisplay();
    if (display.valid()) return &display;
    if (result.valid()) return &result;
    return nullptr;
}

const uint8_t* App::maskPreviewData(int* out_w, int* out_h) {
    if (out_w) *out_w = 0;
    if (out_h) *out_h = 0;
    if (!mask_used || mask_.empty() || mask_w_ <= 0 || mask_h_ <= 0) return nullptr;
    if (mask_preview_gen_ != mask_gen || mask_preview_.empty()) {
        mask_preview_gen_ = mask_gen;
        const int32_t mw = mask_w_;
        const int32_t mh = mask_h_;
        const float longest = static_cast<float>(std::max(mw, mh));
        const float f = longest > 900.0f ? 900.0f / longest : 1.0f;
        const int32_t ow = std::max(1, static_cast<int32_t>(static_cast<float>(mw) * f));
        const int32_t oh = std::max(1, static_cast<int32_t>(static_cast<float>(mh) * f));
        mask_preview_.assign(static_cast<size_t>(ow) * static_cast<size_t>(oh) * 4u, 0);
        const float fx = static_cast<float>(mw) / static_cast<float>(ow);
        const float fy = static_cast<float>(mh) / static_cast<float>(oh);
        for (int32_t y = 0; y < oh; ++y) {
            const int32_t my = std::min(mh - 1, static_cast<int32_t>((static_cast<float>(y) + 0.5f) * fy));
            const uint8_t* row = mask_.data() + static_cast<size_t>(my) * static_cast<size_t>(mw);
            for (int32_t x = 0; x < ow; ++x) {
                const int32_t mx = std::min(mw - 1, static_cast<int32_t>((static_cast<float>(x) + 0.5f) * fx));
                const float a = static_cast<float>(row[mx]) * (0.42f);
                uint8_t* px = mask_preview_.data() +
                              (static_cast<size_t>(y) * static_cast<size_t>(ow) +
                               static_cast<size_t>(x)) * 4u;
                px[0] = 255;
                px[1] = 70;
                px[2] = 58;
                px[3] = static_cast<uint8_t>(std::clamp(a, 0.0f, 255.0f));
            }
        }
        mask_preview_w_ = ow;
        mask_preview_h_ = oh;
    }
    if (out_w) *out_w = mask_preview_w_;
    if (out_h) *out_h = mask_preview_h_;
    return mask_preview_.empty() ? nullptr : mask_preview_.data();
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
    const ff::Image* shown = displayImage();
    if (!shown || !shown->valid()) {
        setStatus("Nothing to save", true);
        return;
    }
    if (shown->save(path.c_str())) {
        save_path = path;
        setStatus("Saved - " + path, false);
    } else {
        setStatus("Could not save - " + path, true);
    }
}

void App::copyNow() {
    const ff::Image* shown = displayImage();
    if (!shown || !shown->valid()) {
        setStatus("Nothing to copy", true);
        return;
    }
    clipboard_png.clear();
    if (shown->encodePng(clipboard_png)) {
        clipboard_gen++;
        setStatus("Copied to clipboard", false);
    } else {
        clipboard_png.clear();
        setStatus("Could not encode image", true);
    }
}

bool App::encodeDisplayPng(std::vector<uint8_t>& out) {
    const ff::Image* shown = displayImage();
    if (!shown || !shown->valid()) return false;
    out.clear();
    return shown->encodePng(out);
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

int App::identityCount() const {
    std::lock_guard<std::mutex> lk(ident_mu_);
    return std::min(static_cast<int>(identities.size()), kMaxIdentities);
}

bool App::identityEmbedding(int index, float out[512]) const {
    std::lock_guard<std::mutex> lk(ident_mu_);
    if (index < 0 || index >= static_cast<int>(identities.size())) return false;
    std::memcpy(out, identities[static_cast<size_t>(index)].embedding, sizeof(float) * 512);
    return true;
}

int App::defaultIdentity() const {
    std::lock_guard<std::mutex> lk(ident_mu_);
    return default_identity;
}

ff::Engine* App::ensureVideoEngine(bool sequential) {
    if (engine.ptr && engine.sequential == sequential) return &engine;
    if (engine.ptr) engine.destroy();
    if (model_dir.empty()) {
        video_engine_error_ = "Model directory is empty";
        return nullptr;
    }
    std::string err;
    if (!engine.create(model_dir.c_str(), static_cast<int>(provider), sequential, &err)) {
        video_engine_error_ = err;
        return nullptr;
    }
    provider_name = engine.provider();
    engine.setRestoreMouth(restore_mouth ? 1 : 0);
    return &engine;
}

// ---- worker ----------------------------------------------------------------

void App::spawnJob(JobKind kind, int arg) {
    if (videoBusy()) {
        setStatus("A video job is running - finish or cancel it first", true);
        return;
    }
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
    job_upscale_scale_ = upscale_scale;
    job_restore_mouth_ = restore_mouth;
    // The upscaler always works on the full-resolution result, so fast preview
    // is skipped while it is on.
    job_fast_preview_ = fast_preview && pending_export_ == ExportKind::None &&
                        upscale_scale == 0;
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
    const int upscale = job_upscale_scale_;
    mix(&upscale, sizeof(upscale));
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

    // Optional standalone Real-ESRGAN pass on the composed result. Unlike the
    // ESRGAN entries in the old enhancer combo this keeps the model's output
    // size, so the result really is larger.
    if (job_upscale_scale_ > 0) {
        setStage(Stage::Enhancing, 950);
        const int64_t t1 = ff::nowNs();
        ff::Image up;
        if (!engine.upscale(cur, job_upscale_scale_, &up, progressCb, this)) {
            return jobFail(engine.lastError());
        }
        cur = std::move(up);
        timings.enhance_ms += nsToMs(ff::nowNs() - t1);
    }

    const size_t out_bytes = static_cast<size_t>(cur.width) * static_cast<size_t>(cur.height) * 3;
    pending_result_.assign(cur.data, cur.data + out_bytes);
    pending_w_ = cur.width;
    pending_h_ = cur.height;
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
    view_gen_ += 1;
    updateDisplay();
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
    display.reset();
    view_gen_ += 1;
    // Nothing to blend while there is no result; mark as built so the next
    // frame does not rebuild for no reason.
    display_gen_ = view_gen_;
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
