//! Video workspace — a purpose-built face-swap editor for clips.
//!
//! Deliberately independent from the still-image editor: it owns its timeline,
//! project file, analysis data, frame caches and jobs, and only borrows the
//! identity library, the engine and the status bar through `Host`.
//!
//! Design notes
//! ------------
//! * Frames are decoded on demand straight from the container. Nothing is
//!   extracted unless a processed frame is worth caching.
//! * Analysis is chunked and resumable. Finished chunks are folded into the
//!   project immediately, so closing the app never loses completed work.
//! * Detections are grouped into a small number of editable sequences. An
//!   identity assignment applies to a whole sequence and can be propagated
//!   forward; per-frame overrides handle the exceptions.
//! * Processed frames are cached per quality and per content signature, so
//!   changing an assignment invalidates stale caches instead of showing them.
//! * Export always runs at the source resolution and streams raw frames into
//!   ffmpeg; long exports can be split into parts and concatenated losslessly.

const std = @import("std");
const dvui = @import("dvui");
const ff = @import("ff.zig");

const alloc = std.heap.c_allocator;

pub const max_faces = 8;
/// Assignment sentinel for "leave this face untouched". Identity ids are >= 0
/// and -1 means "use the default identity", so -2 is free for skip.
const skip_id: i32 = -2;
const max_assigns = 4096;
const max_overrides = 4096;
const max_chunks = 20000;
const max_segments = 240;
const history_cap = 12;

// ---------------------------------------------------------------------------
// Host bridge
// ---------------------------------------------------------------------------

pub const Host = struct {
    user: *anyopaque,
    allocator: std.mem.Allocator,
    /// Returns a ready engine (creating it if needed) or null.
    ensure_engine: *const fn (user: *anyopaque) ?*ff.Engine,
    /// Same, but requests sequential sessions (one model resident at a time)
    /// so long video jobs stay inside a small VRAM budget.
    ensure_engine_seq: *const fn (user: *anyopaque, sequential: bool) ?*ff.Engine,
    /// True while the still-image worker owns the engine.
    engine_busy: *const fn (user: *anyopaque) bool,
    identity_count: *const fn (user: *anyopaque) usize,
    identity_embedding: *const fn (user: *anyopaque, i: usize) *const [512]f32,
    identity_label: *const fn (user: *anyopaque, i: usize) []const u8,
    /// Face thumbnail for identity `i`, or null until it has been uploaded.
    identity_thumb: *const fn (user: *anyopaque, i: usize) ?dvui.Texture,
    default_identity: *const fn (user: *anyopaque) usize,
    enhancer: *const fn (user: *anyopaque) i32,
    restore_mouth: *const fn (user: *anyopaque) bool,
    set_restore_mouth: *const fn (user: *anyopaque, on: bool) void,
    draw_identities: *const fn (user: *anyopaque) void,
    status: *const fn (user: *anyopaque, text: []const u8, is_error: bool) void,
    /// Folder for temporary files (cache, parts). Empty = next to the video.
    temp_dir: *const fn (user: *anyopaque) []const u8,
    /// Default folder for exported videos. Empty = next to the video.
    export_dir: *const fn (user: *anyopaque) []const u8,
};

// ---------------------------------------------------------------------------
// Persistent model
// ---------------------------------------------------------------------------

const Sample = struct {
    f: i64 = 0,
    n: u8 = 0,
    b: [max_faces][4]f32 = @splat(@splat(0)),
};

const Chunk = struct {
    start: i64 = 0,
    end: i64 = 0,
    done: bool = false,
};

/// Identity assignment range. `ids[i] == -1` uses the default identity and
/// `ids[i] == skip_id` leaves face i exactly as it is in the source frame.
const Assign = struct {
    a: i64 = 0,
    b: i64 = 0,
    ids: [max_faces]i32 = @splat(-1),
};

const Override = struct {
    f: i64 = 0,
    ids: [max_faces]i32 = @splat(-1),
};

/// A contiguous run of frames with the same face count and stable positions.
const Segment = struct {
    start: i64 = 0,
    end: i64 = 0,
    count: u8 = 0,
    boxes: [max_faces][4]f32 = @splat(@splat(0)),
};

/// Small face crop shown next to a sequence assignment so the numbered rows
/// can be matched to the faces in the viewer instead of reading only text.
const FaceThumb = struct {
    img: ?ff.Image = null,
    tex: ?dvui.Texture = null,
    dirty: bool = false,
};

const SampleJson = struct { f: i64 = 0, n: u8 = 0, b: []const [4]f32 = &.{} };
const ChunkJson = struct { start: i64 = 0, end: i64 = 0, done: bool = false };
const AssignJson = struct { a: i64 = 0, b: i64 = 0, ids: []const i32 = &.{} };
const OverrideJson = struct { f: i64 = 0, ids: []const i32 = &.{} };

// --- Studio retouch --------------------------------------------------------
//
// A painted retouch is stored as a list of brush strokes in source-frame pixel
// coordinates. Keeping the geometry instead of a raster lets one edit be
// re-applied at any resolution and translated/scaled when copied onto a similar
// face in another frame.

const StrokePt = struct { x: f32 = 0, y: f32 = 0 };

const Stroke = struct {
    pts: std.ArrayListUnmanaged(StrokePt) = .empty,
    radius: f32 = 30,
    /// true = reveal the original pixels (erase), false = paint the result back.
    reveal: bool = true,

    pub fn jsonStringify(self: *const Stroke, jw: anytype) !void {
        try jw.beginObject();
        try jw.objectField("radius");
        try jw.write(self.radius);
        try jw.objectField("reveal");
        try jw.write(self.reveal);
        try jw.objectField("pts");
        try jw.write(self.pts.items);
        try jw.endObject();
    }
};

/// Manual Studio retouch for one frame. `bypassed` keeps the strokes around but
/// stops them from being applied, so an edit can be toggled off without losing
/// the work.
const FrameEdit = struct {
    f: i64 = 0,
    bypassed: bool = false,
    strokes: std.ArrayListUnmanaged(Stroke) = .empty,

    pub fn jsonStringify(self: *const FrameEdit, jw: anytype) !void {
        try jw.beginObject();
        try jw.objectField("f");
        try jw.write(self.f);
        try jw.objectField("bypassed");
        try jw.write(self.bypassed);
        try jw.objectField("strokes");
        try jw.write(self.strokes.items);
        try jw.endObject();
    }
};

const StrokePtJson = struct { x: f32 = 0, y: f32 = 0 };
const StrokeJson = struct { pts: []const StrokePtJson = &.{}, radius: f32 = 30, reveal: bool = true };
const FrameEditJson = struct { f: i64 = 0, bypassed: bool = false, strokes: []const StrokeJson = &.{} };

const ProjectJson = struct {
    version: u32 = 1,
    video: []const u8 = "",
    in_frame: i64 = 0,
    out_frame: i64 = 0,
    chunk: i64 = 300,
    stride: i64 = 1,
    chunk_f: f32 = 300,
    stride_f: f32 = 1,
    scrubbing: bool = false,
    /// Faces with no explicit assignment are left untouched instead of using
    /// the default identity.
    default_skip: bool = false,
    samples: []const SampleJson = &.{},
    chunks: []const ChunkJson = &.{},
    assigns: []const AssignJson = &.{},
    overrides: []const OverrideJson = &.{},
    edits: []const FrameEditJson = &.{},
};

const JobKind = enum { none, analyze, process, encode };

/// Automated end-to-end check driven by FACEFORGE_VIDEOTEST.
const TestState = enum { off, start, wait_analyze, wait_process, wait_export, wait_play, done };

const Quality = enum { low, high };

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

pub const App = struct {
    host: Host = undefined,
    host_ready: bool = false,
    inited: bool = false,

    path: [1024]u8 = @splat(0),
    path_len: usize = 0,
    proj_path: [1024]u8 = @splat(0),
    proj_path_len: usize = 0,
    cache_root: [1024]u8 = @splat(0),
    cache_root_len: usize = 0,
    name: [128]u8 = @splat(0),
    name_len: usize = 0,

    open: bool = false,
    info: ff.Video.Info = .{},

    in_frame: i64 = 0,
    out_frame: i64 = 0,
    chunk: i64 = 300,
    stride: i64 = 1,
    chunk_f: f32 = 300,
    stride_f: f32 = 1,
    scrubbing: bool = false,
    loop_play: bool = true,
    playing: bool = false,
    /// When set, playback (and loop) is confined to the selected sequence
    /// instead of the analysis in/out range.
    play_sequence: bool = false,

    cur: i64 = 0,
    decoder: ?ff.Video = null,
    cur_loaded: i64 = -1,
    cur_img: ?ff.Image = null,
    cur_tex: ?dvui.Texture = null,
    res_img: ?ff.Image = null,
    res_tex: ?dvui.Texture = null,
    /// The unedited cached/rendered result, kept so a retouch edit can be
    /// re-applied whenever the strokes change.
    res_base: ?ff.Image = null,
    res_frame: i64 = -1,
    res_sig: u64 = 0xffff_ffff_ffff_ffff,
    res_missing_sig: u64 = 0xffff_ffff_ffff_ffff,
    /// `edit_gen` value `res_img` was rebuilt from.
    res_edit_gen: u64 = 0xffff_ffff_ffff_ffff,
    show_original: bool = false,
    /// Draw the detected-face boxes over the viewer.
    show_boxes: bool = true,

    // Studio (per-frame retouch)
    studio: bool = false,
    studio_erase: bool = true,
    studio_size: f32 = 60.0,
    studio_hard: f32 = 0.6,
    studio_opac: f32 = 0.9,
    studio_show_mask: bool = true,
    studio_view_original: bool = false,
    studio_hover: ?dvui.Point.Physical = null,
    stroke_active: bool = false,
    stroke_last_x: f32 = 0,
    stroke_last_y: f32 = 0,
    /// Copy-to-similar-faces confirmation: how many target frames already carry
    /// an edit, so the user can overwrite or skip them.
    copy_conflicts: usize = 0,
    copy_pending: bool = false,
    /// Set by the panel "Close" button; acted on once any running job stops.
    close_requested: bool = false,
    play_t0: i64 = 0,
    play_from: i64 = 0,

    low_quality: bool = true,
    export_audio: bool = true,
    export_chunks: bool = false,
    export_whole: bool = false,
    process_use_cache: bool = true,

    samples: std.ArrayListUnmanaged(Sample) = .empty,
    chunks: std.ArrayListUnmanaged(Chunk) = .empty,
    assigns: std.ArrayListUnmanaged(Assign) = .empty,
    overrides: std.ArrayListUnmanaged(Override) = .empty,
    segments: std.ArrayListUnmanaged(Segment) = .empty,
    /// Manual Studio retouch, one entry per edited frame.
    edits: std.ArrayListUnmanaged(FrameEdit) = .empty,
    /// Bumped whenever any frame edit changes so the viewer can rebuild.
    edit_gen: u64 = 1,
    data_mu: std.atomic.Mutex = .unlocked,

    sel_seg: i32 = -1,
    seg_ids: [max_faces]i32 = @splat(-1),
    seg_scope_all: bool = true,
    /// Faces with no explicit assignment are left exactly as in the source
    /// instead of using the default identity. Useful when only one person in a
    /// scene should be swapped.
    default_skip: bool = false,
    /// Face crops for the selected sequence, rebuilt when the selection changes.
    face_thumbs: [max_faces]FaceThumb = @splat(FaceThumb{}),
    face_thumb_seg: i32 = -1,
    /// Set when segments are rebuilt; the crops are freed on the next draw so
    /// this never touches textures from a pre-window preload.
    face_thumbs_stale: bool = false,

    // job
    job: JobKind = .none,
    thread: ?std.Thread = null,
    cancel: std.atomic.Value(bool) = .init(false),
    done: std.atomic.Value(bool) = .init(false),
    ok: std.atomic.Value(bool) = .init(false),
    progress: std.atomic.Value(u32) = .init(0),
    job_msg: [256]u8 = @splat(0),
    job_msg_len: usize = 0,
    job_from: i64 = 0,
    job_to: i64 = 0,
    job_low: bool = true,
    job_audio: bool = false,
    job_chunks: bool = false,
    job_use_cache: bool = true,
    job_quality: Quality = .low,
    job_out: [1024]u8 = @splat(0),
    job_out_len: usize = 0,
    job_done_frames: std.atomic.Value(u32) = .init(0),
    job_total_frames: std.atomic.Value(u32) = .init(1),

    // ui
    enhancer_failed: bool = false,
    /// Cached total VRAM for the enhancer hint (NVML is not free, so refresh at
    /// most twice per second).
    vram_total_mb: f32 = 0,
    vram_checked_ms: i64 = 0,
    shutting_down: bool = false,
    test_state: TestState = .off,
    test_path: [1024]u8 = @splat(0),
    test_path_len: usize = 0,
    test_done: bool = false,
    test_keep: bool = false,
    test_play_moves: i32 = 0,
    test_play_last: i64 = -1,
    test_play_budget: i32 = 0,

    status: [256]u8 = @splat(0),
    status_len: usize = 0,
    status_err: bool = false,
    err: [256]u8 = @splat(0),
    err_len: usize = 0,
    show_timeline: bool = true,
    view_zoom: f32 = 1.0,

    fn statusSlice(self: *const App) []const u8 {
        return self.status[0..self.status_len];
    }
    fn errSlice(self: *const App) []const u8 {
        return self.err[0..self.err_len];
    }
};

var g: App = .{};

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

fn lockData() void {
    while (!g.data_mu.tryLock()) std.atomic.spinLoopHint();
}

fn setStatus(text: []const u8, is_error: bool) void {
    const n = @min(text.len, g.status.len);
    @memcpy(g.status[0..n], text[0..n]);
    g.status_len = n;
    g.status_err = is_error;
}

fn setStatusFmt(comptime fmt: []const u8, args: anytype, is_error: bool) void {
    var buf: [256]u8 = undefined;
    const text = std.fmt.bufPrint(&buf, fmt, args) catch return;
    setStatus(text, is_error);
}

fn setErr(text: []const u8) void {
    const n = @min(text.len, g.err.len);
    @memcpy(g.err[0..n], text[0..n]);
    g.err_len = n;
}

fn pathSlice(buf: *const [1024]u8, len: usize) []const u8 {
    return buf[0..len];
}

fn copyPath(dst: *[1024]u8, dst_len: *usize, src: []const u8) void {
    const n = @min(src.len, dst.len - 1);
    @memcpy(dst[0..n], src[0..n]);
    dst[n] = 0;
    dst_len.* = n;
}

fn zPath(buf: *[1024]u8, len: usize) [:0]const u8 {
    return buf[0..len :0];
}

fn baseName(path: []const u8) []const u8 {
    var i = path.len;
    while (i > 0) : (i -= 1) {
        if (path[i - 1] == '/' or path[i - 1] == '\\') return path[i..];
    }
    return path;
}

fn stripExt(name: []const u8) []const u8 {
    if (std.mem.lastIndexOfScalar(u8, name, '.')) |i| return name[0..i];
    return name;
}

fn joinZ(buf: []u8, parts: []const []const u8) ![:0]u8 {
    var n: usize = 0;
    for (parts, 0..) |p, i| {
        if (p.len == 0) continue;
        if (i > 0 and n > 0 and buf[n - 1] != '/' and p[0] != '/') {
            buf[n] = '/';
            n += 1;
        }
        if (n + p.len + 1 > buf.len) return error.NoSpace;
        @memcpy(buf[n .. n + p.len], p);
        n += p.len;
    }
    buf[n] = 0;
    return buf[0..n :0];
}

fn timeText(buf: []u8, frame: i64) []const u8 {
    const fps = if (g.info.fps > 0) g.info.fps else 25.0;
    const secs = @as(f64, @floatFromInt(frame)) / fps;
    const ms: i64 = @intFromFloat(@round(secs * 1000.0));
    const total_s = @divTrunc(ms, 1000);
    const m = @divTrunc(total_s, 60);
    const s = @mod(total_s, 60);
    const frac = @mod(ms, 1000);
    return std.fmt.bufPrint(buf, "{d:0>2}:{d:0>2}.{d:0>3}", .{ m, s, frac }) catch "?";
}

/// Zero-pad `value` to `width` digits. Frame counters are rendered this way so
/// their text keeps a constant width and the viewer never resizes as the frame
/// number gains digits during playback.
fn padNum(buf: []u8, value: i64, width: usize) []const u8 {
    var tmp: [24]u8 = undefined;
    const s = std.fmt.bufPrint(&tmp, "{d}", .{value}) catch return "0";
    if (s.len >= width or width > buf.len) {
        @memcpy(buf[0..s.len], s);
        return buf[0..s.len];
    }
    const zeros = width - s.len;
    @memset(buf[0..zeros], '0');
    @memcpy(buf[zeros..][0..s.len], s);
    return buf[0 .. zeros + s.len];
}

/// Digit width used for frame counters in labels (at least three digits).
fn frameWidth() usize {
    var v = @max(@as(i64, 0), g.info.frames);
    var w: usize = 1;
    while (v >= 10) : (v = @divTrunc(v, 10)) w += 1;
    return @max(@as(usize, 3), w);
}

fn cacheSig() u64 {
    var h = std.hash.Wyhash.init(0);
    const cnt = g.host.identity_count(g.host.user);
    h.update(std.mem.asBytes(&cnt));
    const enh = g.host.enhancer(g.host.user);
    h.update(std.mem.asBytes(&enh));
    const mouth = g.host.restore_mouth(g.host.user);
    h.update(std.mem.asBytes(&mouth));
    h.update(std.mem.asBytes(&g.default_skip));
    lockData();
    defer g.data_mu.unlock();
    for (g.assigns.items) |a| {
        h.update(std.mem.asBytes(&a.a));
        h.update(std.mem.asBytes(&a.b));
        for (a.ids) |id| h.update(std.mem.asBytes(&id));
    }
    for (g.overrides.items) |o| {
        h.update(std.mem.asBytes(&o.f));
        for (o.ids) |id| h.update(std.mem.asBytes(&id));
    }
    return h.final();
}

fn idsFor(frame: i64, count: usize, out: *[max_faces]i32) void {
    const fallback: i32 = if (g.default_skip) skip_id else -1;
    for (out) |*v| v.* = fallback;
    lockData();
    defer g.data_mu.unlock();
    for (g.overrides.items) |o| {
        if (o.f == frame) {
            @memcpy(out[0..count], o.ids[0..count]);
            return;
        }
    }
    var best: ?*const Assign = null;
    for (g.assigns.items) |*a| {
        if (frame >= a.a and frame <= a.b) best = a;
    }
    if (best) |a| @memcpy(out[0..count], a.ids[0..count]);
}

fn upsertAssign(a: i64, b: i64, ids: *const [max_faces]i32) void {
    lockData();
    defer g.data_mu.unlock();
    var i: usize = 0;
    while (i < g.assigns.items.len) {
        const cur = g.assigns.items[i];
        if (cur.b < a or cur.a > b) {
            i += 1;
            continue;
        }
        // Merge overlapping ranges into the new one.
        const na = @min(cur.a, a);
        const nb = @max(cur.b, b);
        _ = g.assigns.orderedRemove(i);
        return upsertAssignLocked(na, nb, ids);
    }
    upsertAssignLocked(a, b, ids);
}

fn upsertAssignLocked(a: i64, b: i64, ids: *const [max_faces]i32) void {
    if (g.assigns.items.len >= max_assigns) {
        _ = g.assigns.orderedRemove(0);
    }
    g.assigns.append(alloc, .{ .a = a, .b = b, .ids = ids.* }) catch {};
}

fn setOverride(frame: i64, ids: *const [max_faces]i32) void {
    lockData();
    defer g.data_mu.unlock();
    var i: usize = 0;
    while (i < g.overrides.items.len) : (i += 1) {
        if (g.overrides.items[i].f == frame) {
            g.overrides.items[i].ids = ids.*;
            return;
        }
    }
    if (g.overrides.items.len >= max_overrides) _ = g.overrides.orderedRemove(0);
    g.overrides.append(alloc, .{ .f = frame, .ids = ids.* }) catch {};
}

fn clearOverride(frame: i64) void {
    lockData();
    defer g.data_mu.unlock();
    var i: usize = 0;
    while (i < g.overrides.items.len) : (i += 1) {
        if (g.overrides.items[i].f == frame) {
            _ = g.overrides.orderedRemove(i);
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// Studio retouch (per-frame manual mask)
// ---------------------------------------------------------------------------

fn freeFrameEdit(e: *FrameEdit) void {
    for (e.strokes.items) |*st| st.pts.deinit(alloc);
    e.strokes.deinit(alloc);
}

fn freeAllEdits() void {
    for (g.edits.items) |*e| freeFrameEdit(e);
    g.edits.clearAndFree(alloc);
}

/// Index of the edit for `frame`, or null. Caller holds `data_mu`.
fn frameEditIndex(frame: i64) ?usize {
    for (g.edits.items, 0..) |e, i| {
        if (e.f == frame) return i;
    }
    return null;
}

/// Index of the edit for `frame`, creating it if needed. Caller holds `data_mu`.
fn ensureEditLocked(frame: i64) ?usize {
    if (frameEditIndex(frame)) |i| return i;
    if (g.edits.items.len >= 4096) return null;
    g.edits.append(alloc, .{ .f = frame }) catch return null;
    return g.edits.items.len - 1;
}

/// Does `frame` carry a retouch that should actually be applied?
fn hasActiveEdit(frame: i64) bool {
    lockData();
    defer g.data_mu.unlock();
    const idx = frameEditIndex(frame) orelse return false;
    const e = &g.edits.items[idx];
    return !e.bypassed and e.strokes.items.len > 0;
}

/// Number of edited frames (bypassed included) inside `[a, b]`.
fn editsInRange(a: i64, b: i64) usize {
    lockData();
    defer g.data_mu.unlock();
    var n: usize = 0;
    for (g.edits.items) |e| {
        if (e.f >= a and e.f <= b) n += 1;
    }
    return n;
}

fn deleteCurrentEdit() void {
    lockData();
    if (frameEditIndex(g.cur)) |idx| {
        freeFrameEdit(&g.edits.items[idx]);
        _ = g.edits.orderedRemove(idx);
    }
    g.data_mu.unlock();
    g.edit_gen +%= 1;
    saveProject();
    setStatus("Frame retouch deleted", false);
}

fn clearAllEdits() void {
    lockData();
    for (g.edits.items) |*e| freeFrameEdit(e);
    g.edits.clearRetainingCapacity();
    g.data_mu.unlock();
    g.edit_gen +%= 1;
    saveProject();
    setStatus("All frame retouches cleared", false);
}

fn setCurrentBypass(on: bool) void {
    lockData();
    if (frameEditIndex(g.cur)) |idx| {
        g.edits.items[idx].bypassed = on;
    }
    g.data_mu.unlock();
    g.edit_gen +%= 1;
    saveProject();
}

/// Drop the last stroke of the current frame (a one-step undo per stroke). An
/// edit that runs out of strokes is removed entirely.
fn undoStroke() void {
    lockData();
    if (frameEditIndex(g.cur)) |idx| {
        const e = &g.edits.items[idx];
        if (e.strokes.items.len > 0) {
            const st = &e.strokes.items[e.strokes.items.len - 1];
            st.pts.deinit(alloc);
            _ = e.strokes.pop();
        }
        if (e.strokes.items.len == 0) {
            freeFrameEdit(&g.edits.items[idx]);
            _ = g.edits.orderedRemove(idx);
        }
    }
    g.data_mu.unlock();
    g.edit_gen +%= 1;
    saveProject();
}

/// `{strokes, bypassed}` for the current frame.
fn currentEditInfo() struct { strokes: usize, bypassed: bool } {
    lockData();
    defer g.data_mu.unlock();
    const idx = frameEditIndex(g.cur) orelse return .{ .strokes = 0, .bypassed = false };
    const e = &g.edits.items[idx];
    return .{ .strokes = e.strokes.items.len, .bypassed = e.bypassed };
}

fn toggleCurrentBypass() void {
    lockData();
    if (frameEditIndex(g.cur)) |idx| {
        g.edits.items[idx].bypassed = !g.edits.items[idx].bypassed;
    }
    g.data_mu.unlock();
    g.edit_gen +%= 1;
    saveProject();
}

/// One brush dab into an 8-bit mask (0 = keep result, 255 = show original).
fn stampDab(mask: []u8, w: i32, h: i32, cx: f32, cy: f32, radius: f32, hard: f32, reveal: bool) void {
    const opac = std.math.clamp(g.studio_opac, 0.01, 1.0);
    const inner = radius * hard;
    const falloff = 1.0 / @max(0.0001, radius - inner);

    const x0 = @max(0, @as(i32, @intFromFloat(@floor(cx - radius))));
    const y0 = @max(0, @as(i32, @intFromFloat(@floor(cy - radius))));
    const x1 = @min(w - 1, @as(i32, @intFromFloat(@floor(cx + radius))));
    const y1 = @min(h - 1, @as(i32, @intFromFloat(@floor(cy + radius))));
    if (x1 < x0 or y1 < y0) return;

    const wu: usize = @intCast(w);
    var yy = y0;
    while (yy <= y1) : (yy += 1) {
        const row = mask[@as(usize, @intCast(yy)) * wu ..];
        const dy = @as(f32, @floatFromInt(yy)) + 0.5 - cy;
        var xx = x0;
        while (xx <= x1) : (xx += 1) {
            const dx = @as(f32, @floatFromInt(xx)) + 0.5 - cx;
            const d = @sqrt(dx * dx + dy * dy);
            var t: f32 = 1.0;
            if (d > inner) t = 1.0 - (d - inner) * falloff;
            if (t <= 0.0) continue;
            const a = opac * t;
            const cur: f32 = @floatFromInt(row[@intCast(xx)]);
            const nv: f32 = if (reveal) cur + (255.0 - cur) * a else cur * (1.0 - a);
            row[@intCast(xx)] = @intFromFloat(std.math.clamp(nv, 0.0, 255.0));
        }
    }
}

/// Stamp one polyline stroke into a mask. Stroke points are in source-frame
/// pixels; `sx`/`sy` scale them into the mask's own resolution.
fn stampStroke(mask: []u8, w: i32, h: i32, st: *const Stroke, sx: f32, sy: f32, hard: f32) void {
    const pts = st.pts.items;
    if (pts.len == 0) return;
    const radius = @max(0.6, st.radius * 0.5 * sx);
    const spacing = @max(1.0, radius * 0.35);

    var px = pts[0].x * sx;
    var py = pts[0].y * sy;
    stampDab(mask, w, h, px, py, radius, hard, st.reveal);
    for (pts[1..]) |p| {
        const x = p.x * sx;
        const y = p.y * sy;
        const dx = x - px;
        const dy = y - py;
        const dist = @sqrt(dx * dx + dy * dy);
        const steps: u32 = @intFromFloat(@max(1.0, @ceil(dist / spacing)));
        var k: u32 = 1;
        while (k <= steps) : (k += 1) {
            const t = @as(f32, @floatFromInt(k)) / @as(f32, @floatFromInt(steps));
            stampDab(mask, w, h, px + dx * t, py + dy * t, radius, hard, st.reveal);
        }
        px = x;
        py = y;
    }
}

/// Build the retouch mask for `edit` at `w`x`h`, mapping source pixels through
/// `sx`/`sy`. Caller holds `data_mu`. Returns a freshly allocated buffer.
fn buildEditMaskLocked(edit: *const FrameEdit, w: i32, h: i32, sx: f32, sy: f32) ?[]u8 {
    if (w <= 0 or h <= 0) return null;
    const mask = alloc.alloc(u8, @as(usize, @intCast(w)) * @as(usize, @intCast(h))) catch return null;
    @memset(mask, 0);
    const hard = std.math.clamp(g.studio_hard, 0.0, 0.98);
    for (edit.strokes.items) |*st| stampStroke(mask, w, h, st, sx, sy, hard);
    return mask;
}

/// Blend the frame's retouch (if any) into `img`. `orig` must be at `img`'s
/// resolution; the strokes are in `src_w`x`src_h` pixels.
fn applyEditToImage(img: *ff.Image, orig: *const ff.Image, src_w: i32, src_h: i32, frame: i64) void {
    if (src_w <= 0 or src_h <= 0) return;
    lockData();
    const idx = frameEditIndex(frame) orelse {
        g.data_mu.unlock();
        return;
    };
    const edit = &g.edits.items[idx];
    if (edit.bypassed or edit.strokes.items.len == 0) {
        g.data_mu.unlock();
        return;
    }
    const sx = @as(f32, @floatFromInt(img.width)) / @as(f32, @floatFromInt(src_w));
    const sy = @as(f32, @floatFromInt(img.height)) / @as(f32, @floatFromInt(src_h));
    const mask = buildEditMaskLocked(edit, img.width, img.height, sx, sy) orelse {
        g.data_mu.unlock();
        return;
    };
    g.data_mu.unlock();
    defer alloc.free(mask);
    ff.blendMask(img, orig, mask, img.width, img.height) catch {};
}

/// Frame boxes for `frame`, preferring the per-frame sample and falling back to
/// its sequence. Caller holds `data_mu`.
fn boxesForFrameLocked(frame: i64, out: *[max_faces][4]f32) usize {
    var lo: usize = 0;
    var hi: usize = g.samples.items.len;
    while (lo < hi) {
        const mid = (lo + hi) / 2;
        if (g.samples.items[mid].f < frame) lo = mid + 1 else hi = mid;
    }
    var smp: ?Sample = null;
    if (lo < g.samples.items.len and g.samples.items[lo].f == frame) smp = g.samples.items[lo];
    if (smp == null and lo > 0 and g.samples.items[lo - 1].f == frame) smp = g.samples.items[lo - 1];
    if (smp) |s| {
        out.* = s.b;
        return s.n;
    }
    for (g.segments.items) |seg| {
        if (frame >= seg.start and frame <= seg.end) {
            out.* = seg.boxes;
            return seg.count;
        }
    }
    return 0;
}

/// The face a retouch most likely belongs to: the one nearest the stroke
/// centroid. Caller holds `data_mu`.
fn referenceFaceIndex(edit: *const FrameEdit, boxes: *const [max_faces][4]f32, count: usize) ?usize {
    if (count == 0) return null;
    var cx: f32 = 0;
    var cy: f32 = 0;
    var n: f32 = 0;
    for (edit.strokes.items) |st| {
        for (st.pts.items) |p| {
            cx += p.x;
            cy += p.y;
            n += 1;
        }
    }
    if (n == 0) return null;
    cx /= n;
    cy /= n;
    var best: ?usize = null;
    var best_d: f32 = std.math.floatMax(f32);
    for (0..count) |k| {
        const b = boxes[k];
        const bx = b[0] + b[2] * 0.5;
        const by = b[1] + b[3] * 0.5;
        const d = (bx - cx) * (bx - cx) + (by - cy) * (by - cy);
        if (d < best_d) {
            best_d = d;
            best = k;
        }
    }
    return best;
}

/// Position/size similarity between two boxes; smaller is closer. Faces in a
/// clearly different spot or size get a large value so they are not matched.
fn faceDistance(a: [4]f32, b: [4]f32) f32 {
    const aw = @max(1.0, a[2]);
    const ah = @max(1.0, a[3]);
    const bw = @max(1.0, b[2]);
    const bh = @max(1.0, b[3]);
    const acx = a[0] + aw * 0.5;
    const acy = a[1] + ah * 0.5;
    const bcx = b[0] + bw * 0.5;
    const bcy = b[1] + bh * 0.5;
    const dsize = @abs(bw - aw) / aw + @abs(bh - ah) / ah;
    const dpos = (@abs(bcx - acx) + @abs(bcy - acy)) / @max(aw, ah);
    return dsize + dpos;
}

/// How many frames a copy would overwrite because they already carry an edit.
fn countCopyConflicts() usize {
    lockData();
    defer g.data_mu.unlock();
    const src_idx = frameEditIndex(g.cur) orelse return 0;
    if (g.edits.items[src_idx].strokes.items.len == 0) return 0;
    const seg_idx = segmentAt(g.cur);
    if (seg_idx < 0) return 0;
    const seg = g.segments.items[@intCast(seg_idx)];

    var rboxes: [max_faces][4]f32 = @splat(@splat(0));
    const rcount = boxesForFrameLocked(g.cur, &rboxes);
    const rf = referenceFaceIndex(&g.edits.items[src_idx], &rboxes, rcount) orelse return 0;
    const rb = rboxes[rf];

    var n: usize = 0;
    var f = seg.start;
    while (f <= seg.end) : (f += 1) {
        if (f == g.cur) continue;
        var tboxes: [max_faces][4]f32 = @splat(@splat(0));
        const tcount = boxesForFrameLocked(f, &tboxes);
        if (tcount == 0) continue;
        var best: ?usize = null;
        var best_d: f32 = std.math.floatMax(f32);
        for (0..tcount) |k| {
            const d = faceDistance(rb, tboxes[k]);
            if (d < best_d) {
                best_d = d;
                best = k;
            }
        }
        const k = best orelse continue;
        if (faceDistance(rb, tboxes[k]) > 1.7) continue;
        if (frameEditIndex(f)) |ti| {
            if (g.edits.items[ti].strokes.items.len > 0) n += 1;
        }
    }
    return n;
}

/// Copy the current frame's retouch onto the matching face of every other frame
/// in the sequence, transforming each stroke by the target face's offset and
/// scale. `overwrite` replaces existing edits on those frames. Returns the
/// number of frames updated.
fn copyEditToSimilar(overwrite: bool) usize {
    lockData();
    defer g.data_mu.unlock();
    const src_idx = frameEditIndex(g.cur) orelse return 0;
    if (g.edits.items[src_idx].strokes.items.len == 0) return 0;
    const seg_idx = segmentAt(g.cur);
    if (seg_idx < 0) return 0;
    const seg = g.segments.items[@intCast(seg_idx)];

    var rboxes: [max_faces][4]f32 = @splat(@splat(0));
    const rcount = boxesForFrameLocked(g.cur, &rboxes);
    const rf = referenceFaceIndex(&g.edits.items[src_idx], &rboxes, rcount) orelse return 0;
    const rb = rboxes[rf];
    const rw = @max(1.0, rb[2]);
    const rh = @max(1.0, rb[3]);
    const rcx = rb[0] + rw * 0.5;
    const rcy = rb[1] + rh * 0.5;

    var applied: usize = 0;
    var f = seg.start;
    while (f <= seg.end) : (f += 1) {
        if (f == g.cur) continue;
        var tboxes: [max_faces][4]f32 = @splat(@splat(0));
        const tcount = boxesForFrameLocked(f, &tboxes);
        if (tcount == 0) continue;
        var best: ?usize = null;
        var best_d: f32 = std.math.floatMax(f32);
        for (0..tcount) |k| {
            const d = faceDistance(rb, tboxes[k]);
            if (d < best_d) {
                best_d = d;
                best = k;
            }
        }
        const k = best orelse continue;
        const tb = tboxes[k];
        if (faceDistance(rb, tb) > 1.7) continue; // reject faces in another position

        // Existing edit on the target?
        const existing = frameEditIndex(f);
        if (existing) |ei| {
            if (g.edits.items[ei].strokes.items.len > 0 and !overwrite) continue;
        }

        const tw = @max(1.0, tb[2]);
        const th = @max(1.0, tb[3]);
        const tcx = tb[0] + tw * 0.5;
        const tcy = tb[1] + th * 0.5;
        const sx = tw / rw;
        const sy = th / rh;

        // Create or clear the target slot. Any append to `edits` can move the
        // backing storage, so all pointers are re-fetched afterwards.
        var tidx: usize = undefined;
        if (existing) |ei| {
            tidx = ei;
            const tgt = &g.edits.items[tidx];
            if (overwrite and tgt.strokes.items.len > 0) {
                for (tgt.strokes.items) |*st| st.pts.deinit(alloc);
                tgt.strokes.clearRetainingCapacity();
                tgt.bypassed = false;
            }
        } else {
            if (g.edits.items.len >= 4096) continue;
            g.edits.append(alloc, .{ .f = f }) catch continue;
            tidx = g.edits.items.len - 1;
        }

        const ref = &g.edits.items[src_idx];
        const tgt = &g.edits.items[tidx];
        for (ref.strokes.items) |st| {
            var ns = Stroke{ .radius = st.radius, .reveal = st.reveal };
            for (st.pts.items) |p| {
                ns.pts.append(alloc, .{ .x = tcx + (p.x - rcx) * sx, .y = tcy + (p.y - rcy) * sy }) catch {};
            }
            tgt.strokes.append(alloc, ns) catch {
                ns.pts.deinit(alloc);
            };
        }
        applied += 1;
    }
    return applied;
}

// --- Studio painting -------------------------------------------------------

/// Map a physical mouse position to source-frame pixels, or null when outside
/// the displayed image.
fn studioToSource(p: dvui.Point.Physical, r: dvui.Rect.Physical) ?[2]f32 {
    if (r.w <= 0 or r.h <= 0) return null;
    const nx = (p.x - r.x) / r.w;
    const ny = (p.y - r.y) / r.h;
    if (nx < 0 or ny < 0 or nx > 1 or ny > 1) return null;
    return .{
        nx * @as(f32, @floatFromInt(g.info.width)),
        ny * @as(f32, @floatFromInt(g.info.height)),
    };
}

fn studioBegin(sx: f32, sy: f32) void {
    if (!g.studio) return;
    lockData();
    if (ensureEditLocked(g.cur)) |idx| {
        const e = &g.edits.items[idx];
        e.bypassed = false;
        if (e.strokes.items.len < 512) {
            e.strokes.append(alloc, .{ .radius = g.studio_size, .reveal = g.studio_erase }) catch {};
            if (e.strokes.items.len > 0) {
                const st = &e.strokes.items[e.strokes.items.len - 1];
                st.pts.append(alloc, .{ .x = sx, .y = sy }) catch {};
                g.stroke_active = true;
                g.stroke_last_x = sx;
                g.stroke_last_y = sy;
            }
        }
    }
    g.data_mu.unlock();
    g.edit_gen +%= 1;
}

fn studioTo(sx: f32, sy: f32) void {
    if (!g.stroke_active) return;
    const min_d = @max(2.0, g.studio_size * 0.2);
    const dx = sx - g.stroke_last_x;
    const dy = sy - g.stroke_last_y;
    if (dx * dx + dy * dy < min_d * min_d) return;
    lockData();
    if (frameEditIndex(g.cur)) |idx| {
        const e = &g.edits.items[idx];
        if (e.strokes.items.len > 0) {
            const st = &e.strokes.items[e.strokes.items.len - 1];
            st.pts.append(alloc, .{ .x = sx, .y = sy }) catch {};
            g.stroke_last_x = sx;
            g.stroke_last_y = sy;
        }
    }
    g.data_mu.unlock();
    g.edit_gen +%= 1;
}

fn studioEnd() void {
    if (!g.stroke_active) return;
    g.stroke_active = false;
    saveProject();
}

/// Handle mouse input for Studio painting over the viewer, and draw the brush
/// ring. `r` is the physical rect the frame is drawn into.
fn studioEvents(view: anytype, crs: anytype, r: dvui.Rect.Physical) void {
    const evts = dvui.events();
    for (evts) |*e| {
        if (!dvui.eventMatch(e, .{ .id = view.data().id, .r = crs.r })) continue;
        switch (e.evt) {
            .mouse => |me| switch (me.action) {
                .press => {
                    if (me.button == .right) {
                        g.studio_erase = !g.studio_erase;
                        setStatusFmt("Brush: {s}", .{if (g.studio_erase) "erase (reveal original)" else "restore result"}, false);
                        e.handle(@src(), view.data());
                    } else if (me.button == .left) {
                        if (studioToSource(me.p, r)) |p| {
                            studioBegin(p[0], p[1]);
                            dvui.captureMouse(view.data(), e.num);
                        }
                        e.handle(@src(), view.data());
                    }
                },
                .motion => {
                    if (g.stroke_active and dvui.captured(view.data().id)) {
                        if (studioToSource(me.p, r)) |p| studioTo(p[0], p[1]);
                        dvui.cursorSet(.crosshair);
                        e.handle(@src(), view.data());
                    }
                },
                .release => {
                    if (me.button == .left and g.stroke_active) {
                        studioEnd();
                        dvui.captureMouse(null, e.num);
                        e.handle(@src(), view.data());
                    }
                },
                .position => {
                    g.studio_hover = me.p;
                    dvui.cursorSet(.crosshair);
                },
                else => {},
            },
            else => {},
        }
    }

    // Brush ring sized in source pixels.
    if (g.studio_hover) |hp| {
        if (g.info.width > 0) {
            const rad = @max(1.0, (g.studio_size * 0.5 / @as(f32, @floatFromInt(g.info.width))) * r.w);
            const col: dvui.Color = if (g.studio_erase)
                .{ .r = 0xff, .g = 0x6b, .b = 0x5e }
            else
                .{ .r = 0x3e, .g = 0xcf, .b = 0x70 };
            var pb = dvui.Path.Builder.init(alloc);
            defer pb.deinit();
            pb.addArc(.{ .x = hp.x, .y = hp.y }, rad, std.math.pi * 2.0, 0.0, false);
            dvui.Path.stroke(pb.build(), .{ .thickness = 1.5, .color = .{ .color = col } });
            fillRect(.{ .x = hp.x - 1.0, .y = hp.y - 1.0, .w = 2.0, .h = 2.0 }, col);
        }
    }
}

// ---------------------------------------------------------------------------
// Project file
// ---------------------------------------------------------------------------

fn configuredTempDir() []const u8 {
    if (!g.host_ready) return "";
    return g.host.temp_dir(g.host.user);
}

fn configuredExportDir() []const u8 {
    if (!g.host_ready) return "";
    return g.host.export_dir(g.host.user);
}

/// Recompute the cache root for the current video. Used when the Temp folder
/// changes in Settings while a video is open, without touching the project or
/// the export path.
pub fn refreshPaths() void {
    if (g.path_len == 0) return;
    setCacheRootFor(pathSlice(&g.path, g.path_len));
}

fn setCacheRootFor(video_path: []const u8) void {
    const name = baseName(video_path);
    const stem = stripExt(name);
    const dir_len = video_path.len - name.len;

    var cbuf: [1024]u8 = @splat(0);
    const configured = configuredTempDir();
    if (configured.len > 0) {
        // One cache per video: the stem keeps it readable and the hash avoids
        // collisions between same-named files from different folders.
        const hash = std.hash.Wyhash.hash(0, video_path);
        var nbuf: [512]u8 = undefined;
        const sub = std.fmt.bufPrint(&nbuf, "{s}-{x:0>8}.faceforge.cache", .{ stem, hash & 0xffff_ffff }) catch return;
        const root = joinZ(&cbuf, &.{ configured, sub }) catch return;
        copyPath(&g.cache_root, &g.cache_root_len, root);
    } else {
        const root = joinZ(&cbuf, &.{ video_path[0..dir_len], ".faceforge.cache" }) catch return;
        copyPath(&g.cache_root, &g.cache_root_len, root);
    }
}

fn projectPathFor(video_path: []const u8) void {
    var buf: [1024]u8 = @splat(0);
    const name = baseName(video_path);
    const stem = stripExt(name);
    // Keep the project next to the video, named after it.
    const dir_len = video_path.len - name.len;
    var n: usize = 0;
    @memcpy(buf[0..dir_len], video_path[0..dir_len]);
    n = dir_len;
    const suffix = ".faceforge.json";
    const take = @min(stem.len, buf.len - n - suffix.len - 2);
    @memcpy(buf[n .. n + take], stem[0..take]);
    n += take;
    @memcpy(buf[n .. n + suffix.len], suffix);
    n += suffix.len;
    buf[n] = 0;
    copyPath(&g.proj_path, &g.proj_path_len, buf[0..n]);

    setCacheRootFor(video_path);

    // Default export: <export_dir or video_dir>/<stem>_faceforge.mp4.
    const export_root = configuredExportDir();
    const out_dir = if (export_root.len > 0) export_root else video_path[0..dir_len];
    var nbuf: [512]u8 = undefined;
    if (std.fmt.bufPrint(&nbuf, "{s}_faceforge.mp4", .{stem})) |out_name| {
        var obuf: [1024]u8 = @splat(0);
        if (joinZ(&obuf, &.{ out_dir, out_name })) |out| {
            copyPath(&g.job_out, &g.job_out_len, out);
        } else |_| {}
    } else |_| {}
}

pub fn saveProject() void {
    if (g.proj_path_len == 0 or !g.open) return;
    const text = blk: {
        lockData();
        defer g.data_mu.unlock();
        break :blk std.json.Stringify.valueAlloc(alloc, .{
            .version = @as(u32, 1),
            .video = pathSlice(&g.path, g.path_len),
            .in_frame = g.in_frame,
            .out_frame = g.out_frame,
            .chunk = g.chunk,
            .stride = g.stride,
            .default_skip = g.default_skip,
            .samples = g.samples.items,
            .chunks = g.chunks.items,
            .assigns = g.assigns.items,
            .overrides = g.overrides.items,
            .edits = g.edits.items,
        }, .{}) catch return;
    };
    defer alloc.free(text);
    if (!ff.writeFile(zPath(&g.proj_path, g.proj_path_len), text)) {
        setErr("Cannot write the project file");
    }
}

fn loadProject() bool {
    if (g.proj_path_len == 0) return false;
    const text = ff.readFile(zPath(&g.proj_path, g.proj_path_len), alloc) orelse return false;
    defer alloc.free(text);
    var parsed = std.json.parseFromSlice(ProjectJson, alloc, text, .{ .ignore_unknown_fields = true }) catch return false;
    defer parsed.deinit();
    const p = parsed.value;

    lockData();
    g.samples.clearRetainingCapacity();
    g.chunks.clearRetainingCapacity();
    g.assigns.clearRetainingCapacity();
    g.overrides.clearRetainingCapacity();
    for (p.samples) |s| {
        var smp = Sample{ .f = s.f, .n = @min(s.n, max_faces) };
        const n = @min(s.b.len, max_faces);
        for (0..n) |i| smp.b[i] = s.b[i];
        g.samples.append(alloc, smp) catch {};
    }
    for (p.chunks) |c| {
        g.chunks.append(alloc, .{ .start = c.start, .end = c.end, .done = c.done }) catch {};
    }
    for (p.assigns) |a| {
        var as: Assign = .{ .a = a.a, .b = a.b };
        for (0..@min(a.ids.len, max_faces)) |i| as.ids[i] = a.ids[i];
        g.assigns.append(alloc, as) catch {};
    }
    for (p.overrides) |o| {
        var ov: Override = .{ .f = o.f };
        for (0..@min(o.ids.len, max_faces)) |i| ov.ids[i] = o.ids[i];
        g.overrides.append(alloc, ov) catch {};
    }
    for (g.edits.items) |*e| freeFrameEdit(e);
    g.edits.clearRetainingCapacity();
    for (p.edits) |je| {
        var fe = FrameEdit{ .f = je.f, .bypassed = je.bypassed };
        for (je.strokes) |js| {
            var st = Stroke{ .radius = js.radius, .reveal = js.reveal };
            for (js.pts) |jp| st.pts.append(alloc, .{ .x = jp.x, .y = jp.y }) catch {};
            fe.strokes.append(alloc, st) catch {
                st.pts.deinit(alloc);
            };
        }
        g.edits.append(alloc, fe) catch {
            freeFrameEdit(&fe);
        };
    }
    g.data_mu.unlock();
    g.edit_gen +%= 1;

    if (p.in_frame >= 0) g.in_frame = @min(p.in_frame, if (g.info.frames > 0) g.info.frames - 1 else p.in_frame);
    if (p.out_frame > 0) g.out_frame = p.out_frame;
    if (p.chunk >= 1) g.chunk = @min(p.chunk, 100000);
    if (p.stride >= 1) g.stride = @min(p.stride, 240);
    g.default_skip = p.default_skip;
    return true;
}

// ---------------------------------------------------------------------------
// Open / close
// ---------------------------------------------------------------------------

pub fn setAutotest(path: []const u8) void {
    copyPath(&g.test_path, &g.test_path_len, path);
    g.test_keep = std.c.getenv("FACEFORGE_VIDEOTEST_KEEP") != null;
    g.test_state = .start;
}

/// Install the host callbacks before the first draw, so opening a video from
/// the CLI can already resolve its cache/export folders.
pub fn setHost(host: Host) void {
    g.host = host;
    g.host_ready = true;
}

pub fn testDone() bool {
    return g.test_done;
}

fn autotestStep() void {
    switch (g.test_state) {
        .off, .done => return,
        .start => {
            if (g.host.engine_busy(g.host.user)) return;
            if (g.host.identity_count(g.host.user) == 0) return;
            if (g.test_path_len == 0) {
                g.test_state = .done;
                return;
            }
            std.debug.print("[videotest] opening {s}\n", .{g.test_path[0..g.test_path_len]});
            openPath(g.test_path[0..g.test_path_len]);
            g.in_frame = 0;
            g.out_frame = @min(@as(i64, 12), @max(0, g.info.frames - 1));
            startAnalyze();
            g.test_state = .wait_analyze;
        },
        .wait_analyze => {
            if (g.job != .none) return;
            std.debug.print("[videotest] analysis done: samples={d} segments={d} faces0={d}\n", .{
                g.samples.items.len,                                            g.segments.items.len,
                if (g.segments.items.len > 0) g.segments.items[0].count else 0,
            });
            for (&g.seg_ids) |*v| v.* = -1;
            startProcessRange(0, @min(@as(i64, 4), g.out_frame));
            g.test_state = .wait_process;
        },
        .wait_process => {
            if (g.job != .none) return;
            std.debug.print("[videotest] processed range done, exporting\n", .{});
            copyPath(&g.job_out, &g.job_out_len, "/tmp/opencode/vtest/gui_out.mp4");
            g.job_from = 0;
            g.job_to = @min(@as(i64, 4), g.out_frame);
            g.job_audio = true;
            g.job_chunks = true;
            if (!spawn(.encode)) {
                std.debug.print("[videotest] FAILED to start export\n", .{});
                g.test_state = .done;
                return;
            }
            g.test_state = .wait_export;
        },
        .wait_export => {
            if (g.job != .none) return;
            const exists = ff.fileExists(zPath(&g.job_out, g.job_out_len));
            std.debug.print("[videotest] {s}: export {s}\n", .{
                if (exists) "OK" else "FAILED",
                pathSlice(&g.job_out, g.job_out_len),
            });
            // Playback is driven by the app loop alone: no input events are
            // synthesised here, so this proves the frame keeps repainting on its
            // own. It also checks that "Sequence" playback stays inside bounds.
            if (g.segments.items.len > 0) {
                const start = g.segments.items[0].start;
                g.sel_seg = 0;
                g.play_sequence = true;
                g.loop_play = true;
                seekTo(start);
                selectSegmentAt(g.cur);
                startPlayback();
                g.test_play_moves = 0;
                g.test_play_last = g.cur;
                g.test_play_budget = 900;
                g.test_state = .wait_play;
                return;
            }
            testFinish();
        },
        .wait_play => {
            const seg = g.segments.items[0];
            if (g.cur < seg.start or g.cur > seg.end) {
                std.debug.print("[videotest] FAILED: playback left sequence bounds (cur={d}, seg={d}-{d})\n", .{ g.cur, seg.start, seg.end });
                stopPlayback();
                g.play_sequence = false;
                testFinish();
                return;
            }
            if (g.cur != g.test_play_last) {
                g.test_play_moves += 1;
                g.test_play_last = g.cur;
            }
            g.test_play_budget -= 1;
            if (g.test_play_moves < 3 and g.test_play_budget > 0) return;
            const ok = g.test_play_moves >= 3;
            std.debug.print("[videotest] {s}: playback advanced {d} frame(s) inside sequence {d}-{d}\n", .{
                if (ok) "OK" else "FAILED",
                g.test_play_moves,
                seg.start,
                seg.end,
            });
            stopPlayback();
            g.play_sequence = false;
            testFinish();
        },
    }
}

fn testFinish() void {
    if (g.test_keep) {
        seekTo(@min(@as(i64, 3), g.job_to));
        g.test_state = .done;
    } else {
        g.test_state = .done;
        g.test_done = true;
    }
}

pub fn openDialog() void {
    const chosen = dvui.dialogNativeFileOpen(alloc, .{
        .title = "Open video",
        .filters = &.{ "*.mp4", "*.mov", "*.mkv", "*.webm", "*.avi", "*.m4v" },
        .filter_description = "Videos",
    }) catch null;
    if (chosen) |path| {
        defer alloc.free(path);
        openPath(path);
    }
}

pub fn openPath(path: []const u8) void {
    closeVideo();
    if (path.len == 0) return;
    copyPath(&g.path, &g.path_len, path);

    var dec = ff.Video.open(zPath(&g.path, g.path_len)) catch {
        setErr("Cannot open this video. Check the file and the codec.");
        return;
    };
    const info = dec.info();
    if (info.width <= 0 or info.height <= 0) {
        dec.deinit();
        setErr("The video has no decodable video stream.");
        return;
    }
    g.decoder = dec;
    g.info = info;
    g.open = true;
    g.in_frame = 0;
    g.out_frame = if (info.frames > 0) info.frames - 1 else 0;
    g.cur = 0;
    g.cur_loaded = -1;
    g.playing = false;
    g.sel_seg = -1;
    // Keep the current Low/High choice across videos: silently forcing Low
    // quality on every open made it look like the checkbox could not be turned
    // off.

    const name = baseName(path);
    const n = @min(name.len, g.name.len);
    @memcpy(g.name[0..n], name[0..n]);
    g.name_len = n;

    projectPathFor(path);
    rebuildSegments();
    if (ff.fileExists(zPath(&g.proj_path, g.proj_path_len))) {
        _ = loadProject();
        rebuildSegments();
        g.chunk_f = @floatFromInt(g.chunk);
        g.stride_f = @floatFromInt(g.stride);
        setStatusFmt("Loaded {s} and its project", .{g.name[0..g.name_len]}, false);
    } else {
        setStatusFmt("{s} · {d}×{d} · {d:.2} fps", .{ g.name[0..g.name_len], info.width, info.height, info.fps }, false);
    }
    g.err_len = 0;
    seekTo(g.cur);
    selectSegmentAt(g.cur);
}

fn closeVideo() void {
    stopPlayback();
    if (g.job != .none) {
        g.cancel.store(true, .release);
        if (g.thread) |t| {
            t.join();
            g.thread = null;
        }
        g.job = .none;
    }
    if (g.decoder) |*d| d.deinit();
    g.decoder = null;
    freeFaceThumbs();
    freeImage(&g.cur_img);
    freeImage(&g.res_img);
    freeImage(&g.res_base);
    freeTex(&g.cur_tex);
    freeTex(&g.res_tex);
    g.cur_loaded = -1;
    g.res_frame = -1;
    g.res_edit_gen = 0xffff_ffff_ffff_ffff;
    g.studio = false;
    g.stroke_active = false;
    g.copy_pending = false;
    g.open = false;
    g.path_len = 0;
    g.proj_path_len = 0;
    g.cache_root_len = 0;
    g.info = .{};
    g.in_frame = 0;
    g.out_frame = 0;
    g.cur = 0;
    g.playing = false;
    lockData();
    g.samples.clearRetainingCapacity();
    g.chunks.clearRetainingCapacity();
    g.assigns.clearRetainingCapacity();
    g.overrides.clearRetainingCapacity();
    g.segments.clearRetainingCapacity();
    for (g.edits.items) |*e| freeFrameEdit(e);
    g.edits.clearRetainingCapacity();
    g.data_mu.unlock();
    g.edit_gen +%= 1;
    g.sel_seg = -1;
}

pub fn isOpen() bool {
    return g.open;
}

pub fn hasProject() bool {
    return g.proj_path_len > 0 and ff.fileExists(zPath(&g.proj_path, g.proj_path_len));
}

/// True while a video job (analysis / processing / export) is running.
pub fn active() bool {
    return g.job != .none;
}

pub fn deinit() void {
    g.shutting_down = true;
    closeVideo();
    lockData();
    g.samples.deinit(alloc);
    g.chunks.deinit(alloc);
    g.assigns.deinit(alloc);
    g.overrides.deinit(alloc);
    g.segments.deinit(alloc);
    g.edits.deinit(alloc);
    g.data_mu.unlock();
    g.inited = false;
}

// ---------------------------------------------------------------------------
// Sequences
// ---------------------------------------------------------------------------

fn iou(a: *const [4]f32, b: *const [4]f32) f32 {
    const ax0 = a[0];
    const ay0 = a[1];
    const ax1 = a[0] + a[2];
    const ay1 = a[1] + a[3];
    const bx0 = b[0];
    const by0 = b[1];
    const bx1 = b[0] + b[2];
    const by1 = b[1] + b[3];
    const ix = @max(0.0, @min(ax1, bx1) - @max(ax0, bx0));
    const iy = @max(0.0, @min(ay1, by1) - @max(ay0, by0));
    const inter = ix * iy;
    const union_a = @max(0.0, ax1 - ax0) * @max(0.0, ay1 - ay0) +
        @max(0.0, bx1 - bx0) * @max(0.0, by1 - by0) - inter;
    if (union_a <= 0.0001) return 0.0;
    return inter / union_a;
}

fn closeBoxes(a: *const [max_faces][4]f32, b: *const [max_faces][4]f32, count: usize) bool {
    for (0..count) |i| {
        if (iou(&a[i], &b[i]) >= 0.18) continue;
        // A small drift is fine when boxes are similar in size.
        const aw = @max(a[i][2], 1.0);
        const ah = @max(a[i][3], 1.0);
        const dx = (a[i][0] + a[i][2] * 0.5) - (b[i][0] + b[i][2] * 0.5);
        const dy = (a[i][1] + a[i][3] * 0.5) - (b[i][1] + b[i][3] * 0.5);
        const dist = @sqrt(dx * dx + dy * dy);
        if (dist > @max(aw, ah) * 0.9) return false;
    }
    return true;
}

/// Rebuild the editable sequences. Consecutive samples with the same face count
/// and stable positions form one sequence. When analysis produces too many
/// sequences for a usable list, neighbouring ones are merged (the assignment is
/// still per face index, which is stable across a shot).
fn rebuildSegments() void {
    g.face_thumbs_stale = true;
    lockData();
    defer g.data_mu.unlock();
    g.segments.clearRetainingCapacity();
    const items = g.samples.items;
    if (items.len == 0) return;

    var i: usize = 0;
    while (i < items.len) {
        const s = items[i];
        if (s.n == 0) {
            i += 1;
            continue;
        }
        var seg = Segment{ .start = s.f, .end = s.f, .count = s.n, .boxes = s.b };
        var j = i + 1;
        while (j < items.len) : (j += 1) {
            const t = items[j];
            if (t.n != seg.count) break;
            if (t.f - seg.end > g.stride * 3) break;
            if (!closeBoxes(&seg.boxes, &t.b, seg.count)) break;
            seg.end = t.f;
        }
        if (g.segments.items.len < max_segments) {
            g.segments.append(alloc, seg) catch {};
        } else if (g.segments.items.len > 0) {
            // Coarsen: extend the last sequence instead of dropping data.
            g.segments.items[g.segments.items.len - 1].end = seg.end;
        }
        i = j;
    }

    // If the list is still huge (e.g. crowded scenes), merge consecutive
    // sequences until it fits; the user can still override single frames.
    while (g.segments.items.len > max_segments) {
        var best: usize = 0;
        var best_gap: i64 = std.math.maxInt(i64);
        for (0..g.segments.items.len - 1) |k| {
            if (g.segments.items[k].count != g.segments.items[k + 1].count) continue;
            const gap = g.segments.items[k + 1].start - g.segments.items[k].end;
            if (gap < best_gap) {
                best_gap = gap;
                best = k;
            }
        }
        if (best_gap == std.math.maxInt(i64)) break;
        g.segments.items[best].end = g.segments.items[best + 1].end;
        _ = g.segments.orderedRemove(best + 1);
    }
}

fn segmentAt(frame: i64) i32 {
    for (g.segments.items, 0..) |s, i| {
        if (frame >= s.start and frame <= s.end) return @intCast(i);
    }
    return -1;
}

fn selectSegmentAt(frame: i64) void {
    const idx = segmentAt(frame);
    g.sel_seg = idx;
    if (idx >= 0) {
        const s = g.segments.items[@intCast(idx)];
        var ids: [max_faces]i32 = @splat(-1);
        idsFor(frame, s.count, &ids);
        g.seg_ids = ids;
    }
}

fn sampleAt(frame: i64) ?Sample {
    lockData();
    defer g.data_mu.unlock();
    // Samples are ordered by frame; binary search then scan neighbours.
    var lo: usize = 0;
    var hi: usize = g.samples.items.len;
    while (lo < hi) {
        const mid = (lo + hi) / 2;
        if (g.samples.items[mid].f < frame) lo = mid + 1 else hi = mid;
    }
    if (lo < g.samples.items.len and g.samples.items[lo].f == frame) return g.samples.items[lo];
    if (lo > 0 and g.samples.items[lo - 1].f == frame) return g.samples.items[lo - 1];
    return null;
}

// ---------------------------------------------------------------------------
// Cache paths
// ---------------------------------------------------------------------------

fn cacheDirInto(q: Quality, sig: u64, buf: []u8) ?[:0]const u8 {
    var qbuf: [16]u8 = @splat(0);
    const qs = switch (q) {
        .low => "low",
        .high => "high",
    };
    const sig_s = std.fmt.bufPrint(&qbuf, "{x:0>16}", .{sig}) catch return null;
    return std.mem.printSentinel(buf, "{s}/{s}/{s}", .{
        pathSlice(&g.cache_root, g.cache_root_len),
        sig_s,
        qs,
    }, 0) catch null;
}

fn framePathInto(q: Quality, sig: u64, frame: i64, buf: []u8) ?[:0]const u8 {
    var qbuf: [16]u8 = @splat(0);
    const qs = switch (q) {
        .low => "low",
        .high => "high",
    };
    const sig_s = std.fmt.bufPrint(&qbuf, "{x:0>16}", .{sig}) catch return null;
    return std.mem.printSentinel(buf, "{s}/{s}/{s}/f{d:0>8}.jpg", .{
        pathSlice(&g.cache_root, g.cache_root_len),
        sig_s,
        qs,
        frame,
    }, 0) catch null;
}

fn cachedFrame(frame: i64, sig: u64) ?ff.Image {
    // Prefer the high-quality frame: low and high share the same signature, so
    // after a high-quality pass both files exist and returning low first made
    // the viewer look like high quality had no effect.
    for ([_]Quality{ .high, .low }) |q| {
        var pbuf: [1024]u8 = @splat(0);
        const p = framePathInto(q, sig, frame, &pbuf) orelse continue;
        if (ff.fileExists(p)) {
            return ff.Image.load(p) catch null;
        }
    }
    return null;
}

// ---------------------------------------------------------------------------
// Decode / preview
// ---------------------------------------------------------------------------

fn freeImage(slot: *?ff.Image) void {
    if (slot.*) |*im| im.deinit();
    slot.* = null;
}

fn freeTex(slot: *?dvui.Texture) void {
    // During shutdown the DVUI window is already gone; textures die with it.
    if (g.shutting_down) {
        slot.* = null;
        return;
    }
    if (slot.*) |tex| dvui.textureDestroyLater(tex);
    slot.* = null;
}

fn toPma(img: *const ff.Image) ?[]dvui.Color.PMA {
    const w: usize = @intCast(img.width);
    const h: usize = @intCast(img.height);
    const pma = alloc.alloc(dvui.Color.PMA, w * h) catch return null;
    const stride: usize = @intCast(img.stride);
    for (0..h) |y| {
        const row = img.data[y * stride ..];
        for (0..w) |x| {
            pma[y * w + x] = .{
                .r = row[x * 3 + 2],
                .g = row[x * 3 + 1],
                .b = row[x * 3 + 0],
                .a = 255,
            };
        }
    }
    return pma;
}

fn upload(slot: *?dvui.Texture, img: *const ff.Image) void {
    freeTex(slot);
    const pma = toPma(img) orelse return;
    defer alloc.free(pma);
    slot.* = dvui.textureCreate(pma, .{
        .width = @intCast(img.width),
        .height = @intCast(img.height),
        .interpolation = .linear,
    }) catch null;
}

fn freeFaceThumbs() void {
    for (&g.face_thumbs) |*t| {
        freeImage(&t.img);
        freeTex(&t.tex);
        t.dirty = false;
    }
    g.face_thumb_seg = -1;
}

/// Crop a padded square around a detected face and scale it down for the
/// assignment rows. Boxes are `[x, y, w, h]` in source-frame pixels, matching
/// `Sample.b` / `Segment.boxes`.
fn makeFaceThumb(img: *const ff.Image, box: [4]f32) !ff.Image {
    const bw = @max(1.0, box[2]);
    const bh = @max(1.0, box[3]);
    const cx = box[0] + bw * 0.5;
    const cy = box[1] + bh * 0.5;
    // A little headroom so the crop reads as a face rather than eyes/nose.
    const side = @max(bw, bh) * 1.35;
    const iw: f32 = @floatFromInt(img.width);
    const ih: f32 = @floatFromInt(img.height);
    const x0 = std.math.clamp(cx - side * 0.5, 0, @max(0, iw - 1));
    const y0 = std.math.clamp(cy - side * 0.5, 0, @max(0, ih - 1));
    const x1 = std.math.clamp(x0 + side, 1, iw);
    const y1 = std.math.clamp(y0 + side, 1, ih);
    const rx: i32 = @intFromFloat(x0);
    const ry: i32 = @intFromFloat(y0);
    const rw: i32 = @max(1, @as(i32, @intFromFloat(x1 - x0)));
    const rh: i32 = @max(1, @as(i32, @intFromFloat(y1 - y0)));
    return img.resizeRegion(rx, ry, rw, rh, 72, 72);
}

/// Build the face crops for the selected sequence, once per selection. Uses the
/// decoder only for this single frame; the displayed image is kept intact.
fn ensureFaceThumbs() void {
    if (g.face_thumbs_stale) {
        freeFaceThumbs();
        g.face_thumbs_stale = false;
    }
    const seg_idx = g.sel_seg;
    if (seg_idx < 0 or @as(usize, @intCast(seg_idx)) >= g.segments.items.len) {
        if (g.face_thumb_seg != -1) freeFaceThumbs();
        return;
    }
    if (g.face_thumb_seg == seg_idx) return;

    const seg = g.segments.items[@intCast(seg_idx)];
    freeFaceThumbs();
    g.face_thumb_seg = seg_idx;

    const dec = &(g.decoder orelse return);
    var frame = dec.frame(seg.start) catch return;
    defer frame.deinit();
    const n = @min(@as(usize, seg.count), max_faces);
    for (0..n) |k| {
        g.face_thumbs[k].img = makeFaceThumb(&frame, seg.boxes[k]) catch continue;
        g.face_thumbs[k].dirty = true;
    }
}

fn uploadFaceThumbs() void {
    for (&g.face_thumbs) |*t| {
        if (!t.dirty) continue;
        t.dirty = false;
        if (t.img) |*im| upload(&t.tex, im);
    }
}

fn seekTo(frame: i64) void {
    if (!g.open) return;
    const max_frame = if (g.info.frames > 0) g.info.frames - 1 else std.math.maxInt(i64);
    g.cur = std.math.clamp(frame, 0, @max(0, max_frame));
    g.res_frame = -1;
}

fn ensureFrame() void {
    if (!g.open or g.cur_loaded == g.cur) return;
    const dec = &(g.decoder orelse return);
    freeImage(&g.cur_img);
    freeTex(&g.cur_tex);
    g.cur_img = dec.frame(g.cur) catch null;
    if (g.cur_img) |*im| upload(&g.cur_tex, im);
    g.cur_loaded = g.cur;
}

fn ensureResult() void {
    if (!g.open) return;
    const sig = cacheSig();
    if (g.res_frame == g.cur and g.res_sig == sig and g.res_edit_gen == g.edit_gen) return;
    freeImage(&g.res_base);
    freeImage(&g.res_img);
    freeTex(&g.res_tex);
    g.res_frame = g.cur;
    g.res_sig = sig;
    if (g.res_missing_sig != sig) {
        g.res_missing_sig = sig;
    }
    if (cachedFrame(g.cur, sig)) |im| {
        g.res_base = im;
        rebuildEditedResult();
    } else {
        g.res_edit_gen = g.edit_gen;
    }
}

/// Rebuild the displayed result from `res_base`, blending in this frame's
/// retouch. When there is no active edit the base is shown untouched (moved
/// over, not copied).
fn rebuildEditedResult() void {
    freeImage(&g.res_img);
    freeTex(&g.res_tex);
    if (g.res_base == null) {
        g.res_edit_gen = g.edit_gen;
        return;
    }
    if (!hasActiveEdit(g.cur)) {
        g.res_img = g.res_base;
        g.res_base = null;
        if (g.res_img) |*im| upload(&g.res_tex, im);
        g.res_edit_gen = g.edit_gen;
        return;
    }
    const base = &g.res_base.?;
    var disp = base.clone() catch {
        g.res_edit_gen = g.edit_gen;
        return;
    };
    ensureFrame();
    if (g.cur_img) |*src| {
        var owned: ?ff.Image = null;
        defer if (owned) |*o| o.deinit();
        var orig: *const ff.Image = src;
        if (src.width != disp.width or src.height != disp.height) {
            owned = src.resized(disp.width, disp.height) catch null;
            if (owned) |*o| orig = o;
        }
        applyEditToImage(&disp, orig, src.width, src.height, g.cur);
    }
    g.res_img = disp;
    if (g.res_img) |*im| upload(&g.res_tex, im);
    g.res_edit_gen = g.edit_gen;
}

/// Playback bounds: the selected sequence when "Sequence" is on, otherwise the
/// analysis in/out range.
fn playBounds() struct { from: i64, to: i64 } {
    if (g.play_sequence and g.sel_seg >= 0 and
        @as(usize, @intCast(g.sel_seg)) < g.segments.items.len)
    {
        const seg = g.segments.items[@intCast(g.sel_seg)];
        return .{ .from = seg.start, .to = seg.end };
    }
    return .{ .from = g.in_frame, .to = g.out_frame };
}

fn startPlayback() void {
    if (!g.open or g.playing) return;
    const b = playBounds();
    if (g.cur < b.from or g.cur >= b.to) g.cur = b.from;
    g.playing = true;
    g.play_t0 = ff.nowNs();
    g.play_from = g.cur;
}

fn stopPlayback() void {
    g.playing = false;
}

fn advancePlayback() void {
    if (!g.playing or !g.open) return;
    // Ask for another frame for as long as playback is on. Without this the loop
    // would go idle between frame deadlines (nothing else requests a redraw) and
    // playback would only advance while the mouse moved.
    dvui.refresh(null, @src(), null);
    const b = playBounds();
    const fps = if (g.info.fps > 0) g.info.fps else 25.0;
    const dt = @as(f64, @floatFromInt(ff.nowNs() - g.play_t0)) / 1e9;
    const target = g.play_from + @as(i64, @intFromFloat(@floor(dt * fps)));
    if (target <= g.cur) return;
    if (target > b.to) {
        if (g.loop_play) {
            g.play_from = b.from;
            g.play_t0 = ff.nowNs();
            seekTo(b.from);
        } else {
            stopPlayback();
        }
        return;
    }
    // Skip frames if decoding cannot keep up, but never fall behind.
    seekTo(target);
    if (g.playing) {
        ensureFrame();
        ensureResult();
        selectSegmentAt(g.cur);
    }
}

// ---------------------------------------------------------------------------
// Frame processing
// ---------------------------------------------------------------------------

const RenderResult = struct {
    img: ff.Image,
    /// Scale from the rendered resolution back to the source resolution.
    scale: f32 = 1.0,
};

fn renderFrame(engine: *ff.Engine, src: *const ff.Image, frame: i64, quality: Quality) !RenderResult {
    const long: f32 = @floatFromInt(@max(src.width, src.height));
    const downscale = quality == .low and long > 640.0;
    const f = if (downscale) 640.0 / long else 1.0;
    const ww: i32 = @max(1, @as(i32, @intFromFloat(@as(f32, @floatFromInt(src.width)) * f)));
    const wh: i32 = @max(1, @as(i32, @intFromFloat(@as(f32, @floatFromInt(src.height)) * f)));

    var work_owned: ?ff.Image = null;
    defer if (work_owned) |*im| im.deinit();
    var work: ff.Image = undefined;
    if (downscale) {
        work_owned = try src.resized(ww, wh);
        work = work_owned.?;
    } else {
        work = .{ .data = src.data, .width = src.width, .height = src.height, .stride = src.stride };
    }

    var faces: [max_faces]ff.Face = undefined;
    const n = engine.detect(&work, faces[0..]) catch 0;
    var ids: [max_faces]i32 = @splat(-1);
    idsFor(frame, n, &ids);

    const bytes = @as(usize, @intCast(work.width)) * @as(usize, @intCast(work.height)) * 3;
    const ping = try alloc.alloc(u8, bytes);
    defer alloc.free(ping);
    const pong = try alloc.alloc(u8, bytes);
    defer alloc.free(pong);
    @memcpy(ping, work.data[0..bytes]);

    var cur = ff.Image{ .data = ping.ptr, .width = work.width, .height = work.height, .stride = work.width * 3 };
    var next = ff.Image{ .data = pong.ptr, .width = work.width, .height = work.height, .stride = work.width * 3 };

    const id_count = g.host.identity_count(g.host.user);
    const def = g.host.default_identity(g.host.user);
    var act_faces: [max_faces]ff.Face = undefined;
    var act_n: usize = 0;
    const mouth = g.host.restore_mouth(g.host.user);

    for (0..n) |i| {
        const chosen = ids[i];
        // Skip: keep this face as it appears in the source frame while the
        // other faces in the same frame are still swapped.
        if (chosen == skip_id) continue;
        var id: usize = undefined;
        if (chosen >= 0) {
            id = @intCast(chosen);
        } else if (def < id_count) {
            id = def;
        } else {
            continue;
        }
        if (id >= id_count) continue;
        engine.setRestoreMouth(mouth);
        var matrix: [6]f64 = @splat(0);
        var patch = engine.swapPatch(&cur, g.host.identity_embedding(g.host.user, id), faces[i], &matrix) catch continue;
        defer patch.deinit();
        ff.pastePatch(&cur, &patch, &matrix, &next) catch continue;
        std.mem.swap(ff.Image, &cur, &next);
        act_faces[act_n] = faces[i];
        act_n += 1;
    }

    // GFPGAN works on each face's 512x512 crop, so its memory footprint does
    // not scale with the frame size. Attempt it whenever an enhancer is
    // selected; if a run really fails (a truly tiny GPU), the core splits the
    // frame per face and we fall back to the swapped frame for the rest of the
    // job instead of crashing. The output buffer is separate from `cur` so a
    // failed run cannot corrupt the frame.
    if (quality == .high and act_n > 0 and !g.enhancer_failed and g.host.enhancer(g.host.user) != 0) {
        engine.setEnhancer(g.host.enhancer(g.host.user));
        const ebuf = try alloc.alloc(u8, bytes);
        defer alloc.free(ebuf);
        var en: ff.Image = .{ .data = ebuf.ptr, .width = work.width, .height = work.height, .stride = work.width * 3 };
        const enh_ok = blk: {
            engine.enhance(&cur, act_faces[0..act_n], &en, null, null) catch break :blk false;
            break :blk true;
        };
        if (enh_ok) {
            @memcpy(cur.data[0..bytes], ebuf[0..bytes]);
        } else {
            // Keep the swapped frame and stop trying for the rest of the job.
            g.enhancer_failed = true;
            engine.setEnhancer(0);
        }
    } else {
        engine.setEnhancer(0);
    }

    const copy = alloc.dupe(u8, cur.data[0..bytes]) catch return error.OutOfMemory;
    return .{ .img = ff.Image.fromOwnedBuffer(copy.ptr, work.width, work.height), .scale = f };
}

fn processAndCache(engine: *ff.Engine, video: *ff.Video, frame: i64, quality: Quality, sig: u64, use_cache: bool) !void {
    if (use_cache) {
        var cbuf: [1024]u8 = @splat(0);
        if (framePathInto(quality, sig, frame, &cbuf)) |p| {
            if (ff.fileExists(p)) return;
        }
    }
    var src = video.frame(frame) catch return error.DecodeFailed;
    defer src.deinit();
    const r = renderFrame(engine, &src, frame, quality) catch return error.RenderFailed;
    var out = r.img;
    defer out.deinit();
    var pbuf: [1024]u8 = @splat(0);
    const p = framePathInto(quality, sig, frame, &pbuf) orelse return;
    out.save(p) catch return error.CacheWriteFailed;
}

fn ensureCacheDirs() void {
    if (g.cache_root_len == 0) return;
    _ = ff.mkdirs(zPath(&g.cache_root, g.cache_root_len));
    const sig = cacheSig();
    for ([_]Quality{ .low, .high }) |q| {
        var dbuf: [1024]u8 = @splat(0);
        if (cacheDirInto(q, sig, &dbuf)) |d| _ = ff.mkdirs(d);
    }
}

// ---------------------------------------------------------------------------
// Jobs
// ---------------------------------------------------------------------------

fn spawn(kind: JobKind) bool {
    if (!g.open) {
        setStatus("Open a video first", true);
        return false;
    }
    if (g.job != .none) {
        setStatus("A video job is already running", true);
        return false;
    }
    if (g.host.engine_busy(g.host.user)) {
        setStatus("Wait for the current photo job to finish", true);
        return false;
    }
    if (g.host.identity_count(g.host.user) == 0) {
        setStatus("Add an identity image first", true);
        return false;
    }
    ensureCacheDirs();
    g.job = kind;
    g.cancel.store(false, .release);
    g.done.store(false, .release);
    g.ok.store(false, .release);
    g.progress.store(0, .monotonic);
    g.job_done_frames.store(0, .monotonic);
    g.job_total_frames.store(1, .monotonic);
    g.job_msg_len = 0;
    g.err_len = 0;
    g.enhancer_failed = false;

    g.thread = std.Thread.spawn(.{}, jobMain, .{kind}) catch |e| {
        g.job = .none;
        setStatusFmt("Cannot start video worker: {s}", .{@errorName(e)}, true);
        return false;
    };
    return true;
}

fn setJobError(comptime fmt: []const u8, args: anytype) void {
    var buf: [256]u8 = undefined;
    const text = std.fmt.bufPrint(&buf, fmt, args) catch return;
    const n = @min(text.len, g.job_msg.len);
    @memcpy(g.job_msg[0..n], text[0..n]);
    g.job_msg_len = n;
}

fn jobMain(kind: JobKind) void {
    const ok = switch (kind) {
        .analyze => runAnalyze(),
        .process => runProcess(),
        .encode => runExport(),
        .none => true,
    };
    g.ok.store(ok, .release);
    g.done.store(true, .release);
    dvui.refresh(null, @src(), null);
}

fn openJobVideo() ?ff.Video {
    return ff.Video.open(zPath(&g.path, g.path_len)) catch {
        setJobError("Cannot reopen the video", .{});
        return null;
    };
}

fn mergeSamples(batch: []const Sample, from: i64, to: i64) void {
    lockData();
    defer g.data_mu.unlock();
    var i: usize = 0;
    while (i < g.samples.items.len) {
        if (g.samples.items[i].f >= from and g.samples.items[i].f <= to) {
            _ = g.samples.orderedRemove(i);
        } else {
            i += 1;
        }
    }
    // Keep the list sorted by frame.
    for (batch) |s| {
        var lo: usize = 0;
        var hi: usize = g.samples.items.len;
        while (lo < hi) {
            const mid = (lo + hi) / 2;
            if (g.samples.items[mid].f < s.f) lo = mid + 1 else hi = mid;
        }
        g.samples.insert(alloc, lo, s) catch {};
    }
}

fn chunkDone(start: i64, end: i64) bool {
    lockData();
    defer g.data_mu.unlock();
    for (g.chunks.items) |c| {
        if (c.start <= start and c.end >= end) return c.done;
    }
    return false;
}

fn markChunk(start: i64, end: i64) void {
    lockData();
    defer g.data_mu.unlock();
    for (g.chunks.items) |*c| {
        if (c.start == start and c.end == end) {
            c.done = true;
            return;
        }
    }
    if (g.chunks.items.len < max_chunks) {
        g.chunks.append(alloc, .{ .start = start, .end = end, .done = true }) catch {};
    }
}

fn runAnalyze() bool {
    var video = openJobVideo() orelse return false;
    defer video.deinit();
    const engine = g.host.ensure_engine_seq(g.host.user, true) orelse {
        setJobError("Cannot load the models", .{});
        return false;
    };
    var total: u32 = 0;
    var f = g.job_from;
    while (f <= g.job_to) : (f += g.chunk) total += 1;
    g.job_total_frames.store(@max(1, total), .monotonic);
    var done_chunks: u32 = 0;

    var batch: std.ArrayListUnmanaged(Sample) = .empty;
    defer batch.deinit(alloc);

    var start = g.job_from;
    while (start <= g.job_to and !g.cancel.load(.acquire)) : (start += g.chunk) {
        const end = @min(start + g.chunk - 1, g.job_to);
        if (chunkDone(start, end)) {
            done_chunks += 1;
            g.job_done_frames.store(done_chunks, .monotonic);
            continue;
        }
        batch.clearRetainingCapacity();
        var fi = start;
        while (fi <= end and !g.cancel.load(.acquire)) : (fi += g.stride) {
            var src = video.frame(fi) catch {
                // Seek failure should not kill the whole analysis.
                continue;
            };
            defer src.deinit();
            var faces: [max_faces]ff.Face = undefined;
            const n = engine.detect(&src, faces[0..]) catch 0;
            var smp = Sample{ .f = fi, .n = @intCast(@min(n, max_faces)) };
            for (0..smp.n) |k| {
                smp.b[k] = .{
                    faces[k].bbox[0],                    faces[k].bbox[1],
                    faces[k].bbox[2] - faces[k].bbox[0], faces[k].bbox[3] - faces[k].bbox[1],
                };
            }
            batch.append(alloc, smp) catch {};
        }
        if (batch.items.len > 0) {
            mergeSamples(batch.items, start, end);
            // Persist finished work right away: an interrupted analysis loses
            // at most the chunk that was in flight.
            saveProject();
        }
        if (!g.cancel.load(.acquire)) {
            markChunk(start, end);
            done_chunks += 1;
            g.job_done_frames.store(done_chunks, .monotonic);
            g.progress.store(@intCast(@min(1000, done_chunks * 1000 / @max(1, total))), .monotonic);
        }
    }
    return true;
}

fn runProcess() bool {
    var video = openJobVideo() orelse return false;
    defer video.deinit();
    const engine = g.host.ensure_engine_seq(g.host.user, true) orelse {
        setJobError("Cannot load the models", .{});
        return false;
    };
    const q: Quality = if (g.job_quality == .low) .low else .high;
    const sig = cacheSig();
    const total: u32 = @intCast(@max(1, g.job_to - g.job_from + 1));
    g.job_total_frames.store(total, .monotonic);
    var done: u32 = 0;
    var f = g.job_from;
    while (f <= g.job_to and !g.cancel.load(.acquire)) : (f += 1) {
        processAndCache(engine, &video, f, q, sig, g.job_use_cache) catch {};
        done += 1;
        g.job_done_frames.store(done, .monotonic);
        g.progress.store(@intCast(@min(1000, done * 1000 / total)), .monotonic);
        if ((done & 7) == 0) dvui.refresh(null, @src(), null);
    }
    g.res_frame = -1;
    return true;
}

fn runExport() bool {
    var video = openJobVideo() orelse return false;
    defer video.deinit();
    const engine = g.host.ensure_engine_seq(g.host.user, true) orelse {
        setJobError("Cannot load the models", .{});
        return false;
    };
    const out = zPath(&g.job_out, g.job_out_len);
    const sig = cacheSig();
    const w = video.info().width;
    const h = video.info().height;
    const fps = if (g.info.fps > 0) g.info.fps else 25.0;
    const total: u32 = @intCast(@max(1, g.job_to - g.job_from + 1));
    g.job_total_frames.store(total, .monotonic);

    var tmp_buf: [1024]u8 = @splat(0);
    const video_out = if (g.job_audio) blk: {
        // Keep the video-only intermediate in the configured temp folder when
        // there is one, so exports do not leave stray files next to the result.
        if (configuredTempDir().len > 0 and g.cache_root_len > 0) {
            ensureCacheDirs();
            const p = std.mem.printSentinel(&tmp_buf, "{s}/mux_video.mp4", .{
                pathSlice(&g.cache_root, g.cache_root_len),
            }, 0) catch return false;
            break :blk p;
        }
        const p = std.mem.printSentinel(&tmp_buf, "{s}.video.mp4", .{out}, 0) catch return false;
        break :blk p;
    } else out;

    var writer: ?ff.VideoWriter = null;
    defer if (writer) |*wr| wr.finish() catch {};

    if (g.job_chunks) {
        ensureCacheDirs();
        // Encode parts into the cache and concatenate losslessly afterwards.
        var part_index: usize = 0;
        var f = g.job_from;
        while (f <= g.job_to and !g.cancel.load(.acquire)) {
            var pbuf: [1024]u8 = @splat(0);
            const part_path = std.mem.printSentinel(&pbuf, "{s}/part_{d:0>4}.mp4", .{
                pathSlice(&g.cache_root, g.cache_root_len), part_index,
            }, 0) catch return false;
            writer = ff.VideoWriter.open(part_path, w, h, fps, "libx264", 16, "veryfast", "ffmpeg") catch null;
            if (writer == null) {
                setJobError("Cannot start the video encoder (ffmpeg)", .{});
                return false;
            }
            const part_end = @min(f + g.chunk - 1, g.job_to);
            var done: u32 = 0;
            while (f <= part_end and !g.cancel.load(.acquire)) : (f += 1) {
                var frame_img = renderExportFrame(engine, &video, f, sig) orelse continue;
                defer frame_img.deinit();
                if (frame_img.width != w or frame_img.height != h) {
                    const fixed = frame_img.resized(w, h) catch continue;
                    frame_img.deinit();
                    frame_img = fixed;
                }
                writer.?.write(&frame_img) catch {
                    setJobError("Encoder rejected frame {d}", .{f});
                    return false;
                };
                done += 1;
                g.job_done_frames.store(@intCast(f - g.job_from + 1), .monotonic);
                g.progress.store(@intCast(@min(1000, @divTrunc(@as(i64, f - g.job_from + 1) * 1000, @as(i64, total)))), .monotonic);
                if ((done & 7) == 0) dvui.refresh(null, @src(), null);
            }
            writer.?.finish() catch {
                setJobError("ffmpeg failed while writing part {d}", .{part_index});
                return false;
            };
            writer = null;
            part_index += 1;
        }
        if (g.cancel.load(.acquire)) return false;

        var list_buf: [2048]u8 = @splat(0);
        var list_len: usize = 0;
        var k: usize = 0;
        while (k < part_index) : (k += 1) {
            var pbuf: [1024]u8 = @splat(0);
            const part = std.fmt.bufPrint(&pbuf, "{s}/part_{d:0>4}.mp4", .{
                pathSlice(&g.cache_root, g.cache_root_len), k,
            }) catch return false;
            const line = std.fmt.bufPrint(list_buf[list_len..], "file '{s}'\n", .{part}) catch return false;
            list_len += line.len;
        }
        var list_path_buf: [1024]u8 = @splat(0);
        const list_path = std.mem.printSentinel(&list_path_buf, "{s}/concat.txt", .{
            pathSlice(&g.cache_root, g.cache_root_len),
        }, 0) catch return false;
        if (!ff.writeFile(list_path, list_buf[0..list_len])) {
            setJobError("Cannot write the concat list", .{});
            return false;
        }
        const video_only = if (g.job_audio) video_out else out;
        ff.concatVideos(list_path, video_only, "ffmpeg") catch {
            setJobError("Cannot concatenate the exported parts", .{});
            return false;
        };
        if (g.job_audio) {
            const start_sec = @as(f64, @floatFromInt(g.job_from)) / fps;
            const dur = @as(f64, @floatFromInt(g.job_to - g.job_from + 1)) / fps;
            ff.muxAudio(video_only, zPath(&g.path, g.path_len), start_sec, dur, out, "ffmpeg") catch {
                setJobError("Video written, but muxing the audio failed", .{});
                return false;
            };
            ff.removeFile(video_only);
        }
        return true;
    }

    writer = ff.VideoWriter.open(video_out, w, h, fps, "libx264", 16, "veryfast", "ffmpeg") catch null;
    if (writer == null) {
        setJobError("Cannot start the video encoder (ffmpeg)", .{});
        return false;
    }
    var done: u32 = 0;
    var f = g.job_from;
    while (f <= g.job_to and !g.cancel.load(.acquire)) : (f += 1) {
        var frame_img = renderExportFrame(engine, &video, f, sig) orelse continue;
        defer frame_img.deinit();
        if (frame_img.width != w or frame_img.height != h) {
            const fixed = frame_img.resized(w, h) catch continue;
            frame_img.deinit();
            frame_img = fixed;
        }
        writer.?.write(&frame_img) catch {
            setJobError("Encoder rejected frame {d}", .{f});
            return false;
        };
        done += 1;
        g.job_done_frames.store(done, .monotonic);
        g.progress.store(@intCast(@min(1000, done * 1000 / total)), .monotonic);
        if ((done & 7) == 0) dvui.refresh(null, @src(), null);
    }
    writer.?.finish() catch {
        setJobError("ffmpeg failed while encoding", .{});
        return false;
    };
    writer = null;

    if (g.cancel.load(.acquire)) return false;
    if (g.job_audio) {
        const start_sec = @as(f64, @floatFromInt(g.job_from)) / fps;
        const dur = @as(f64, @floatFromInt(g.job_to - g.job_from + 1)) / fps;
        ff.muxAudio(video_out, zPath(&g.path, g.path_len), start_sec, dur, out, "ffmpeg") catch {
            setJobError("Video written, but muxing the audio failed", .{});
            return false;
        };
        ff.removeFile(video_out);
    }
    return true;
}

fn renderExportFrame(engine: *ff.Engine, video: *ff.Video, frame: i64, sig: u64) ?ff.Image {
    // Reuse the high-quality cache when it matches the current signature.
    var cbuf: [1024]u8 = @splat(0);
    var img: ?ff.Image = null;
    if (framePathInto(.high, sig, frame, &cbuf)) |p| {
        if (ff.fileExists(p)) {
            if (ff.Image.load(p)) |im| img = im else |_| {}
        }
    }
    if (img == null) {
        var src = video.frame(frame) catch return null;
        defer src.deinit();
        const r = renderFrame(engine, &src, frame, .high) catch return null;
        img = r.img;
    }
    // Stamp any manual retouch; needs the pristine frame, so decode it again
    // only when this frame actually carries an active edit.
    if (hasActiveEdit(frame)) {
        if (img) |*im| {
            if (video.frame(frame)) |src_val| {
                var src = src_val;
                defer src.deinit();
                var owned: ?ff.Image = null;
                defer if (owned) |*o| o.deinit();
                var orig: *const ff.Image = &src;
                if (src.width != im.width or src.height != im.height) {
                    owned = src.resized(im.width, im.height) catch null;
                    if (owned) |*o| orig = o;
                }
                applyEditToImage(im, orig, src.width, src.height, frame);
            } else |_| {}
        }
    }
    return img;
}

// ---------------------------------------------------------------------------
// Job polling
// ---------------------------------------------------------------------------

fn poll() void {
    advancePlayback();
    if (g.thread == null) return;
    if (!g.done.load(.acquire)) {
        dvui.refresh(null, @src(), null);
        return;
    }
    const t = g.thread.?;
    g.thread = null;
    t.join();
    const ok = g.ok.load(.acquire);
    const cancelled = g.cancel.load(.acquire);
    const kind = g.job;
    g.job = .none;
    g.done.store(false, .release);

    if (kind == .analyze) {
        rebuildSegments();
        selectSegmentAt(g.cur);
    }
    if (kind == .process or kind == .encode) {
        g.res_frame = -1;
        ensureResult();
    }
    if (!ok) {
        const msg = g.job_msg[0..g.job_msg_len];
        setStatus(if (msg.len > 0) msg else "Video job failed", true);
    } else if (cancelled) {
        setStatus("Cancelled · finished work was saved", false);
    } else {
        switch (kind) {
            .analyze => setStatusFmt("Analysis finished · {d} sequence(s)", .{g.segments.items.len}, false),
            .process => setStatus("Frames processed and cached", false),
            .encode => setStatusFmt("Exported {s}", .{baseName(pathSlice(&g.job_out, g.job_out_len))}, false),
            .none => {},
        }
    }
    saveProject();
    g.cancel.store(false, .release);
}

fn startAnalyze() void {
    g.job_from = g.in_frame;
    g.job_to = g.out_frame;
    _ = spawn(.analyze);
}

fn startProcessFrame() void {
    g.job_from = g.cur;
    g.job_to = g.cur;
    g.job_quality = if (g.low_quality) .low else .high;
    g.job_use_cache = g.process_use_cache;
    _ = spawn(.process);
}

fn startProcessRange(from: i64, to: i64) void {
    g.job_from = std.math.clamp(from, 0, g.out_frame);
    g.job_to = std.math.clamp(to, g.job_from, g.out_frame);
    g.job_quality = if (g.low_quality) .low else .high;
    g.job_use_cache = g.process_use_cache;
    _ = spawn(.process);
}

fn exportDialog(whole: bool) void {
    if (!g.open) return;
    const chosen = dvui.dialogNativeFileSave(alloc, .{
        .title = "Export video",
        .path = if (g.job_out_len > 0) pathSlice(&g.job_out, g.job_out_len) else "",
        .filters = &.{ "*.mp4", "*.mkv", "*.mov" },
        .filter_description = "Video",
    }) catch null;
    if (chosen) |path| {
        defer alloc.free(path);
        copyPath(&g.job_out, &g.job_out_len, path);
        g.export_whole = whole;
        g.job_from = if (whole) 0 else g.in_frame;
        g.job_to = if (whole) @max(0, g.info.frames - 1) else g.out_frame;
        g.job_audio = g.export_audio;
        g.job_chunks = g.export_chunks;
        if (spawn(.encode)) setStatus("Exporting…", false);
    }
}

// ---------------------------------------------------------------------------
// Workspace
// ---------------------------------------------------------------------------

pub fn draw(host: Host) void {
    if (!g.inited) {
        g.host = host;
        g.host_ready = true;
        g.inited = true;
    }
    g.host = host;
    g.host_ready = true;
    g.chunk = chunkI();
    g.stride = @max(1, @as(i64, @intFromFloat(@round(g.stride_f))));
    poll();
    autotestStep();
    // A requested close only runs once the worker has stopped, so it never
    // blocks the UI thread.
    if (g.close_requested and g.job == .none) {
        g.close_requested = false;
        closeVideo();
    }
    if (!g.open) {
        drawEmpty();
        return;
    }
    ensureFrame();
    ensureResult();

    var body = dvui.box(@src(), .{ .dir = .horizontal }, .{ .expand = .both });
    defer body.deinit();
    drawLeftPanel();
    drawCenter();
    drawRightPanel();
}

fn drawEmpty() void {
    var body = dvui.box(@src(), .{ .dir = .vertical }, .{
        .expand = .both,
        .padding = .all(32),
    });
    defer body.deinit();
    _ = dvui.spacer(@src(), .{ .expand = .vertical });
    dvui.label(@src(), "Video workspace", .{}, .{ .font = .theme(.title), .gravity_x = 0.5 });
    dvui.label(@src(), "Chunked face detection, per-sequence identity assignment, cached processing and ffmpeg export.", .{}, .{
        .gravity_x = 0.5,
        .margin = .{ .y = 8, .h = 8 },
        .color_text = .{ .color = .{ .r = 0x9a, .g = 0xa3, .b = 0xb0 } },
    });
    {
        var row = dvui.box(@src(), .{ .dir = .horizontal }, .{ .gravity_x = 0.5 });
        defer row.deinit();
        if (dvui.button(@src(), "Open video…", .{}, .{ .min_size_content = .{ .w = 160, .h = 32 } })) openDialog();
        if (dvui.button(@src(), "Open project…", .{}, .{ .min_size_content = .{ .w = 160, .h = 32 } })) {
            // Projects live next to their video; let the user pick the JSON
            // and load the video it references.
            const chosen = dvui.dialogNativeFileOpen(alloc, .{
                .title = "Open video project",
                .filters = &.{"*.faceforge.json"},
                .filter_description = "FaceForge video project",
            }) catch null;
            if (chosen) |p| {
                defer alloc.free(p);
                const zp = alloc.dupeSentinel(u8, p, 0) catch return;
                defer alloc.free(zp);
                if (ff.readFile(zp, alloc)) |text| {
                    defer alloc.free(text);
                    var parsed = std.json.parseFromSlice(ProjectJson, alloc, text, .{ .ignore_unknown_fields = true }) catch null;
                    if (parsed) |*pr| {
                        defer pr.deinit();
                        if (pr.value.video.len > 0) openPath(pr.value.video);
                    }
                }
            }
        }
    }
    if (g.status_len > 0) {
        dvui.labelNoFmt(@src(), g.statusSlice(), .{}, .{ .gravity_x = 0.5, .margin = .{ .y = 12, .h = 0 } });
    }
    if (g.err_len > 0) {
        dvui.labelNoFmt(@src(), g.errSlice(), .{}, .{
            .gravity_x = 0.5,
            .margin = .{ .y = 8, .h = 0 },
            .color_text = .{ .color = .{ .r = 0xf0, .g = 0x8a, .b = 0x8a } },
        });
    }
    dvui.label(@src(), "Tip: drop a video anywhere in the window.", .{}, .{
        .gravity_x = 0.5,
        .margin = .{ .y = 16, .h = 0 },
        .color_text = .{ .color = .{ .r = 0x74, .g = 0x7d, .b = 0x89 } },
    });
    _ = dvui.spacer(@src(), .{ .expand = .vertical });
}

fn drawLeftPanel() void {
    var panel = dvui.box(@src(), .{ .dir = .vertical }, .{
        .expand = .vertical,
        .min_size_content = .{ .w = 330 },
        .max_size_content = .{ .w = 360, .h = dvui.max_float_safe },
        .padding = .all(10),
        .background = true,
        .style = .window,
    });
    defer panel.deinit();

    var scroll = dvui.scrollArea(@src(), .{}, .{ .expand = .both });
    defer scroll.deinit();

    {
        var row = dvui.box(@src(), .{ .dir = .horizontal }, .{ .expand = .horizontal });
        defer row.deinit();
        var nbuf: [160]u8 = undefined;
        const shown = clipEllipsisInto(&nbuf, g.name[0..g.name_len], 26);
        dvui.label(@src(), "{s}", .{shown}, .{ .font = .theme(.heading), .gravity_y = 0.5 });
        _ = dvui.spacer(@src(), .{ .expand = .horizontal });
        if (dvui.button(@src(), "Close", .{}, .{})) {
            g.close_requested = true;
            g.cancel.store(true, .release);
        }
    }
    dvui.label(@src(), "{d}×{d} · {d:.2} fps · {s}", .{
        g.info.width,                                     g.info.height, g.info.fps,
        if (g.info.frames > 0) "indexed" else "streamed",
    }, .{ .color_text = .{ .color = .{ .r = 0x8b, .g = 0x93, .b = 0xa0 } } });
    if (g.info.frames > 0) {
        var tbuf: [32]u8 = undefined;
        dvui.label(@src(), "{d} frames · {s}", .{ g.info.frames, timeText(&tbuf, g.info.frames) }, .{
            .color_text = .{ .color = .{ .r = 0x8b, .g = 0x93, .b = 0xa0 } },
        });
    }

    _ = dvui.separator(@src(), .{ .expand = .horizontal, .margin = .{ .y = 6, .h = 2 } });

    // --- analysis ---------------------------------------------------------
    section("Analysis range");
    {
        var row = dvui.box(@src(), .{ .dir = .horizontal }, .{ .expand = .horizontal });
        defer row.deinit();
        dvui.label(@src(), "in {d} · out {d}", .{ g.in_frame, g.out_frame }, .{ .gravity_y = 0.5 });
        _ = dvui.spacer(@src(), .{ .expand = .horizontal });
        if (dvui.button(@src(), "Set in", .{}, .{})) {
            g.in_frame = g.cur;
            if (g.out_frame < g.in_frame) g.out_frame = g.in_frame;
        }
        if (dvui.button(@src(), "Set out", .{}, .{})) {
            g.out_frame = g.cur;
            if (g.in_frame > g.out_frame) g.in_frame = g.out_frame;
        }
    }
    {
        var row = dvui.box(@src(), .{ .dir = .horizontal }, .{ .expand = .horizontal });
        defer row.deinit();
        _ = dvui.sliderEntry(@src(), "Chunk {d}", .{
            .value = &g.chunk_f,
            .min = 30,
            .max = 3000,
            .interval = 30,
        }, .{ .expand = .horizontal });
    }
    {
        var row = dvui.box(@src(), .{ .dir = .horizontal }, .{ .expand = .horizontal });
        defer row.deinit();
        _ = dvui.sliderEntry(@src(), "Sample every {d}", .{
            .value = &g.stride_f,
            .min = 1,
            .max = 30,
            .interval = 1,
        }, .{ .expand = .horizontal });
    }
    {
        var row = dvui.box(@src(), .{ .dir = .horizontal }, .{ .expand = .horizontal });
        defer row.deinit();
        const busy = g.job != .none;
        if (dvui.button(@src(), "Analyze range", .{}, .{ .expand = .horizontal })) startAnalyze();
        if (dvui.button(@src(), "Cancel", .{}, .{})) g.cancel.store(true, .release);
        if (dvui.button(@src(), "Clear", .{}, .{})) {
            lockData();
            g.samples.clearRetainingCapacity();
            g.chunks.clearRetainingCapacity();
            g.segments.clearRetainingCapacity();
            g.data_mu.unlock();
            saveProject();
            setStatus("Analysis cleared", false);
        }
        _ = busy;
    }
    if (g.job == .analyze) {
        dvui.progress(@src(), .{ .percent = progressFrac() }, .{
            .expand = .horizontal,
            .min_size_content = .{ .h = 8 },
            .margin = .{ .y = 4, .h = 4 },
        });
        dvui.label(@src(), "analyzing {d}/{d} chunks", .{
            g.job_done_frames.load(.monotonic), g.job_total_frames.load(.monotonic),
        }, .{});
    } else {
        const done = doneChunks();
        dvui.label(@src(), "{d} samples · {d}/{d} chunks analyzed", .{ g.samples.items.len, done.done, done.total }, .{
            .color_text = .{ .color = .{ .r = 0x8b, .g = 0x93, .b = 0xa0 } },
        });
    }

    _ = dvui.separator(@src(), .{ .expand = .horizontal, .margin = .{ .y = 6, .h = 2 } });

    // --- sequences --------------------------------------------------------
    section("Sequences");
    dvui.label(@src(), "Grouped by face count and position. Pick one to assign identities once for the whole run.", .{}, .{
        .color_text = .{ .color = .{ .r = 0x8b, .g = 0x93, .b = 0xa0 } },
        .margin = .{ .y = 0, .h = 4 },
    });
    {
        var list = dvui.box(@src(), .{ .dir = .vertical }, .{
            .expand = .horizontal,
            .min_size_content = .{ .h = 120 },
            .max_size_content = .{ .w = dvui.max_float_safe, .h = 220 },
        });
        defer list.deinit();
        var sa = dvui.scrollArea(@src(), .{}, .{ .expand = .both });
        defer sa.deinit();
        if (g.segments.items.len == 0) {
            dvui.label(@src(), "No sequences yet — analyze a range.", .{}, .{});
        } else {
            for (g.segments.items, 0..) |seg, i| {
                var row = dvui.box(@src(), .{ .dir = .horizontal }, .{
                    .expand = .horizontal,
                    .id_extra = i,
                    .padding = .all(3),
                    .background = true,
                    .color_fill = .{ .color = if (@as(i32, @intCast(i)) == g.sel_seg)
                        dvui.Color{ .r = 0x2f, .g = 0x5a, .b = 0xa8 }
                    else
                        dvui.Color{ .r = 0x27, .g = 0x2b, .b = 0x32 } },
                    .margin = .{ .y = 1, .h = 1 },
                });
                defer row.deinit();
                if (widgetClicked(row.data())) {
                    g.sel_seg = @intCast(i);
                    seekTo(seg.start);
                    selectSegmentAt(seg.start);
                }
                dvui.label(@src(), "#{d}  {d}–{d}", .{ i + 1, seg.start, seg.end }, .{
                    .id_extra = i,
                    .gravity_y = 0.5,
                    .min_size_content = .{ .w = 130 },
                });
                dvui.label(@src(), "{d} face(s)", .{seg.count}, .{
                    .id_extra = i,
                    .gravity_y = 0.5,
                    .color_text = .{ .color = .{ .r = 0xc7, .g = 0xcd, .b = 0xd6 } },
                });
                // Manual Studio edits, bypassed ones included, so a sequence
                // with retouched frames is visible at a glance.
                const nedits = editsInRange(seg.start, seg.end);
                if (nedits > 0) {
                    var ebuf: [32]u8 = undefined;
                    const txt = std.fmt.bufPrint(&ebuf, "[{d} edit(s)]", .{nedits}) catch "";
                    dvui.labelNoFmt(@src(), txt, .{}, .{
                        .id_extra = i,
                        .gravity_y = 0.5,
                        .color_text = .{ .color = .{ .r = 0xe0, .g = 0xa0, .b = 0x4a } },
                    });
                }
            }
        }
    }

    if (g.sel_seg >= 0 and @as(usize, @intCast(g.sel_seg)) < g.segments.items.len) {
        const seg = g.segments.items[@intCast(g.sel_seg)];
        section("Assign identities");
        dvui.label(@src(), "Sequence {d}–{d} · {d} face(s)", .{ seg.start, seg.end, seg.count }, .{});
        ensureFaceThumbs();
        uploadFaceThumbs();
        var labels: [50][]const u8 = undefined;
        labels[0] = "(default)";
        const nid = @min(g.host.identity_count(g.host.user), 48);
        for (0..nid) |k| labels[k + 1] = g.host.identity_label(g.host.user, k);
        // Last entry leaves the face untouched, so one scene can mix swapped
        // and original faces.
        const skip_choice = nid + 1;
        labels[skip_choice] = "(leave unchanged)";
        for (0..seg.count) |k| {
            {
                var row = dvui.box(@src(), .{ .dir = .horizontal }, .{
                    .expand = .horizontal,
                    .id_extra = k,
                    .margin = .{ .x = 0, .y = 2, .w = 0, .h = 2 },
                });
                defer row.deinit();
                // Face crop tinted with the same colour as the viewer box, so
                // the numbered row is matched visually instead of by text.
                {
                    var cell = dvui.box(@src(), .{}, .{
                        .min_size_content = .{ .w = 40, .h = 40 },
                        .id_extra = k,
                        .border = .all(2),
                        .color_border = .{ .color = faceColor(k) },
                        .margin = .{ .x = 0, .y = 0, .w = 6, .h = 0 },
                    });
                    defer cell.deinit();
                    const crs = cell.data().contentRectScale();
                    if (g.face_thumbs[k].tex) |tex| {
                        var tw = crs.r.w;
                        var th = crs.r.h;
                        if (tex.width > 0 and tex.height > 0) {
                            const aspect = @as(f32, @floatFromInt(tex.width)) / @as(f32, @floatFromInt(tex.height));
                            if (tw / th > aspect) tw = th * aspect else th = tw / aspect;
                        }
                        const tr = dvui.Rect.Physical{
                            .x = crs.r.x + (crs.r.w - tw) * 0.5,
                            .y = crs.r.y + (crs.r.h - th) * 0.5,
                            .w = tw,
                            .h = th,
                        };
                        dvui.renderTexture(tex, .{ .r = tr, .s = crs.s }, .{ .corners = .all(3) }) catch {};
                    }
                }
                dvui.label(@src(), "Face {d}", .{k + 1}, .{
                    .gravity_y = 0.5,
                    .id_extra = k,
                    .min_size_content = .{ .w = 54 },
                    .color_text = .{ .color = faceColor(k) },
                });
                var choice: usize = if (g.seg_ids[k] == skip_id)
                    skip_choice
                else if (g.seg_ids[k] >= 0)
                    @as(usize, @intCast(g.seg_ids[k])) + 1
                else
                    0;
                if (dvui.dropdown(@src(), labels[0 .. nid + 2], .{ .choice = &choice }, .{}, .{
                    .expand = .horizontal,
                    .id_extra = k,
                })) {
                    g.seg_ids[k] = if (choice == 0)
                        -1
                    else if (choice == skip_choice)
                        skip_id
                    else
                        @intCast(choice - 1);
                }
                // Small preview of the chosen identity, so the dropdown is not
                // just a filename.
                {
                    var prev = dvui.box(@src(), .{}, .{
                        .min_size_content = .{ .w = 30, .h = 30 },
                        .id_extra = k,
                        .border = .all(1),
                        .color_border = .{ .color = .{ .r = 0x4a, .g = 0x50, .b = 0x5a } },
                        .margin = .{ .x = 6, .y = 0, .w = 0, .h = 0 },
                    });
                    defer prev.deinit();
                    const iid: ?usize = if (choice >= 1 and choice != skip_choice) choice - 1 else null;
                    if (iid) |ii| {
                        if (g.host.identity_thumb(g.host.user, ii)) |tex| {
                            const prs = prev.data().contentRectScale();
                            var tw = prs.r.w;
                            var th = prs.r.h;
                            if (tex.width > 0 and tex.height > 0) {
                                const aspect = @as(f32, @floatFromInt(tex.width)) / @as(f32, @floatFromInt(tex.height));
                                if (tw / th > aspect) tw = th * aspect else th = tw / aspect;
                            }
                            const tr = dvui.Rect.Physical{
                                .x = prs.r.x + (prs.r.w - tw) * 0.5,
                                .y = prs.r.y + (prs.r.h - th) * 0.5,
                                .w = tw,
                                .h = th,
                            };
                            dvui.renderTexture(tex, .{ .r = tr, .s = prs.s }, .{ .corners = .all(2) }) catch {};
                        }
                    }
                }
            }
        }
        {
            // Handy bulk actions: leave every face of this sequence untouched,
            // or point them all at the chosen default identity.
            var row = dvui.box(@src(), .{ .dir = .horizontal }, .{ .expand = .horizontal });
            defer row.deinit();
            if (dvui.button(@src(), "All faces: default", .{}, .{ .expand = .horizontal })) {
                for (0..seg.count) |k| g.seg_ids[k] = -1;
                setStatus("Sequence faces set to the default identity", false);
            }
            if (dvui.button(@src(), "All faces: skip", .{}, .{ .expand = .horizontal })) {
                for (0..seg.count) |k| g.seg_ids[k] = skip_id;
                setStatus("Sequence faces set to leave unchanged", false);
            }
        }
        {
            var row = dvui.box(@src(), .{ .dir = .horizontal }, .{ .expand = .horizontal });
            defer row.deinit();
            if (dvui.button(@src(), "Apply", .{}, .{ .expand = .horizontal })) {
                upsertAssign(seg.start, seg.end, &g.seg_ids);
                saveProject();
                setStatus("Assignment applied to the sequence", false);
            }
            if (dvui.button(@src(), "Forward", .{}, .{ .expand = .horizontal })) {
                upsertAssign(seg.start, g.out_frame, &g.seg_ids);
                saveProject();
                setStatus("Assignment propagated forward", false);
            }
            if (dvui.button(@src(), "This frame", .{}, .{ .expand = .horizontal })) {
                setOverride(g.cur, &g.seg_ids);
                saveProject();
                g.res_frame = -1;
                setStatus("Assignment applied to this frame only", false);
            }
        }
        {
            var row = dvui.box(@src(), .{ .dir = .horizontal }, .{ .expand = .horizontal });
            defer row.deinit();
            if (dvui.button(@src(), "Clear frame override", .{}, .{})) {
                clearOverride(g.cur);
                saveProject();
                g.res_frame = -1;
            }
        }
    }

    // --- studio retouch ---------------------------------------------------
    section("Studio (per-frame retouch)");
    {
        var studio = g.studio;
        if (dvui.checkbox(@src(), &studio, "Paint retouch on this frame", .{})) {
            g.studio = studio;
            g.stroke_active = false;
            if (studio)
                setStatus("Studio: drag to paint. Right-click flips erase/restore.", false);
        }
    }
    if (g.studio) {
        if (g.res_img == null) {
            dvui.label(@src(), "No processed result yet: strokes are saved and will be stamped when the frame is processed or exported.", .{}, .{
                .color_text = .{ .color = .{ .r = 0xe0, .g = 0xa0, .b = 0x4a } },
                .margin = .{ .y = 2, .h = 4 },
            });
        }
        {
            var row = dvui.box(@src(), .{ .dir = .horizontal }, .{ .expand = .horizontal });
            defer row.deinit();
            const st_on: dvui.Options = .{
                .expand = .horizontal,
                .background = true,
                .color_fill = .{ .color = .{ .r = 0x7a, .g = 0x33, .b = 0x30 } },
                .color_text = .{ .color = .{ .r = 0xff, .g = 0xd2, .b = 0xcc } },
            };
            const st_off: dvui.Options = .{ .expand = .horizontal };
            if (dvui.button(@src(), "Erase", .{}, if (g.studio_erase) st_on else st_off)) g.studio_erase = true;
            const rs_on: dvui.Options = .{
                .expand = .horizontal,
                .background = true,
                .color_fill = .{ .color = .{ .r = 0x25, .g = 0x5c, .b = 0x39 } },
                .color_text = .{ .color = .{ .r = 0xd3, .g = 0xff, .b = 0xe2 } },
            };
            if (dvui.button(@src(), "Restore", .{}, if (!g.studio_erase) rs_on else st_off)) g.studio_erase = false;
        }
        _ = dvui.sliderEntry(@src(), "Size {d:.0}", .{
            .value = &g.studio_size,
            .min = 2,
            .max = 300,
            .interval = 1,
        }, .{ .expand = .horizontal });
        var hard_pct = g.studio_hard * 100.0;
        if (dvui.sliderEntry(@src(), "Hard {d:.0}%", .{
            .value = &hard_pct,
            .min = 0,
            .max = 100,
            .interval = 1,
        }, .{ .expand = .horizontal })) {
            g.studio_hard = hard_pct / 100.0;
        }
        var opac_pct = g.studio_opac * 100.0;
        if (dvui.sliderEntry(@src(), "Opac {d:.0}%", .{
            .value = &opac_pct,
            .min = 5,
            .max = 100,
            .interval = 1,
        }, .{ .expand = .horizontal })) {
            g.studio_opac = opac_pct / 100.0;
        }
        _ = dvui.checkbox(@src(), &g.studio_view_original, "View original", .{});

        const info = currentEditInfo();
        if (info.strokes > 0) {
            dvui.label(@src(), "frame {d}: {d} stroke(s){s}", .{
                g.cur, info.strokes,
                if (info.bypassed) " · bypassed" else "",
            }, .{ .color_text = .{ .color = .{ .r = 0xe0, .g = 0xa0, .b = 0x4a } } });
            var byp = info.bypassed;
            if (dvui.checkbox(@src(), &byp, "Bypass this edit", .{})) setCurrentBypass(byp);
        } else {
            dvui.label(@src(), "frame {d}: no edit yet", .{g.cur}, .{
                .color_text = .{ .color = .{ .r = 0x8b, .g = 0x93, .b = 0xa0 } },
            });
        }
        {
            var row = dvui.box(@src(), .{ .dir = .horizontal }, .{ .expand = .horizontal });
            defer row.deinit();
            if (dvui.button(@src(), "Undo stroke", .{}, .{ .expand = .horizontal })) undoStroke();
            if (dvui.button(@src(), "Delete edit", .{}, .{ .expand = .horizontal })) deleteCurrentEdit();
        }
        {
            var row = dvui.box(@src(), .{ .dir = .horizontal }, .{ .expand = .horizontal });
            defer row.deinit();
            if (dvui.button(@src(), "Copy to similar faces", .{}, .{ .expand = .horizontal })) {
                const conflicts = countCopyConflicts();
                if (conflicts > 0) {
                    g.copy_pending = true;
                    g.copy_conflicts = conflicts;
                } else {
                    const n = copyEditToSimilar(true);
                    g.edit_gen +%= 1;
                    saveProject();
                    setStatusFmt("Retouch copied to {d} similar frame(s)", .{n}, false);
                }
            }
            if (dvui.button(@src(), "Clear all edits", .{}, .{ .expand = .horizontal })) clearAllEdits();
        }
        if (g.copy_pending) {
            dvui.label(@src(), "{d} target frame(s) already edited", .{g.copy_conflicts}, .{
                .color_text = .{ .color = .{ .r = 0xe0, .g = 0xa0, .b = 0x4a } },
            });
            var row = dvui.box(@src(), .{ .dir = .horizontal }, .{ .expand = .horizontal });
            defer row.deinit();
            if (dvui.button(@src(), "Overwrite", .{}, .{ .expand = .horizontal })) {
                const n = copyEditToSimilar(true);
                g.edit_gen +%= 1;
                saveProject();
                g.copy_pending = false;
                setStatusFmt("Retouch copied to {d} frame(s)", .{n}, false);
            }
            if (dvui.button(@src(), "Skip edited", .{}, .{ .expand = .horizontal })) {
                const n = copyEditToSimilar(false);
                g.edit_gen +%= 1;
                saveProject();
                g.copy_pending = false;
                setStatusFmt("Retouch copied to {d} frame(s)", .{n}, false);
            }
            if (dvui.button(@src(), "Cancel", .{}, .{ .expand = .horizontal })) g.copy_pending = false;
        }
    }

    _ = dvui.separator(@src(), .{ .expand = .horizontal, .margin = .{ .y = 6, .h = 2 } });

    // --- processing -------------------------------------------------------
    section("Process & cache");
    {
        var row = dvui.box(@src(), .{ .dir = .horizontal }, .{ .expand = .horizontal });
        defer row.deinit();
        var low = g.low_quality;
        if (dvui.checkbox(@src(), &low, "Low quality (fast)", .{})) g.low_quality = low;
        var uc = g.process_use_cache;
        if (dvui.checkbox(@src(), &uc, "Use cache", .{})) g.process_use_cache = uc;
    }
    {
        var row = dvui.box(@src(), .{ .dir = .horizontal }, .{ .expand = .horizontal });
        defer row.deinit();
        // Mouth restoration applies to the source face; toggling it changes the
        // cache signature, so the displayed result is re-resolved below.
        var mouth = g.host.restore_mouth(g.host.user);
        if (dvui.checkbox(@src(), &mouth, "Restore mouths", .{})) {
            g.host.set_restore_mouth(g.host.user, mouth);
            g.res_frame = -1;
        }
        // When set, faces with no explicit assignment keep the source pixels
        // instead of falling back to the default identity.
        var dskip = g.default_skip;
        if (dvui.checkbox(@src(), &dskip, "Skip unassigned", .{})) {
            g.default_skip = dskip;
            selectSegmentAt(g.cur);
            g.res_frame = -1;
            saveProject();
        }
    }
    {
        // Spell out what the current choice actually does. GFPGAN runs on each
        // face's crop, so frame size no longer limits it; only a real failure
        // disables it mid-job.
        const enh = g.host.enhancer(g.host.user);
        var ebuf: [160]u8 = undefined;
        const msg: []const u8 = if (g.low_quality)
            "Fast 640 px pass · enhancer off"
        else if (enh == 0)
            "Full res · enhancer: none"
        else if (g.enhancer_failed)
            "Full res · enhancer failed, disabled for this clip"
        else
            std.fmt.bufPrint(&ebuf, "Full res · enhancer on ({s}, {d:.0} MB)", .{ enhancerName(enh), vramTotalMb() }) catch "Full res · enhancer on";
        dvui.label(@src(), "{s}", .{clip(msg, 50)}, .{
            .color_text = .{ .color = .{ .r = 0x8b, .g = 0x93, .b = 0xa0 } },
        });
    }
    {
        var row = dvui.box(@src(), .{ .dir = .horizontal }, .{ .expand = .horizontal });
        defer row.deinit();
        if (dvui.button(@src(), "Process frame", .{}, .{ .expand = .horizontal })) startProcessFrame();
        if (dvui.button(@src(), "Process sequence", .{}, .{ .expand = .horizontal })) {
            if (g.sel_seg >= 0) {
                const s = g.segments.items[@intCast(g.sel_seg)];
                startProcessRange(s.start, s.end);
            } else {
                startProcessFrame();
            }
        }
    }
    if (dvui.button(@src(), "Process current chunk", .{}, .{ .expand = .horizontal })) {
        const start = g.in_frame + @divTrunc(g.cur - g.in_frame, chunkI()) * chunkI();
        startProcessRange(start, @min(start + chunkI() - 1, g.out_frame));
    }
    if (g.job == .process) {
        dvui.progress(@src(), .{ .percent = progressFrac() }, .{
            .expand = .horizontal,
            .min_size_content = .{ .h = 8 },
            .margin = .{ .y = 4, .h = 4 },
        });
        dvui.label(@src(), "processing {d}/{d}", .{
            g.job_done_frames.load(.monotonic), g.job_total_frames.load(.monotonic),
        }, .{});
    }

    _ = dvui.separator(@src(), .{ .expand = .horizontal, .margin = .{ .y = 6, .h = 2 } });

    // --- export -----------------------------------------------------------
    section("Export");
    {
        var row = dvui.box(@src(), .{ .dir = .horizontal }, .{ .expand = .horizontal });
        defer row.deinit();
        var with_audio = g.export_audio;
        if (dvui.checkbox(@src(), &with_audio, "Audio", .{})) g.export_audio = with_audio;
        var with_chunks = g.export_chunks;
        if (dvui.checkbox(@src(), &with_chunks, "Split chunks", .{})) g.export_chunks = with_chunks;
    }
    {
        var row = dvui.box(@src(), .{ .dir = .horizontal }, .{ .expand = .horizontal });
        defer row.deinit();
        if (dvui.button(@src(), "Export range", .{}, .{ .expand = .horizontal })) exportDialog(false);
        if (dvui.button(@src(), "Export all", .{}, .{ .expand = .horizontal })) exportDialog(true);
    }
    if (g.job == .encode) {
        dvui.progress(@src(), .{ .percent = progressFrac() }, .{
            .expand = .horizontal,
            .min_size_content = .{ .h = 8 },
            .margin = .{ .y = 4, .h = 4 },
        });
        dvui.label(@src(), "encoding {d}/{d}", .{
            g.job_done_frames.load(.monotonic), g.job_total_frames.load(.monotonic),
        }, .{});
        if (dvui.button(@src(), "Cancel export", .{}, .{ .expand = .horizontal })) g.cancel.store(true, .release);
    }
    dvui.label(@src(), "Export always runs at the source resolution; cached high-quality frames are reused.", .{}, .{
        .color_text = .{ .color = .{ .r = 0x74, .g = 0x7d, .b = 0x89 } },
    });
}

fn progressFrac() f32 {
    return @as(f32, @floatFromInt(g.progress.load(.monotonic))) / 1000.0;
}

const ChunkStats = struct { done: usize = 0, total: usize = 0 };

fn doneChunks() ChunkStats {
    var out = ChunkStats{};
    const span = @max(1, g.out_frame - g.in_frame + 1);
    out.total = @intCast(@max(1, @divTrunc(span + chunkI() - 1, chunkI())));
    lockData();
    defer g.data_mu.unlock();
    for (g.chunks.items) |c| {
        if (c.done and c.end >= g.in_frame and c.start <= g.out_frame) out.done += 1;
    }
    return out;
}

fn clip(text: []const u8, max_len: usize) []const u8 {
    if (text.len <= max_len) return text;
    return text[0 .. max_len - 1];
}

/// Total VRAM in MB, cached for ~500 ms because NVML queries are not free.
fn vramTotalMb() f32 {
    const now: i64 = @intCast(@divTrunc(ff.nowNs(), std.time.ns_per_ms));
    if (g.vram_checked_ms != 0 and now - g.vram_checked_ms < 500) return g.vram_total_mb;
    g.vram_checked_ms = now;
    g.vram_total_mb = ff.memoryInfo().vram_total_mb;
    return g.vram_total_mb;
}

fn enhancerName(enh: i32) []const u8 {
    return switch (enh) {
        1 => "GFPGAN",
        2 => "Real-ESRGAN x2",
        3 => "Real-ESRGAN x4",
        else => "none",
    };
}

/// Copy `text` into `buf`, truncating it to at most `max_chars` bytes with a
/// trailing "..." so a long video name cannot push the header buttons out of
/// view. Never splits a UTF-8 sequence.
fn clipEllipsisInto(buf: []u8, text: []const u8, max_chars: usize) []const u8 {
    if (text.len <= max_chars or max_chars <= 3) return text;
    var keep = max_chars - 3;
    while (keep > 0 and (text[keep] & 0xC0) == 0x80) keep -= 1;
    @memcpy(buf[0..keep], text[0..keep]);
    @memcpy(buf[keep..][0..3], "...");
    return buf[0 .. keep + 3];
}

fn section(text: []const u8) void {
    const id_extra: usize = @truncate(std.hash.Wyhash.hash(0, text));
    dvui.labelNoFmt(@src(), text, .{}, .{
        .id_extra = id_extra,
        .font = .theme(.heading),
        .margin = .{ .x = 0, .y = 10, .w = 0, .h = 3 },
    });
}

fn chunkI() i64 {
    return @max(1, @as(i64, @intFromFloat(@round(g.chunk_f))));
}

fn drawCenter() void {
    var col = dvui.box(@src(), .{ .dir = .vertical }, .{
        .expand = .both,
        .padding = .{ .x = 10, .y = 8, .w = 10, .h = 8 },
        .min_size_content = .{ .w = 320 },
    });
    defer col.deinit();

    // --- viewer -----------------------------------------------------------
    {
        var view = dvui.box(@src(), .{}, .{
            .expand = .both,
            .min_size_content = .{ .h = 200 },
            .background = true,
            .color_fill = .{ .color = .{ .r = 0x0e, .g = 0x10, .b = 0x13 } },
        });
        defer view.deinit();
        const crs = view.data().contentRectScale();
        const show_result = g.res_tex != null and !g.show_original;
        // In Studio the canvas shows the processed result so retouches are
        // visible; when the frame has no cached result yet, fall back to the
        // source frame instead of rendering nothing.
        const tex: ?dvui.Texture = if (g.studio and !g.studio_view_original)
            (g.res_tex orelse g.cur_tex)
        else if (show_result)
            g.res_tex
        else
            g.cur_tex;
        var drawn_rect: ?dvui.Rect.Physical = null;
        if (tex) |t| {
            if (t.width > 0 and t.height > 0) {
                const aspect = @as(f32, @floatFromInt(t.width)) / @as(f32, @floatFromInt(t.height));
                var w = crs.r.w;
                var h = crs.r.h;
                if (w / h > aspect) w = h * aspect else h = w / aspect;
                const r = dvui.Rect.Physical{
                    .x = crs.r.x + (crs.r.w - w) * 0.5,
                    .y = crs.r.y + (crs.r.h - h) * 0.5,
                    .w = w,
                    .h = h,
                };
                dvui.renderTexture(t, .{ .r = r, .s = crs.s }, .{ .corners = .all(0) }) catch {};
                // Face boxes get in the way while painting.
                if (g.show_boxes and !g.studio) drawBoxes(r, crs.s);
                drawn_rect = r;
            }
        } else if (g.cur_img == null) {
            dvui.label(@src(), "Decoding frame…", .{}, .{ .gravity_x = 0.5, .gravity_y = 0.5 });
        }
        if (g.studio) {
            if (drawn_rect) |r| studioEvents(view, crs, r) else g.studio_hover = null;
        } else {
            g.studio_hover = null;
        }
    }

    // --- transport --------------------------------------------------------
    {
        var row = dvui.box(@src(), .{ .dir = .horizontal }, .{
            .expand = .horizontal,
            .padding = .{ .x = 0, .y = 6, .w = 0, .h = 6 },
        });
        defer row.deinit();
        const bopts = dvui.Options{ .min_size_content = .{ .w = 36, .h = 26 }, .margin = .{ .x = 1, .y = 0, .w = 1, .h = 0 } };
        if (dvui.button(@src(), "|<", .{}, bopts)) {
            seekTo(g.in_frame);
            selectSegmentAt(g.cur);
        }
        if (dvui.button(@src(), "<", .{}, bopts)) {
            seekTo(g.cur - 1);
            selectSegmentAt(g.cur);
        }
        if (dvui.button(@src(), if (g.playing) "Pause" else "Play", .{}, .{
            .min_size_content = .{ .w = 64, .h = 26 },
            .margin = .{ .x = 4, .y = 0, .w = 4, .h = 0 },
        })) {
            if (g.playing) stopPlayback() else startPlayback();
        }
        if (dvui.button(@src(), ">", .{}, bopts)) {
            seekTo(g.cur + 1);
            selectSegmentAt(g.cur);
        }
        if (dvui.button(@src(), ">|", .{}, bopts)) {
            seekTo(g.out_frame);
            selectSegmentAt(g.cur);
        }
        var seq_only = g.play_sequence;
        if (dvui.checkbox(@src(), &seq_only, "Sequence", .{ .margin = .{ .x = 8, .y = 0, .w = 4, .h = 0 } })) g.play_sequence = seq_only;
        var loop = g.loop_play;
        if (dvui.checkbox(@src(), &loop, "Loop", .{ .margin = .{ .x = 8, .y = 0, .w = 4, .h = 0 } })) g.loop_play = loop;
        var orig = g.show_original;
        if (dvui.checkbox(@src(), &orig, "Original", .{})) g.show_original = orig;
        var boxes = g.show_boxes;
        if (dvui.checkbox(@src(), &boxes, "Boxes", .{ .margin = .{ .x = 8, .y = 0, .w = 4, .h = 0 } })) g.show_boxes = boxes;

        _ = dvui.spacer(@src(), .{ .expand = .horizontal });
        var tb: [32]u8 = undefined;
        var nb: [24]u8 = undefined;
        var db: [24]u8 = undefined;
        var gb: [24]u8 = undefined;
        const fw = frameWidth();
        dvui.label(@src(), "{s}  ·  frame {s}  ·  {s}/{s}", .{
            timeText(&tb, g.cur),
            padNum(&nb, g.cur, fw),
            padNum(&db, @intCast(g.job_done_frames.load(.monotonic)), fw),
            padNum(&gb, @intCast(g.job_total_frames.load(.monotonic)), fw),
        }, .{
            .gravity_y = 0.5,
            .color_text = .{ .color = .{ .r = 0xa8, .g = 0xb2, .b = 0xbe } },
        });
    }

    // --- timeline ---------------------------------------------------------
    if (g.show_timeline) drawTimeline();

    // --- current assignment legend ---------------------------------------
    {
        var row = dvui.box(@src(), .{ .dir = .horizontal }, .{
            .expand = .horizontal,
            .padding = .{ .x = 0, .y = 4, .w = 0, .h = 2 },
        });
        defer row.deinit();
        const seg_idx = segmentAt(g.cur);
        if (seg_idx >= 0) {
            const seg = g.segments.items[@intCast(seg_idx)];
            var ids: [max_faces]i32 = @splat(-1);
            idsFor(g.cur, seg.count, &ids);
            const nid = g.host.identity_count(g.host.user);
            const def = g.host.default_identity(g.host.user);
            dvui.label(@src(), "faces:", .{}, .{ .gravity_y = 0.5, .color_text = .{ .color = .{ .r = 0x8b, .g = 0x93, .b = 0xa0 } } });
            const shown = @min(seg.count, 3);
            const unchanged = dvui.Color{ .r = 0x8b, .g = 0x93, .b = 0xa0 };
            for (0..shown) |k| {
                const skipped = ids[k] == skip_id;
                const id = if (ids[k] >= 0) @as(usize, @intCast(ids[k])) else def;
                const label = if (skipped) "unchanged" else clip(if (id < nid) g.host.identity_label(g.host.user, id) else "default", 14);
                dvui.label(@src(), " {d}:{s}", .{ k + 1, label }, .{
                    .id_extra = k,
                    .gravity_y = 0.5,
                    .color_text = .{ .color = if (skipped) unchanged else faceColor(k) },
                });
            }
            if (seg.count > shown) {
                dvui.label(@src(), " +{d} more", .{seg.count - shown}, .{
                    .gravity_y = 0.5,
                    .color_text = .{ .color = .{ .r = 0x8b, .g = 0x93, .b = 0xa0 } },
                });
            }
        } else {
            dvui.label(@src(), "No face sequence at this frame.", .{}, .{
                .gravity_y = 0.5,
                .color_text = .{ .color = .{ .r = 0x74, .g = 0x7d, .b = 0x89 } },
            });
        }
    }

    // --- status -----------------------------------------------------------
    if (g.err_len > 0) {
        dvui.labelNoFmt(@src(), clip(g.errSlice(), 120), .{}, .{ .color_text = .{ .color = .{ .r = 0xf0, .g = 0x8a, .b = 0x8a } } });
    } else if (g.status_len > 0) {
        dvui.labelNoFmt(@src(), clip(g.statusSlice(), 120), .{}, .{
            .color_text = .{ .color = if (g.status_err) dvui.Color{ .r = 0xf0, .g = 0x8a, .b = 0x8a } else .{ .r = 0xa8, .g = 0xb2, .b = 0xbe } },
        });
    }

    handleKeys();
}

fn faceColor(i: usize) dvui.Color {
    const palette = [_]dvui.Color{
        .{ .r = 0x5f, .g = 0xd4, .b = 0x8f },
        .{ .r = 0x5a, .g = 0xa9, .b = 0xf0 },
        .{ .r = 0xf0, .g = 0xc0, .b = 0x5a },
        .{ .r = 0xe0, .g = 0x86, .b = 0xd0 },
        .{ .r = 0x6f, .g = 0xd8, .b = 0xd8 },
        .{ .r = 0xf0, .g = 0x8a, .b = 0x6a },
        .{ .r = 0xb0, .g = 0x9a, .b = 0xf0 },
        .{ .r = 0xc7, .g = 0xcd, .b = 0xd6 },
    };
    return palette[i % palette.len];
}

fn drawBoxes(r: dvui.Rect.Physical, s: f32) void {
    if (g.info.width <= 0 or g.info.height <= 0) return;
    var boxes: [max_faces][4]f32 = @splat(@splat(0));
    var count: usize = 0;
    if (sampleAt(g.cur)) |smp| {
        boxes = smp.b;
        count = smp.n;
    } else if (segmentAt(g.cur) >= 0) {
        const seg = g.segments.items[@intCast(segmentAt(g.cur))];
        boxes = seg.boxes;
        count = seg.count;
    }
    if (count == 0) return;
    const sx = r.w / @as(f32, @floatFromInt(g.info.width));
    const sy = r.h / @as(f32, @floatFromInt(g.info.height));
    var ids: [max_faces]i32 = @splat(-1);
    idsFor(g.cur, count, &ids);
    for (0..count) |k| {
        const b = boxes[k];
        // Skipped faces keep their source pixels, so draw them neutral instead
        // of with the face's assignment colour.
        const col = if (ids[k] == skip_id) dvui.Color{ .r = 0x8b, .g = 0x93, .b = 0xa0 } else faceColor(k);
        const x = r.x + b[0] * sx;
        const y = r.y + b[1] * sy;
        const w = b[2] * sx;
        const h = b[3] * sy;
        outlinePhys(.{ .x = x, .y = y, .w = w, .h = h }, 2, col);
    }
    _ = s;
}

fn outlinePhys(r: dvui.Rect.Physical, t: f32, col: dvui.Color) void {
    fillRect(.{ .x = r.x, .y = r.y, .w = r.w, .h = t }, col);
    fillRect(.{ .x = r.x, .y = r.y + r.h - t, .w = r.w, .h = t }, col);
    fillRect(.{ .x = r.x, .y = r.y, .w = t, .h = r.h }, col);
    fillRect(.{ .x = r.x + r.w - t, .y = r.y, .w = t, .h = r.h }, col);
}

fn fillRect(r: dvui.Rect.Physical, col: dvui.Color) void {
    r.fill(.{}, .{ .color = .{ .color = col } });
}

fn drawTimeline() void {
    var track = dvui.box(@src(), .{}, .{
        .expand = .horizontal,
        .min_size_content = .{ .h = 64 },
        .background = true,
        .color_fill = .{ .color = .{ .r = 0x1a, .g = 0x1d, .b = 0x22 } },
    });
    defer track.deinit();
    const crs = track.data().contentRectScale();
    const r = dvui.Rect.Physical{
        .x = crs.r.x + 6,
        .y = crs.r.y + 8,
        .w = @max(1, crs.r.w - 12),
        .h = @max(1, crs.r.h - 20),
    };
    const span: f64 = @as(f64, @floatFromInt(@max(1, g.out_frame - g.in_frame)));
    const xFor = struct {
        fn f(frame: i64, rr: dvui.Rect.Physical, in_f: i64, sp: f64) f32 {
            const frac: f64 = @as(f64, @floatFromInt(frame - in_f)) / sp;
            return rr.x + @as(f32, @floatCast(frac)) * rr.w;
        }
    }.f;

    // base track
    fillRect(r, .{ .r = 0x2a, .g = 0x2f, .b = 0x37 });

    // outside the analysis range
    if (g.in_frame > 0 and g.info.frames > 0) {
        const x = xFor(g.in_frame, r, 0, @as(f64, @floatFromInt(g.info.frames)));
        fillRect(.{ .x = r.x, .y = r.y, .w = @max(0, x - r.x), .h = r.h }, .{ .r = 0x14, .g = 0x16, .b = 0x1a });
    }
    if (g.info.frames > 0 and g.out_frame < g.info.frames - 1) {
        const x = xFor(g.out_frame, r, 0, @as(f64, @floatFromInt(g.info.frames)));
        fillRect(.{ .x = x, .y = r.y, .w = @max(0, r.x + r.w - x), .h = r.h }, .{ .r = 0x14, .g = 0x16, .b = 0x1a });
    }

    // analyzed chunks
    {
        lockData();
        defer g.data_mu.unlock();
        for (g.chunks.items) |c| {
            if (!c.done) continue;
            const x0 = xFor(c.start, r, g.in_frame, span);
            const x1 = xFor(c.end + 1, r, g.in_frame, span);
            fillRect(.{ .x = x0, .y = r.y + 2, .w = @max(1, x1 - x0), .h = 6 }, .{ .r = 0x2f, .g = 0x5a, .b = 0xa8 });
        }
        // face density / sequences
        for (g.segments.items, 0..) |seg, i| {
            if (seg.end < g.in_frame or seg.start > g.out_frame) continue;
            const x0 = xFor(@max(seg.start, g.in_frame), r, g.in_frame, span);
            const x1 = xFor(@min(seg.end, g.out_frame) + 1, r, g.in_frame, span);
            const h = 4.0 + @as(f32, @floatFromInt(seg.count)) * 2.0;
            const selected = @as(i32, @intCast(i)) == g.sel_seg;
            const col = if (selected) dvui.Color{ .r = 0x4c, .g = 0x8b, .b = 0xf5 } else dvui.Color{ .r = 0x3c, .g = 0x7a, .b = 0x5c };
            fillRect(.{ .x = x0, .y = r.y + r.h - h - 2, .w = @max(1, x1 - x0), .h = h }, col);
        }
    }

    // in/out markers
    const xin = xFor(g.in_frame, r, g.in_frame, span);
    const xout = xFor(g.out_frame, r, g.in_frame, span);
    fillRect(.{ .x = xin, .y = r.y - 2, .w = 2, .h = r.h + 4 }, .{ .r = 0x6f, .g = 0xd8, .b = 0x8f });
    fillRect(.{ .x = xout - 2, .y = r.y - 2, .w = 2, .h = r.h + 4 }, .{ .r = 0xf0, .g = 0x8a, .b = 0x6a });

    // playhead
    const xp = xFor(g.cur, r, g.in_frame, span);
    fillRect(.{ .x = xp - 1, .y = r.y - 4, .w = 3, .h = r.h + 8 }, .{ .r = 0xff, .g = 0xd0, .b = 0x5a });

    // interaction
    const track_id = track.data().id;
    // If scrubbing somehow outlived its capture (e.g. focus was lost before the
    // mouse-up), stop it. Otherwise every later click would be turned into a
    // seek.
    if (g.scrubbing and !dvui.captured(track_id)) g.scrubbing = false;

    const evts = dvui.events();
    for (evts) |*e| {
        if (e.handled) continue;
        const me = switch (e.evt) {
            .mouse => |m| m,
            else => continue,
        };
        if (me.action == .press and me.button == .left and !g.scrubbing and dvui.eventMatch(e, .{ .id = track_id, .r = r })) {
            g.scrubbing = true;
            seekFromX(me.p.x, r);
            dvui.captureMouse(track.data(), e.num);
            e.handle(@src(), track.data());
        } else if (me.action == .motion and g.scrubbing and dvui.captured(track_id)) {
            seekFromX(me.p.x, r);
            e.handle(@src(), track.data());
        } else if (me.action == .release and g.scrubbing) {
            seekFromX(me.p.x, r);
            g.scrubbing = false;
            // DVUI keeps a capture alive as long as the widget exists, so it
            // MUST be released explicitly on mouse-up. Without this the
            // timeline keeps receiving every click in the window, turning the
            // whole UI into a seeker and making all other buttons dead.
            dvui.captureMouse(null, e.num);
            e.handle(@src(), track.data());
        }
    }
}

fn seekFromX(x: f32, r: dvui.Rect.Physical) void {
    const frac = std.math.clamp((x - r.x) / @max(1, r.w), 0.0, 1.0);
    const span = g.out_frame - g.in_frame;
    const frame = g.in_frame + @as(i64, @intFromFloat(@round(frac * @as(f32, @floatFromInt(span)))));
    seekTo(frame);
    selectSegmentAt(frame);
    g.res_frame = -1;
}

fn drawRightPanel() void {
    var panel = dvui.box(@src(), .{ .dir = .vertical }, .{
        .expand = .vertical,
        .min_size_content = .{ .w = 260 },
        .max_size_content = .{ .w = 300, .h = dvui.max_float_safe },
        .padding = .all(10),
        .background = true,
        .style = .window,
    });
    defer panel.deinit();

    section("Identity library");
    g.host.draw_identities(g.host.user);

    section("Cache");
    const sig = cacheSig();
    var lbuf: [1024]u8 = @splat(0);
    var hbuf: [1024]u8 = @splat(0);
    const low_cached = if (framePathInto(.low, sig, g.cur, &lbuf)) |p| ff.fileExists(p) else false;
    const high_cached = if (framePathInto(.high, sig, g.cur, &hbuf)) |p| ff.fileExists(p) else false;
    dvui.label(@src(), "frame {d}: low {s} · high {s}", .{
        g.cur,
        if (low_cached) "cached" else "—",
        if (high_cached) "cached" else "—",
    }, .{});
    dvui.label(@src(), "signature {x:0>8}", .{@as(u32, @truncate(sig))}, .{
        .color_text = .{ .color = .{ .r = 0x74, .g = 0x7d, .b = 0x89 } },
    });
    if (dvui.button(@src(), "Forget this frame's cache", .{}, .{ .expand = .horizontal })) {
        for ([_]Quality{ .low, .high }) |q| {
            var rbuf: [1024]u8 = @splat(0);
            if (framePathInto(q, sig, g.cur, &rbuf)) |p| ff.removeFile(p);
        }
        g.res_frame = -1;
        setStatus("Cache entry removed for this frame", false);
    }
    if (dvui.button(@src(), "Save project", .{}, .{ .expand = .horizontal })) {
        saveProject();
        setStatus("Project saved", false);
    }
    dvui.label(@src(), "{s}", .{pathSlice(&g.proj_path, g.proj_path_len)}, .{
        .color_text = .{ .color = .{ .r = 0x74, .g = 0x7d, .b = 0x89 } },
    });

    section("Shortcuts");
    dvui.label(@src(), "Space play/pause · Left/Right step · Shift+Left/Right 10 frames · I/O set in/out", .{}, .{
        .color_text = .{ .color = .{ .r = 0x8b, .g = 0x93, .b = 0xa0 } },
    });
    dvui.label(@src(), "Sequence plays only the selected sequence; combine it with Loop to review a swap over and over.", .{}, .{
        .color_text = .{ .color = .{ .r = 0x8b, .g = 0x93, .b = 0xa0 } },
    });
    dvui.label(@src(), "Analysis and processing run in the background; the project is saved after every finished chunk.", .{}, .{
        .color_text = .{ .color = .{ .r = 0x74, .g = 0x7d, .b = 0x89 } },
        .margin = .{ .y = 6, .h = 0 },
    });
}

fn handleKeys() void {
    if (dvui.focusedWidgetId() != null) return;
    const evts = dvui.events();
    for (evts) |*e| {
        if (e.handled) continue;
        const ke = switch (e.evt) {
            .key => |k| k,
            else => continue,
        };
        if (ke.action != .down) continue;
        switch (ke.code) {
            .space => {
                if (g.playing) stopPlayback() else startPlayback();
                e.handled = true;
            },
            .left => {
                seekTo(g.cur - (if (ke.mod.shift()) @as(i64, 10) else 1));
                selectSegmentAt(g.cur);
                g.res_frame = -1;
                e.handled = true;
            },
            .right => {
                seekTo(g.cur + (if (ke.mod.shift()) @as(i64, 10) else 1));
                selectSegmentAt(g.cur);
                g.res_frame = -1;
                e.handled = true;
            },
            .home => {
                seekTo(g.in_frame);
                e.handled = true;
            },
            .end => {
                seekTo(g.out_frame);
                e.handled = true;
            },
            .i => {
                g.in_frame = g.cur;
                if (g.out_frame < g.in_frame) g.out_frame = g.in_frame;
                e.handled = true;
            },
            .o => {
                g.out_frame = g.cur;
                if (g.in_frame > g.out_frame) g.in_frame = g.out_frame;
                e.handled = true;
            },
            else => {},
        }
    }
    if (g.scrubbing) dvui.refresh(null, @src(), null);
}

fn widgetClicked(wd: *dvui.WidgetData) bool {
    const evts = dvui.events();
    for (evts) |*e| {
        if (e.handled) continue;
        const me = switch (e.evt) {
            .mouse => |m| m,
            else => continue,
        };
        if (me.action != .press or me.button != .left) continue;
        if (!dvui.eventMatch(e, .{ .id = wd.id, .r = wd.rectScale().r })) continue;
        e.handle(@src(), wd);
        return true;
    }
    return false;
}
