//! FaceForge — native face restoration and face swap.
//!
//! GUI layer: Zig + DVUI (SDL3 backend). All inference lives in the C++ core
//! (ONNX Runtime + OpenCV) and is reached through the C ABI in
//! core/include/faceforge.h.
//!
//! This is the full editor: drag & drop, clipboard, per-face hot updates,
//! restore-mouth, enhancer selection, fast preview, undo history and export.

const std = @import("std");
const dvui = @import("dvui");
const sdl = @import("sdl-backend");
const ff = @import("ff.zig");
const ffi = @import("ffi");
const video = @import("video.zig");
const config = @import("config.zig");
const update = @import("update.zig");
const build_options = @import("build_options");

pub const main = dvui.App.main;
pub const panic = dvui.App.panic;
pub const std_options: std.Options = .{ .logFn = dvui.App.logFn };

pub const dvui_app: dvui.App = .{
    .config = .{
        .options = .{
            .size = .{ .w = 1460, .h = 920 },
            .min_size = .{ .w = 1040, .h = 660 },
            .title = "FaceForge",
            .app = "FaceForge",
            .org = "dev.faceforge",
            .persist_window_geometry = true,
        },
    },
    .frameFn = appFrame,
    .initFn = appInit,
    .deinitFn = appDeinit,
};

const c_alloc = std.heap.c_allocator;
const app_version: []const u8 = build_options.version;
const repo_url = update.repo_url;
/// The configuration is only saved when the user changes something from the
/// Settings panel, and the test harnesses never touch the user's real file
/// unless they were given an explicit --config.
const update_intervals = [_]struct { hours: u32, label: []const u8 }{
    .{ .hours = 6, .label = "Every 6 hours" },
    .{ .hours = 24, .label = "Daily" },
    .{ .hours = 168, .label = "Weekly" },
    .{ .hours = 720, .label = "Monthly" },
};
const max_faces = 32;
/// Maximum number of selectable identity faces in the library (all faces
/// gathered from every identity image that has been added).
const max_identities = 48;
const history_cap = 16;
/// Longest side used for interactive (fast) preview processing.
const preview_long_side: f32 = 640.0;

const Provider = enum(usize) { auto = 0, cuda = 1, cpu = 2 };
const Mode = enum { photo, video };
const Enhancer = enum(usize) { none = 0, gfpgan = 1, esrgan_x2 = 2, esrgan_x4 = 3 };

const JobKind = enum(u32) {
    none = 0,
    init = 1,
    reload_target = 2,
    add_identity = 3,
    process_all = 4,
    process_one = 5,
    compose = 6,
};

/// Headless-ish smoke test driven by FACEFORGE_SELFTEST. Each value names the
/// job whose completion we are waiting for.
const SelfTest = enum(u8) {
    off = 0,
    init = 1,
    per_face = 2,
    compose_skip = 3,
    process_one = 4,
    compose_all = 5,
    level_preview = 6,
    level_full = 7,
    level_cached = 8,
    apply = 9,
    undo = 10,
    done = 11,
};

/// Export requested while the canvas was showing a downscaled preview. The
/// full-resolution job runs first and the export finishes in `pollJob`.
const ExportKind = enum(u8) { none, save, copy, apply_input };

const Stage = enum(u32) {
    idle = 0,
    loading = 1,
    detecting = 2,
    swapping = 3,
    enhancing = 4,
    compositing = 5,
    done = 6,

    fn text(self: Stage) []const u8 {
        return switch (self) {
            .idle => "idle",
            .loading => "loading models",
            .detecting => "detecting faces",
            .swapping => "swapping",
            .enhancing => "enhancing",
            .compositing => "compositing",
            .done => "ready",
        };
    }
};

const Timings = struct {
    detect_ms: f64 = 0,
    embed_ms: f64 = 0,
    swap_ms: f64 = 0,
    enhance_ms: f64 = 0,
    total_ms: f64 = 0,
};

/// A cached 128x128 swap result plus the affine matrix that places it back in
/// the working image. Reusing these lets us recompose without the model.
///
/// The metadata records exactly how the patch was produced so it can be reused
/// until any of those inputs change: which identity was swapped in, whether the
/// original mouth was restored, which working-image resolution it belongs to,
/// and the identity-library generation.
const Patch = struct {
    img: ff.Image = .{ .data = undefined, .width = 128, .height = 128, .stride = 128 * 3 },
    matrix: [6]f64 = @splat(0),
    valid: bool = false,
    identity: i32 = -1,
    mouth: bool = false,
    id_gen: u32 = 0,
    proc_w: i32 = 0,
    proc_h: i32 = 0,
};

/// Two patch tables: index 0 is the full-resolution level, index 1 the
/// downscaled fast-preview level. Keeping both lets the user toggle quality
/// back and forth without re-running the swapper for faces already done.
const patch_levels = 2;
const full_level = 0;
const preview_level = 1;

/// The image swapper operates on: either a clone/downscale of the base image
/// (owned) or an alias of the pristine base (not owned).
const WorkImage = struct {
    img: ff.Image = .{ .data = undefined, .width = 0, .height = 0, .stride = 0 },
    owned: bool = false,

    fn deinit(self: *WorkImage) void {
        if (self.owned) self.img.deinit();
        self.* = .{};
    }
};

/// One stroke of the Studio retouch mask, stored as a rectangle of pixels in
/// base-image coordinates. The undo stack keeps the "before" bytes; the redo
/// stack keeps the "after" bytes.
const MaskEdit = struct {
    x: i32,
    y: i32,
    w: i32,
    h: i32,
    data: []u8,
};

const Thumb = struct {
    img: ?ff.Image = null,
    tex: ?dvui.Texture = null,
    dirty: bool = false,
};

/// One selectable identity face: an ArcFace embedding plus a cropped thumbnail
/// and a human-readable label ("lena.jpg #0"). Identities are gathered from any
/// number of identity images and can be assigned per target face.
const Identity = struct {
    embedding: [512]f32 = @splat(0),
    thumb: Thumb = .{},
    label: [64]u8 = @splat(0),
    label_len: usize = 0,

    fn labelSlice(self: *const Identity) []const u8 {
        return self.label[0..self.label_len];
    }
};

/// An identity image decoded and waiting to be scanned by the worker.
const PendingIdentity = struct {
    img: ff.Image,
    label: [64]u8 = @splat(0),
    label_len: usize = 0,

    fn labelSlice(self: *const PendingIdentity) []const u8 {
        return self.label[0..self.label_len];
    }
};

const State = struct {
    allocator: std.mem.Allocator = undefined,
    win: ?*dvui.Window = null,

    model_dir: []u8 = &.{},
    /// Where video caches/parts live. Empty means next to the source video.
    temp_dir: []u8 = &.{},
    /// Default folder for saved images and exported videos. Empty means next to
    /// the input file.
    export_dir: []u8 = &.{},
    target_path: []u8 = &.{},
    source_path: []u8 = &.{},
    save_path: []u8 = &.{},

    /// Persistent settings. `config_path` is empty when persistence is off
    /// (--no-config or a test run without an explicit path).
    config_path: []u8 = &.{},
    config_enabled: bool = false,
    config_dirty: bool = false,
    /// Set when the user picked an export path explicitly, so opening another
    /// target does not overwrite it.
    save_explicit: bool = false,

    provider: Provider = .auto,
    mode: Mode = .photo,
    enhancer: Enhancer = .gfpgan,
    restore_mouth: bool = true,
    fast_preview: bool = true,
    unload_after_run: bool = false,
    show_overlay: bool = true,
    /// Photo mode: show the untouched input instead of the composed result.
    photo_view_original: bool = false,
    /// Floating panels toggled from the top bar.
    show_settings: bool = false,
    show_about: bool = false,

    /// Update checker preferences (persisted with the rest of the settings).
    check_updates: bool = true,
    update_interval_hours: u32 = 24,
    last_update_check: i64 = 0,

    engine: ?ff.Engine = null,
    provider_name: [32]u8 = @splat(0),
    provider_len: usize = 0,

    // pristine full-resolution input
    base: ?ff.Image = null,
    base_pma: ?[]dvui.Color.PMA = null,
    base_tex: ?dvui.Texture = null,
    base_dirty: bool = false,

    // identity library: a flat list of selectable faces gathered from every
    // identity image that has been added.
    identities: [max_identities]Identity = @splat(Identity{}),
    identity_count: usize = 0,
    default_identity: usize = 0,
    identity_gen: u32 = 0,
    pending_identities: std.ArrayListUnmanaged(PendingIdentity) = .empty,
    pending_mu: std.atomic.Mutex = .unlocked,

    // composed output
    result: ?ff.Image = null,
    result_pma: ?[]dvui.Color.PMA = null,
    result_tex: ?dvui.Texture = null,
    result_dirty: bool = false,
    /// Set once the composed result matches this signature, so a `.compose`
    /// job that changes nothing can be skipped entirely.
    result_sig: u64 = 0,
    result_level: i32 = -1,

    proc: WorkImage = .{},
    proc_w: i32 = 0,
    proc_h: i32 = 0,
    /// Which patch table `proc` corresponds to (full_level / preview_level).
    proc_level: usize = full_level,

    // Studio (retouch layer): a base-resolution alpha mask that reveals the
    // original pixels over the composed result. 0 = result, 255 = original.
    studio: bool = false,
    studio_erase: bool = true,
    studio_size: f32 = 60.0,
    studio_hard: f32 = 0.6,
    studio_opac: f32 = 0.9,
    studio_show_mask: bool = true,
    studio_view_original: bool = false,
    studio_hover: ?dvui.Point.Physical = null,
    mask: []u8 = &.{},
    mask_w: i32 = 0,
    mask_h: i32 = 0,
    mask_gen: u32 = 0,
    mask_used: bool = false,
    mask_tex: ?dvui.Texture = null,
    mask_tex_gen: u32 = 0xffff_ffff,
    /// Composed result with the retouch mask blended in. Mirrors `result`
    /// whenever the mask is empty, so it is only allocated once painting starts.
    display: ?ff.Image = null,
    display_gen: u32 = 0,
    /// Bumped whenever the visible result changes (new result image or mask
    /// edit). `display_gen` records the generation `display` was built from.
    view_gen: u32 = 0,
    /// Cached pristine original resized to the result resolution, used as the
    /// bottom layer while the result is downscaled for fast preview.
    orig_cache: ?ff.Image = null,
    orig_cache_gen: u64 = 0xffff_ffff_ffff_ffff,
    stroke_active: bool = false,
    stroke_before: ?[]u8 = null,
    stroke_min_x: i32 = 0,
    stroke_min_y: i32 = 0,
    stroke_max_x: i32 = -1,
    stroke_max_y: i32 = -1,
    stroke_last_x: f32 = 0,
    stroke_last_y: f32 = 0,
    mask_undo: [32]?MaskEdit = @splat(null),
    mask_undo_len: usize = 0,
    mask_redo: [32]?MaskEdit = @splat(null),
    mask_redo_len: usize = 0,

    faces: [max_faces]ff.Face = undefined,
    face_count: usize = 0,
    face_sel: [max_faces]bool = @splat(true),
    /// Per-face identity override; -1 means "use the default identity".
    face_identity: [max_faces]i32 = @splat(-1),
    /// Per-face mouth-restore override; null means "use the global default".
    face_mouth_over: [max_faces]?bool = @splat(null),
    face_thumb: [max_faces]Thumb = @splat(Thumb{}),
    patches: [patch_levels][max_faces]Patch = .{
        @as([max_faces]Patch, @splat(Patch{})),
        @as([max_faces]Patch, @splat(Patch{})),
    },
    /// Bumped whenever `base` changes, so caches can be invalidated.
    base_gen: u64 = 0,

    // view
    zoom: f32 = 1.0,
    pan_x: f32 = 0,
    pan_y: f32 = 0,
    view_w: f32 = 1,
    view_h: f32 = 1,
    view_fit: f32 = 1,
    drag_pan: bool = false,

    // undo / redo of the base image
    undo: [history_cap]?ff.Image = @splat(null),
    undo_len: usize = 0,
    redo: [history_cap]?ff.Image = @splat(null),
    redo_len: usize = 0,

    // worker
    thread: ?std.Thread = null,
    working: std.atomic.Value(bool) = .init(false),
    job_kind: std.atomic.Value(u32) = .init(0),
    job_arg: i32 = -1,
    stage_value: std.atomic.Value(u32) = .init(0),
    progress_value: std.atomic.Value(u32) = .init(0),
    job_done: std.atomic.Value(bool) = .init(false),
    job_ok: std.atomic.Value(bool) = .init(false),
    job_error: [512]u8 = @splat(0),
    job_error_len: usize = 0,
    // Coalesced follow-up when the user edits something mid-job.
    queued_job: JobKind = .none,
    queued_arg: i32 = -1,

    job_model_dir: [1024]u8 = @splat(0),
    job_provider: i32 = 0,
    job_enhancer: i32 = 0,
    job_restore_mouth: bool = false,
    job_fast_preview: bool = false,
    job_unload: bool = false,

    pending_result: ?[]u8 = null,
    pending_w: i32 = 0,
    pending_h: i32 = 0,

    timings: Timings = .{},
    auto_run: bool = false,
    auto_saved: bool = false,
    booting: bool = false,
    selftest: SelfTest = .off,
    selftest_t0: i128 = 0,
    pending_export: ExportKind = .none,
    export_path: ?[]u8 = null,
    status: [512]u8 = @splat(0),
    status_len: usize = 0,
    status_is_error: bool = false,

    vram_used: f32 = 0,
    vram_total: f32 = 0,
    rss: f32 = 0,
    last_memory_poll: i64 = 0,

    /// Right edge (in window points) of the identity rail, used to route drops.
    left_panel_edge: f32 = 320,

    fn statusSlice(self: *const State) []const u8 {
        return self.status[0..self.status_len];
    }
};

var g: State = .{};

// ---------------------------------------------------------------------------
// Drag & drop (SDL event watch; routed by drop position)
// ---------------------------------------------------------------------------

var drop_buf: [1024]u8 = @splat(0);
var drop_len: usize = 0;
var drop_x: f32 = 0;
var drop_y: f32 = 0;
var drop_pending: bool = false;

fn dropWatch(_: ?*anyopaque, event: [*c]sdl.c.SDL_Event) callconv(.c) bool {
    if (event.*.type == sdl.c.SDL_EVENT_DROP_FILE) {
        const e = &event.*.drop;
        const data = e.data;
        if (data != null) {
            const path = std.mem.span(@as([*:0]const u8, @ptrCast(data)));
            const n = @min(path.len, drop_buf.len - 1);
            @memcpy(drop_buf[0..n], path[0..n]);
            drop_len = n;
            drop_x = e.x;
            drop_y = e.y;
            drop_pending = true;
        }
    }
    return true; // never consume the event
}

fn consumeDrop() void {
    if (!drop_pending) return;
    drop_pending = false;
    const path = drop_buf[0..drop_len];
    if (path.len == 0) return;
    if (isVideoPath(path)) {
        g.mode = .video;
        video.openPath(path);
        return;
    }
    if (g.mode == .video) {
        video.openPath(path);
        return;
    }
    if (drop_x < g.left_panel_edge) {
        loadIdentityImage(path);
        setStatusFmt("Identity dropped · {s}", .{baseName(path)}, false);
    } else {
        loadTargetImage(path);
        setStatusFmt("Target dropped · {s}", .{baseName(path)}, false);
    }
}

// ---------------------------------------------------------------------------
// Clipboard (PNG image via SDL3)
// ---------------------------------------------------------------------------

const ClipPayload = struct { png: []u8 };

fn clipDataCb(userdata: ?*anyopaque, mime: [*c]const u8, size: [*c]usize) callconv(.c) ?*const anyopaque {
    const payload: *ClipPayload = @ptrCast(@alignCast(userdata.?));
    const m = std.mem.span(@as([*:0]const u8, @ptrCast(mime)));
    if (std.mem.eql(u8, m, "image/png")) {
        size.* = payload.png.len;
        return payload.png.ptr;
    }
    size.* = 0;
    return null;
}

fn clipCleanupCb(userdata: ?*anyopaque) callconv(.c) void {
    const payload: *ClipPayload = @ptrCast(@alignCast(userdata.?));
    c_alloc.free(payload.png);
    c_alloc.destroy(payload);
}

fn setClipboardPng(png: []const u8) void {
    const payload = c_alloc.create(ClipPayload) catch return;
    const buf = c_alloc.dupe(u8, png) catch {
        c_alloc.destroy(payload);
        return;
    };
    payload.* = .{ .png = buf };
    const mimes = [_][*c]const u8{ "image/png", null };
    if (!sdl.c.SDL_SetClipboardData(clipDataCb, clipCleanupCb, payload, &mimes, 1)) {
        c_alloc.free(buf);
        c_alloc.destroy(payload);
        setStatus("Clipboard copy failed", true);
        return;
    }
    setStatus("Result copied to clipboard", false);
}

fn readClipboardImage() ?ff.Image {
    const mimes = [_][*c]const u8{ "image/png", "image/bmp", "image/jpeg", "image/webp", null };
    for (mimes[0..4]) |m| {
        var size: usize = 0;
        const raw = sdl.c.SDL_GetClipboardData(m, &size);
        if (raw) |p| {
            if (size > 0) {
                const bytes = @as([*]const u8, @ptrCast(p))[0..size];
                const img = ff.Image.decode(bytes) catch null;
                sdl.c.SDL_free(p);
                if (img) |im| return im;
            } else {
                sdl.c.SDL_free(p);
            }
        }
    }
    return null;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

pub fn appInit(win: *dvui.Window) anyerror!void {
    g.allocator = win.gpa;
    g.win = win;
    win.themeSet(makeTheme());

    const init = dvui.App.main_init;
    const env_map: ?*const std.process.Environ.Map = if (init) |i| i.environ_map else null;

    // ---- configuration precedence: CLI > environment > file > defaults ----
    setStr(&g.model_dir, "models");
    setStr(&g.temp_dir, "");
    setStr(&g.export_dir, "");
    g.provider = .auto;
    g.unload_after_run = false;
    g.check_updates = true;
    g.update_interval_hours = 24;
    g.last_update_check = 0;

    // Collect CLI overrides first so `--no-config` and `--config` can be acted
    // on before the file is read.
    var args: []const [:0]const u8 = &.{};
    if (init) |i| args = i.minimal.args.toSlice(i.arena.allocator()) catch &.{};

    var cli_models: ?[]const u8 = null;
    var cli_temp: ?[]const u8 = null;
    var cli_export: ?[]const u8 = null;
    var cli_provider: ?[]const u8 = null;
    var cli_config: ?[]const u8 = null;
    var cli_target: ?[]const u8 = null;
    var cli_identity: ?[]const u8 = null;
    var cli_save: ?[]const u8 = null;
    var cli_video: ?[]const u8 = null;
    var cli_no_config = false;
    var cli_no_update_check = false;
    var cli_check_updates = false;

    var ai: usize = 1;
    while (ai < args.len) : (ai += 1) {
        const a = args[ai];
        const needValue = struct {
            fn get(list: []const [:0]const u8, idx: *usize) ?[]const u8 {
                if (idx.* + 1 >= list.len) return null;
                idx.* += 1;
                return list[idx.*];
            }
        };
        if (std.mem.eql(u8, a, "--models") or std.mem.eql(u8, a, "--models-dir")) {
            cli_models = needValue.get(args, &ai);
        } else if (std.mem.eql(u8, a, "--temp-dir")) {
            cli_temp = needValue.get(args, &ai);
        } else if (std.mem.eql(u8, a, "--export-dir")) {
            cli_export = needValue.get(args, &ai);
        } else if (std.mem.eql(u8, a, "--provider") or std.mem.eql(u8, a, "--device")) {
            cli_provider = needValue.get(args, &ai);
        } else if (std.mem.eql(u8, a, "--config")) {
            cli_config = needValue.get(args, &ai);
        } else if (std.mem.eql(u8, a, "--target")) {
            cli_target = needValue.get(args, &ai);
        } else if (std.mem.eql(u8, a, "--identity")) {
            cli_identity = needValue.get(args, &ai);
        } else if (std.mem.eql(u8, a, "--save")) {
            cli_save = needValue.get(args, &ai);
        } else if (std.mem.eql(u8, a, "--video")) {
            cli_video = needValue.get(args, &ai);
        } else if (std.mem.eql(u8, a, "--no-config")) {
            cli_no_config = true;
        } else if (std.mem.eql(u8, a, "--no-update-check")) {
            cli_no_update_check = true;
        } else if (std.mem.eql(u8, a, "--check-updates")) {
            cli_check_updates = true;
        }
    }

    g.auto_run = std.c.getenv("FACEFORGE_AUTORUN") != null;
    const test_mode = std.c.getenv("FACEFORGE_SELFTEST") != null or
        std.c.getenv("FACEFORGE_VIDEOTEST") != null;
    const explicit_config = cli_config != null or
        (env_map != null and env_map.?.get("FACEFORGE_CONFIG") != null);

    // Tests never read or write the user's real settings unless a path was
    // supplied explicitly, so CI and local smoke runs stay hermetic.
    if (!cli_no_config and (!test_mode or explicit_config)) {
        const path: ?[]u8 = if (cli_config) |p|
            g.allocator.dupe(u8, p) catch null
        else if (env_map) |m| blk: {
            if (m.get("FACEFORGE_CONFIG")) |p|
                break :blk g.allocator.dupe(u8, p) catch null;
            break :blk config.defaultPath(g.allocator, m) catch null;
        } else null;

        if (path) |p| {
            setStr(&g.config_path, p);
            g.allocator.free(p);
            g.config_enabled = true;
        }
    }

    if (g.config_enabled) {
        if (config.load(g.allocator, g.config_path)) |loaded| {
            defer loaded.parsed.deinit();
            const v = loaded.values;
            g.provider = @fromBackingInt(v.provider);
            setStr(&g.model_dir, v.models_dir);
            setStr(&g.temp_dir, v.temp_dir);
            setStr(&g.export_dir, v.export_dir);
            g.unload_after_run = v.unload_after_run;
            g.check_updates = v.check_updates;
            g.update_interval_hours = v.update_interval_hours;
            g.last_update_check = v.last_update_check;
        }
    }

    if (env_map) |m| {
        if (m.get("FACEFORGE_MODELS")) |v| setStr(&g.model_dir, v);
        if (m.get("FACEFORGE_TEMP_DIR")) |v| setStr(&g.temp_dir, v);
        if (m.get("FACEFORGE_EXPORT_DIR")) |v| setStr(&g.export_dir, v);
        if (m.get("FACEFORGE_PROVIDER")) |v| g.provider = @fromBackingInt(config.providerFromName(v));
        if (m.get("FACEFORGE_SEQUENTIAL") != null) g.unload_after_run = true;
        if (m.get("FACEFORGE_CHECK_UPDATES")) |v| {
            g.check_updates = !(std.mem.eql(u8, v, "0") or std.ascii.eqlIgnoreCase(v, "false") or std.ascii.eqlIgnoreCase(v, "off"));
        }
        if (m.get("FACEFORGE_UPDATE_INTERVAL_HOURS")) |v| {
            if (std.fmt.parseInt(u32, v, 10)) |h| g.update_interval_hours = std.math.clamp(h, 1, 24 * 365) else |_| {}
        }
    }

    if (cli_models) |v| setStr(&g.model_dir, v);
    if (cli_temp) |v| setStr(&g.temp_dir, v);
    if (cli_export) |v| setStr(&g.export_dir, v);
    if (cli_provider) |v| g.provider = @fromBackingInt(config.providerFromName(v));
    if (cli_no_update_check) g.check_updates = false;
    if (cli_check_updates) g.check_updates = true;

    var open_video: ?[]const u8 = null;
    if (std.c.getenv("FACEFORGE_VIDEOTEST")) |p| {
        g.mode = .video;
        video.setAutotest(std.mem.span(p));
    } else if (cli_video) |p| {
        g.mode = .video;
        open_video = p;
    } else if (env_map) |m| {
        if (m.get("FACEFORGE_VIDEO")) |p| {
            g.mode = .video;
            open_video = p;
        }
    }
    if (std.c.getenv("FACEFORGE_SELFTEST") != null) {
        g.selftest = .init;
        g.auto_run = false;
        // The scripted test exercises the full-resolution pipeline so the
        // export/apply paths are measured at their real size.
        g.fast_preview = false;
    }

    setStr(&g.target_path, cli_target orelse "assets/target_t1.jpg");
    setStr(&g.source_path, "assets/source_lena.jpg");
    setStr(&g.save_path, "bench/results/gui_result.png");
    if (cli_save) |p| {
        setStr(&g.save_path, p);
        g.save_explicit = true;
    }

    _ = sdl.c.SDL_AddEventWatch(dropWatch, null);

    // The video workspace resolves its project/cache/export paths relative to
    // the configured folders, and openPath() runs before the first draw(), so
    // the host has to be in place now.
    video.setHost(videoHost());

    g.booting = true;
    loadTargetImage(g.target_path);
    // Optional semicolon-separated list of identity images for headless/testing
    // runs; otherwise the bundled asset is used.
    const identity_spec: ?[]const u8 = cli_identity orelse
        (if (env_map) |m| m.get("FACEFORGE_IDENTITY") else null);
    if (identity_spec) |spec| {
        var it = std.mem.tokenizeScalar(u8, spec, ';');
        while (it.next()) |p| queueIdentityFile(p);
    } else {
        queueIdentityFile(g.source_path);
    }
    g.booting = false;

    if (g.mode == .video) {
        if (open_video) |p| video.openPath(p);
    }

    // Kick off a periodic release check when it is due. The result is only
    // shown in About; nothing is downloaded or installed automatically. Test
    // runs stay offline unless they were given an explicit config path.
    const auto_update = g.check_updates and !(test_mode and !explicit_config);
    if (auto_update and update.due(true, g.update_interval_hours, g.last_update_check, nowSeconds())) {
        if (init) |i| {
            update.check(app_version, i.io);
            g.last_update_check = nowSeconds();
            markConfigDirty();
        }
    }

    setStatus("Loading models…", false);
    spawnJob(.init, -1);
}

pub fn appDeinit(win: *dvui.Window) void {
    _ = win;
    saveConfig();
    video.deinit();
    if (g.thread) |t| {
        t.join();
        g.thread = null;
    }
    if (g.engine) |*e| e.deinit();
    if (g.pending_result) |buf| c_alloc.free(buf);
    if (g.export_path) |p| g.allocator.free(p);

    deinitImage(&g.base);
    deinitImage(&g.result);
    g.proc.deinit();
    freePmaArr(&g.base_pma);
    freePmaArr(&g.result_pma);

    if (g.mask.len > 0) g.allocator.free(g.mask);
    if (g.stroke_before) |b| g.allocator.free(b);
    clearMaskUndoRedo();
    freeDisplay();
    freeOrigCache();
    // Textures live in dvui; the backend releases them with the window.
    g.mask_tex = null;
    g.mask_tex_gen = 0xffff_ffff;

    for (&g.face_thumb) |*t| {
        if (t.img) |*im| im.deinit();
        t.img = null;
    }
    for (&g.identities) |*id| {
        if (id.thumb.img) |*im| im.deinit();
        id.thumb.img = null;
    }
    for (g.pending_identities.items) |*p| {
        var img = p.img;
        img.deinit();
    }
    g.pending_identities.deinit(g.allocator);
    for (&g.patches) |*set| {
        for (set) |*p| {
            if (p.valid) p.img.deinit();
        }
    }
    for (&g.undo) |*u| {
        if (u.*) |*im| im.deinit();
    }
    for (&g.redo) |*u| {
        if (u.*) |*im| im.deinit();
    }

    g.allocator.free(g.model_dir);
    g.allocator.free(g.temp_dir);
    g.allocator.free(g.export_dir);
    g.allocator.free(g.config_path);
    g.allocator.free(g.target_path);
    g.allocator.free(g.source_path);
    g.allocator.free(g.save_path);
}

// ---------------------------------------------------------------------------
// Settings persistence
// ---------------------------------------------------------------------------

fn setStr(dst: *[]u8, value: []const u8) void {
    const copy = g.allocator.dupe(u8, value) catch return;
    if (dst.len > 0) g.allocator.free(dst.*);
    dst.* = copy;
}

/// DVUI's dynamic text entries grow and shrink their backing slices in block
/// sizes and keep them zero-terminated. The logical path is the part before
/// the first NUL, so always normalize when reading one back out.
fn pathValue(s: []const u8) []const u8 {
    return s[0 .. std.mem.indexOfScalar(u8, s, 0) orelse s.len];
}

fn settingsValues() config.Values {
    return .{
        .provider = @intCast(@backingInt(g.provider)),
        .models_dir = pathValue(g.model_dir),
        .temp_dir = pathValue(g.temp_dir),
        .export_dir = pathValue(g.export_dir),
        .unload_after_run = g.unload_after_run,
        .check_updates = g.check_updates,
        .update_interval_hours = g.update_interval_hours,
        .last_update_check = g.last_update_check,
    };
}

fn markConfigDirty() void {
    g.config_dirty = true;
    saveConfig();
}

/// Wall-clock seconds since the epoch, from the process Io implementation.
fn nowSeconds() i64 {
    const init = dvui.App.main_init orelse return 0;
    const timestamp = std.Io.Clock.real.now(init.io);
    return @intCast(@divTrunc(timestamp.nanoseconds, std.time.ns_per_s));
}

fn saveConfig() void {
    if (!g.config_enabled or !g.config_dirty) return;
    if (config.save(g.allocator, g.config_path, settingsValues())) g.config_dirty = false;
}

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------

pub fn appFrame() !dvui.App.Result {
    consumeDrop();
    pollJob();
    if (g.selftest == .done) return .close;
    if (video.testDone()) return .close;
    pollMemory();
    handleShortcuts();

    var root = dvui.box(@src(), .{ .dir = .vertical }, .{ .expand = .both, .style = .window });
    drawTopBar();

    {
        var body = dvui.box(@src(), .{ .dir = .horizontal }, .{
            .expand = .both,
            .padding = .{ .x = 0, .y = 0, .w = 0, .h = 24 },
        });
        defer body.deinit();

        if (g.mode == .video) {
            video.draw(videoHost());
        } else {
            drawLeftPanel();
            drawCanvas();
            drawRightPanel();
        }
    }

    drawStatusBar();
    root.deinit();

    // Floating panels live above the main layout and are toggled from the top
    // bar (or Ctrl+, / F1).
    drawSettingsWindow();
    drawAboutWindow();

    // Keep frames coming while a worker owns the engine so progress and the
    // completion are always picked up.
    if (busy() or video.active()) dvui.refresh(null, @src(), null);
    return .ok;
}

// ---------------------------------------------------------------------------
// Theme
// ---------------------------------------------------------------------------

fn makeTheme() dvui.Theme {
    var t = dvui.Theme.builtin.adwaita_dark;
    t.fill = .{ .r = 0x17, .g = 0x19, .b = 0x1d };
    t.text = .{ .r = 0xe6, .g = 0xe9, .b = 0xee };
    t.border = .{ .r = 0x2c, .g = 0x30, .b = 0x37 };
    t.focus = .{ .r = 0x4c, .g = 0x8b, .b = 0xf5 };
    t.window = .{
        .fill = .{ .r = 0x1f, .g = 0x22, .b = 0x27 },
        .border = .{ .r = 0x2c, .g = 0x30, .b = 0x37 },
        .text = .{ .r = 0xf0, .g = 0xf2, .b = 0xf6 },
    };
    t.control = .{
        .fill = .{ .r = 0x2c, .g = 0x31, .b = 0x39 },
        .fill_hover = .{ .r = 0x38, .g = 0x3e, .b = 0x48 },
        .fill_press = .{ .r = 0x44, .g = 0x4c, .b = 0x58 },
        .text = .{ .r = 0xe8, .g = 0xeb, .b = 0xf0 },
        .border = .{ .r = 0x39, .g = 0x3f, .b = 0x48 },
    };
    t.highlight = .{
        .fill = .{ .r = 0x2f, .g = 0x5a, .b = 0xa8 },
        .fill_hover = .{ .r = 0x3a, .g = 0x6b, .b = 0xc2 },
        .text = .{ .r = 0xff, .g = 0xff, .b = 0xff },
    };
    t.err = .{
        .fill = .{ .r = 0x7a, .g = 0x2a, .b = 0x2a },
        .fill_hover = .{ .r = 0x92, .g = 0x34, .b = 0x34 },
        .text = .{ .r = 0xff, .g = 0xf0, .b = 0xf0 },
    };
    return t;
}

// ---------------------------------------------------------------------------
// Top bar
// ---------------------------------------------------------------------------

fn drawTopBar() void {
    var bar = dvui.box(@src(), .{ .dir = .horizontal }, .{
        .expand = .horizontal,
        .padding = .{ .x = 12, .y = 8, .w = 12, .h = 8 },
        .background = true,
        .style = .window,
        .min_size_content = .{ .h = 40 },
    });
    defer bar.deinit();

    dvui.label(@src(), "FaceForge", .{}, .{ .font = .theme(.title), .gravity_y = 0.5 });
    dvui.label(@src(), "restore · swap · enhance", .{}, .{
        .gravity_y = 0.5,
        .margin = .{ .x = 10, .y = 0, .w = 16, .h = 0 },
        .color_text = .{ .color = .{ .r = 0x8b, .g = 0x93, .b = 0xa0 } },
    });

    const btn_opts = dvui.Options{ .margin = .{ .x = 2, .y = 0, .w = 2, .h = 0 }, .min_size_content = .{ .h = 26 } };
    if (dvui.button(@src(), "Photo", .{}, btn_opts)) g.mode = .photo;
    if (dvui.button(@src(), "Video", .{}, btn_opts)) g.mode = .video;
    if (g.mode == .video) {
        if (dvui.button(@src(), "Open video…", .{}, btn_opts)) video.openDialog();
    } else if (dvui.button(@src(), "Open target…", .{}, btn_opts)) openTargetDialog();
    if (g.mode == .photo and dvui.button(@src(), "Paste target", .{}, btn_opts)) pasteTarget();
    if (dvui.button(@src(), "Add identity…", .{}, btn_opts)) openIdentityDialog();
    if (dvui.button(@src(), "Paste identity", .{}, btn_opts)) pasteIdentity();

    _ = dvui.spacer(@src(), .{ .expand = .horizontal, .min_size_content = .{ .w = 12 } });

    if (dvui.button(@src(), "Settings", .{}, btn_opts)) {
        g.show_settings = !g.show_settings;
        if (g.show_settings) g.show_about = false;
    }
    if (dvui.button(@src(), "About", .{}, btn_opts)) {
        g.show_about = !g.show_about;
        if (g.show_about) g.show_settings = false;
    }
    _ = dvui.spacer(@src(), .{ .min_size_content = .{ .w = 12 } });

    dvui.label(@src(), "VRAM {d:.1}/{d:.1} GB · RSS {d:.1} GB", .{ g.vram_used / 1024.0, g.vram_total / 1024.0, g.rss / 1024.0 }, .{
        .gravity_y = 0.5,
        .margin = .{ .x = 10, .y = 0, .w = 0, .h = 0 },
        .color_text = .{ .color = .{ .r = 0x8b, .g = 0x93, .b = 0xa0 } },
    });
}

// ---------------------------------------------------------------------------
// Floating panels: Settings and About
// ---------------------------------------------------------------------------

const muted: dvui.Options = .{ .color_text = .{ .color = .{ .r = 0x8b, .g = 0x93, .b = 0xa0 } } };
const dim: dvui.Options = .{ .color_text = .{ .color = .{ .r = 0x74, .g = 0x7d, .b = 0x89 } } };

/// Global preferences that are rarely touched: execution provider, folders for
/// models/cache/exports and update checks. The frequently used quality knobs
/// stay in the workspace panels. Non-modal so a provider change can be seen in
/// the canvas.
fn drawSettingsWindow() void {
    if (!g.show_settings) return;

    var fwin = dvui.floatingWindow(@src(), .{ .open_flag = &g.show_settings }, .{
        .min_size_content = .{ .w = 500 },
        .max_size_content = .{ .w = 700, .h = dvui.max_float_safe },
    });
    defer fwin.deinit();
    fwin.dragAreaSet(dvui.windowHeader("Settings", "", &g.show_settings));

    var scroll = dvui.scrollArea(@src(), .{}, .{ .expand = .both, .style = .window });
    defer scroll.deinit();

    sectionLabel("Engine");
    {
        var row = dvui.box(@src(), .{ .dir = .horizontal }, .{ .expand = .horizontal });
        defer row.deinit();
        dvui.label(@src(), "Device", .{}, .{ .gravity_y = 0.5, .min_size_content = .{ .w = 74 } });
        var choice = @backingInt(g.provider);
        if (dvui.dropdown(@src(), &.{ "Auto (GPU first)", "CUDA", "CPU" }, .{ .choice = &choice }, .{}, .{
            .expand = .horizontal,
            .min_size_content = .{ .h = 26 },
        })) {
            g.provider = @fromBackingInt(@intCast(choice));
            markConfigDirty();
            requestReload();
        }
    }
    if (dvui.checkbox(@src(), &g.unload_after_run, "Unload models between runs (saves VRAM)", .{})) {
        markConfigDirty();
    }
    dvui.label(@src(), "Active engine: {s}", .{if (g.engine == null) "not loaded" else providerName()}, muted);

    sectionLabel("Folders");
    if (settingsPathRow("Models", &g.model_dir, "Select the model folder", false)) {
        markConfigDirty();
    }
    if (settingsPathRow("Temp", &g.temp_dir, "Select the temporary/work folder", true)) {
        markConfigDirty();
        video.refreshPaths();
    }
    if (settingsPathRow("Exports", &g.export_dir, "Select the default export folder", true)) {
        markConfigDirty();
        if (!g.save_explicit and g.target_path.len > 0) defaultSavePathFor(g.target_path);
    }
    dvui.label(@src(), "Empty Temp means <video folder>/.faceforge.cache; empty Exports means next to the input file.", .{}, dim);
    dvui.label(@src(), "Saved and exported files are named <name>_faceforge.png / .mp4.", .{}, dim);

    if (dvui.button(@src(), "Reload models", .{}, .{ .expand = .horizontal }) and !busy()) {
        if (g.engine) |*e| {
            e.deinit();
            g.engine = null;
        }
        spawnJob(.init, -1);
    }

    sectionLabel("Updates");
    if (dvui.checkbox(@src(), &g.check_updates, "Check GitHub releases automatically", .{})) {
        markConfigDirty();
    }
    {
        var row = dvui.box(@src(), .{ .dir = .horizontal }, .{ .expand = .horizontal });
        defer row.deinit();
        dvui.label(@src(), "Interval", .{}, .{ .gravity_y = 0.5, .min_size_content = .{ .w = 74 } });
        var choice: usize = 1;
        for (update_intervals, 0..) |iv, i| {
            if (iv.hours == g.update_interval_hours) choice = i;
        }
        var labels: [update_intervals.len][]const u8 = undefined;
        for (update_intervals, 0..) |iv, i| labels[i] = iv.label;
        if (dvui.dropdown(@src(), &labels, .{ .choice = &choice }, .{}, .{
            .expand = .horizontal,
            .min_size_content = .{ .h = 26 },
        })) {
            g.update_interval_hours = update_intervals[choice].hours;
            markConfigDirty();
        }
    }
    {
        var row = dvui.box(@src(), .{ .dir = .horizontal }, .{ .expand = .horizontal });
        defer row.deinit();
        if (dvui.button(@src(), if (update.isCheckingAt(nowSeconds())) "Checking..." else "Check now", .{}, .{ .expand = .horizontal })) {
            if (dvui.App.main_init) |i| {
                update.check(app_version, i.io);
                g.last_update_check = nowSeconds();
                markConfigDirty();
            }
        }
        if (dvui.button(@src(), "Release page", .{}, .{ .expand = .horizontal })) {
            _ = dvui.openURL(.{ .url = update.releases_url, .new_window = false });
        }
    }

    sectionLabel("Configuration");
    var cfg = dvui.textLayout(@src(), .{}, .{ .background = false });
    if (g.config_enabled) {
        cfg.addText("Saved automatically to:\n", .{});
        cfg.addText(g.config_path, .{});
    } else {
        cfg.addText("Persistence is disabled (--no-config or a test run).", .{});
    }
    cfg.deinit();

    _ = dvui.spacer(@src(), .{ .min_size_content = .{ .h = 8 } });
    if (dvui.button(@src(), "Done", .{}, .{ .expand = .horizontal })) g.show_settings = false;
}

/// One labelled folder field with a native folder picker. Returns true when
/// the text or the picked value changed.
fn settingsPathRow(label: []const u8, backing: *[]u8, title: []const u8, allow_empty: bool) bool {
    var changed = false;
    // Every call site shares the same source location, so each row needs its
    // own id namespace or DVUI reports duplicate widget ids and the rows
    // scribble over each other's state.
    const extra: usize = @truncate(std.hash.Wyhash.hash(0, label));
    var row = dvui.box(@src(), .{ .dir = .horizontal }, .{ .expand = .horizontal, .id_extra = extra });
    defer row.deinit();

    dvui.labelNoFmt(@src(), label, .{}, .{
        .gravity_y = 0.5,
        .min_size_content = .{ .w = 74 },
        .id_extra = extra,
    });
    var te = dvui.textEntry(@src(), .{
        .text = .{ .buffer_dynamic = .{ .backing = backing, .allocator = g.allocator } },
    }, .{ .expand = .horizontal, .min_size_content = .{ .h = 26 }, .id_extra = extra });
    if (te.text_changed) changed = true;
    te.deinit();

    if (dvui.button(@src(), "...", .{}, .{ .min_size_content = .{ .w = 32, .h = 26 }, .id_extra = extra })) {
        const start = if (pathValue(backing.*).len > 0 and !allow_empty) pathValue(backing.*) else null;
        const chosen = dvui.dialogNativeFolderSelect(g.allocator, .{
            .title = title,
            .path = start,
        }) catch null;
        if (chosen) |p| {
            defer g.allocator.free(p);
            setStr(backing, p);
            changed = true;
        }
    }
    return changed;
}

/// Version, author, runtime status, update state, pipeline stages and model
/// licensing notes.
fn drawAboutWindow() void {
    if (!g.show_about) return;

    var fwin = dvui.floatingWindow(@src(), .{ .open_flag = &g.show_about }, .{
        .min_size_content = .{ .w = 500 },
        .max_size_content = .{ .w = 700, .h = dvui.max_float_safe },
    });
    defer fwin.deinit();
    fwin.dragAreaSet(dvui.windowHeader("About FaceForge", "", &g.show_about));

    var scroll = dvui.scrollArea(@src(), .{}, .{ .expand = .both, .style = .window });
    defer scroll.deinit();

    dvui.label(@src(), "FaceForge", .{}, .{ .font = .theme(.title) });
    dvui.label(@src(), "Native face restoration and face swap", .{}, muted);
    dvui.label(@src(), "by vhanla", .{}, .{ .font = .theme(.heading) });
    dvui.link(@src(), .{ .url = repo_url, .label = repo_url }, .{});
    dvui.label(@src(), "Version {s}  ·  C ABI v{d}", .{ app_version, ffi.FF_ABI_VERSION }, .{});
    dvui.label(@src(), "GPU first with CPU fallback · built for 4 GB VRAM", .{}, dim);

    sectionLabel("Updates");
    const u = update.snapshot();
    dvui.label(@src(), "{s}", .{updateStatusText(&u, nowSeconds())}, .{});
    if (u.status == .available and u.tag().len > 0) {
        dvui.label(@src(), "Latest release: {s}", .{u.tag()}, .{});
        if (u.url().len > 0) dvui.link(@src(), .{ .url = u.url(), .label = "Open release page" }, .{});
    }
    {
        var row = dvui.box(@src(), .{ .dir = .horizontal }, .{ .expand = .horizontal });
        defer row.deinit();
        if (dvui.button(@src(), if (update.isCheckingAt(nowSeconds())) "Checking..." else "Check for updates", .{}, .{ .expand = .horizontal })) {
            if (dvui.App.main_init) |i| {
                update.check(app_version, i.io);
                g.last_update_check = nowSeconds();
                markConfigDirty();
            }
        }
        var date_buf: [32]u8 = undefined;
        dvui.label(@src(), "Last check: {s}", .{formatDate(&date_buf, g.last_update_check)}, .{
            .gravity_y = 0.5,
            .margin = .{ .x = 8, .y = 0, .w = 0, .h = 0 },
        });
    }

    sectionLabel("Runtime");
    dvui.label(@src(), "Engine: {s}", .{if (g.engine == null) "not loaded" else providerName()}, .{});
    dvui.label(@src(), "Models: {s}", .{if (pathValue(g.model_dir).len == 0) "(default)" else pathValue(g.model_dir)}, .{});
    dvui.label(@src(), "VRAM {d:.1}/{d:.1} GB  ·  RSS {d:.1} GB", .{ g.vram_used / 1024.0, g.vram_total / 1024.0, g.rss / 1024.0 }, .{});

    sectionLabel("Pipeline");
    var pipe = dvui.textLayout(@src(), .{}, .{ .background = false });
    pipe.addText(
        "Detect    SCRFD det_10g\n" ++
            "Embed     ArcFace w600k_r50\n" ++
            "Swap      inswapper_128\n" ++
            "Enhance   GFPGAN 1.4 or Real-ESRGAN x2/x4\n",
        .{},
    );
    pipe.deinit();

    sectionLabel("Models and licenses");
    var lic = dvui.textLayout(@src(), .{}, .{ .background = false });
    lic.addText(
        "Model weights are not bundled with this application. Fetch them with " ++
            "scripts/fetch_models.sh (or fetch_models.ps1) and review their licenses before use.\n\n" ++
            "InsightFace (SCRFD, ArcFace) and inswapper are released for non-commercial " ++
            "use. GFPGAN and Real-ESRGAN follow their respective upstream licenses.",
        .{},
    );
    lic.deinit();

    sectionLabel("Built with");
    var built = dvui.textLayout(@src(), .{}, .{ .background = false });
    built.addText("C++17 · ONNX Runtime · OpenCV · Zig · DVUI · SDL3 · FFmpeg", .{});
    built.deinit();

    _ = dvui.spacer(@src(), .{ .min_size_content = .{ .h = 8 } });
    if (dvui.button(@src(), "Close", .{}, .{ .expand = .horizontal })) g.show_about = false;
}

fn updateStatusText(u: *const update.Snapshot, now: i64) []const u8 {
    return switch (u.status) {
        .idle => "Not checked yet.",
        .checking => if (update.isCheckingAt(now)) "Checking GitHub..." else "The update check timed out - try again.",
        .up_to_date, .available, .failed => u.message(),
    };
}

fn formatDate(buf: []u8, timestamp: i64) []const u8 {
    if (timestamp <= 0) return "never";
    const seconds: u64 = @intCast(timestamp);
    const epoch_seconds = std.time.epoch.EpochSeconds{ .secs = seconds };
    const day = epoch_seconds.getEpochDay();
    const year_day = day.calculateYearDay();
    const month_day = year_day.calculateMonthDay();
    return std.fmt.bufPrint(buf, "{d}-{d:0>2}-{d:0>2}", .{
        year_day.year,
        month_day.month.numeric(),
        month_day.day_index + 1,
    }) catch "?";
}

// ---------------------------------------------------------------------------
// Left rail: identity, quality, history, output
// ---------------------------------------------------------------------------

fn drawLeftPanel() void {
    var scroll = dvui.scrollArea(@src(), .{}, .{
        .expand = .vertical,
        .min_size_content = .{ .w = 270 },
        .max_size_content = .{ .w = 300, .h = dvui.max_float_safe },
        .padding = .{ .x = 10, .y = 10, .w = 10, .h = 30 },
    });
    defer scroll.deinit();
    g.left_panel_edge = rectRightNatural(&scroll.data().rectScale());

    sectionLabel("Identities");

    if (g.identity_count == 0) {
        dvui.label(@src(), "No identities yet.", .{}, .{
            .color_text = .{ .color = .{ .r = 0x8b, .g = 0x93, .b = 0xa0 } },
        });
        dvui.label(@src(), "Add a face photo to swap.", .{}, .{
            .color_text = .{ .color = .{ .r = 0x8b, .g = 0x93, .b = 0xa0 } },
        });
    } else {
        dvui.label(@src(), "default #{d} · {d} in library", .{ g.default_identity + 1, g.identity_count }, .{
            .color_text = .{ .color = .{ .r = 0x8b, .g = 0x93, .b = 0xa0 } },
        });
        drawIdentityGrid();
    }
    {
        var row = dvui.box(@src(), .{ .dir = .horizontal }, .{ .expand = .horizontal });
        defer row.deinit();
        if (dvui.button(@src(), "Add…", .{}, .{ .expand = .horizontal })) openIdentityDialog();
        if (dvui.button(@src(), "Clear", .{}, .{}) and !busy()) clearIdentities();
    }

    sectionLabel("Quality");
    {
        var row = dvui.box(@src(), .{ .dir = .horizontal }, .{ .expand = .horizontal });
        defer row.deinit();
        dvui.label(@src(), "Enhancer", .{}, .{ .gravity_y = 0.5 });
        var choice = @backingInt(g.enhancer);
        if (dvui.dropdown(@src(), &.{ "None", "GFPGAN 1.4", "Real-ESRGAN x2", "Real-ESRGAN x4" }, .{ .choice = &choice }, .{}, .{ .expand = .horizontal })) {
            g.enhancer = @fromBackingInt(@intCast(choice));
            if (g.base != null) spawnJob(.compose, -1);
        }
    }
    if (dvui.checkbox(@src(), &g.restore_mouth, "Restore mouths from original", .{})) {
        if (g.base != null) spawnJob(.process_all, -1);
    }
    if (dvui.checkbox(@src(), &g.fast_preview, "Fast preview (downscaled)", .{})) {
        // Rebuilding the working image is cheap; the per-level patch cache means
        // only faces not yet swapped at the new resolution are reprocessed.
        if (g.base != null) spawnJob(.process_all, -1);
    }
    _ = dvui.checkbox(@src(), &g.unload_after_run, "Unload models between runs", .{});

    sectionLabel("History");
    {
        var row = dvui.box(@src(), .{ .dir = .horizontal }, .{ .expand = .horizontal });
        defer row.deinit();
        if (dvui.button(@src(), "Apply as input", .{}, .{ .expand = .horizontal }) and !busy()) applyResultAsInput();
    }
    {
        var row = dvui.box(@src(), .{ .dir = .horizontal }, .{ .expand = .horizontal });
        defer row.deinit();
        if (dvui.button(@src(), "Undo", .{}, .{ .expand = .horizontal })) undoHistory();
        if (dvui.button(@src(), "Redo", .{}, .{ .expand = .horizontal })) redoHistory();
    }
    dvui.label(@src(), "undo {d} · redo {d}", .{ g.undo_len, g.redo_len }, .{
        .color_text = .{ .color = .{ .r = 0x8b, .g = 0x93, .b = 0xa0 } },
    });

    sectionLabel("Studio");
    {
        var row = dvui.box(@src(), .{ .dir = .horizontal }, .{ .expand = .horizontal });
        defer row.deinit();
        if (!g.studio) {
            if (dvui.button(@src(), "Open Studio", .{}, .{ .expand = .horizontal })) {
                if (g.result == null) {
                    setStatus("Run a job first — Studio retouches the result", true);
                } else {
                    g.studio = true;
                    g.studio_view_original = false;
                    // A brush that feels right at this image size.
                    if (g.base) |b| {
                        const long: f32 = @floatFromInt(@max(b.width, b.height));
                        g.studio_size = std.math.clamp(long * 0.07, 10.0, 300.0);
                    }
                    if (!maskEnsure()) {
                        setStatus("Cannot allocate the retouch mask", true);
                    } else {
                        setStatus("Studio · paint the original back · right-click flips brush", false);
                    }
                }
            }
        } else {
            if (dvui.button(@src(), "Close Studio", .{}, .{ .expand = .horizontal })) {
                strokeEnd();
                g.studio = false;
                g.studio_view_original = false;
                g.studio_hover = null;
            }
            if (dvui.button(@src(), "Clear", .{}, .{})) maskReset();
        }
    }
    if (!g.studio) {
        if (g.mask_used) {
            dvui.label(@src(), "Retouch active — included in save/copy", .{}, .{
                .color_text = .{ .color = .{ .r = 0xc9, .g = 0xb2, .b = 0xff } },
            });
        } else {
            dvui.label(@src(), "Paint the original back over the result.", .{}, .{
                .color_text = .{ .color = .{ .r = 0x8b, .g = 0x93, .b = 0xa0 } },
            });
            dvui.label(@src(), "Erase added hair, blemishes, halos…", .{}, .{
                .color_text = .{ .color = .{ .r = 0x8b, .g = 0x93, .b = 0xa0 } },
            });
        }
    } else {
        {
            var row = dvui.box(@src(), .{ .dir = .horizontal }, .{ .expand = .horizontal });
            defer row.deinit();
            const erase_on = g.studio_erase;
            const st_on: dvui.Options = .{
                .expand = .horizontal,
                .background = true,
                .color_fill = .{ .color = .{ .r = 0x7a, .g = 0x33, .b = 0x30 } },
                .color_text = .{ .color = .{ .r = 0xff, .g = 0xd2, .b = 0xcc } },
            };
            const st_off: dvui.Options = .{ .expand = .horizontal };
            if (dvui.button(@src(), "Erase", .{}, if (erase_on) st_on else st_off)) g.studio_erase = true;
            const rs_on: dvui.Options = .{
                .expand = .horizontal,
                .background = true,
                .color_fill = .{ .color = .{ .r = 0x25, .g = 0x5c, .b = 0x39 } },
                .color_text = .{ .color = .{ .r = 0xd3, .g = 0xff, .b = 0xe2 } },
            };
            if (dvui.button(@src(), "Restore", .{}, if (!erase_on) rs_on else st_off)) g.studio_erase = false;
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

        _ = dvui.checkbox(@src(), &g.studio_show_mask, "Show mask (red)", .{});
        _ = dvui.checkbox(@src(), &g.studio_view_original, "View original", .{});
        {
            var row = dvui.box(@src(), .{ .dir = .horizontal }, .{ .expand = .horizontal });
            defer row.deinit();
            if (dvui.button(@src(), "Undo", .{}, .{ .expand = .horizontal })) maskUndo();
            if (dvui.button(@src(), "Redo", .{}, .{ .expand = .horizontal })) maskRedo();
        }
        dvui.label(@src(), "strokes {d} · redo {d}", .{ g.mask_undo_len, g.mask_redo_len }, .{
            .color_text = .{ .color = .{ .r = 0x8b, .g = 0x93, .b = 0xa0 } },
        });
    }

    sectionLabel("Output");
    {
        var row = dvui.box(@src(), .{ .dir = .horizontal }, .{ .expand = .horizontal });
        defer row.deinit();
        var te = dvui.textEntry(@src(), .{
            .text = .{ .buffer_dynamic = .{ .backing = &g.save_path, .allocator = g.allocator } },
        }, .{ .expand = .horizontal, .min_size_content = .{ .h = 26 } });
        if (te.text_changed) g.save_explicit = true;
        te.deinit();
    }
    {
        var row = dvui.box(@src(), .{ .dir = .horizontal }, .{ .expand = .horizontal });
        defer row.deinit();
        if (dvui.button(@src(), "Save as…", .{}, .{ .expand = .horizontal })) saveResultDialog();
        if (dvui.button(@src(), "Copy", .{}, .{ .expand = .horizontal })) copyResult();
    }
    {
        var row = dvui.box(@src(), .{ .dir = .horizontal }, .{ .expand = .horizontal });
        defer row.deinit();
        if (dvui.button(@src(), "Process all", .{}, .{ .expand = .horizontal }) and !busy()) spawnJob(.process_all, -1);
        if (dvui.button(@src(), "Clear", .{}, .{}) and !busy()) clearResult();
    }
}

/// Thumbnail grid of every identity face in the library. Clicking one makes it
/// the default identity, used by any target face without an explicit choice.
fn drawIdentityGrid() void {
    const cols = 4;
    var i: usize = 0;
    while (i < g.identity_count) : (i += cols) {
        var row = dvui.box(@src(), .{ .dir = .horizontal }, .{ .expand = .horizontal });
        defer row.deinit();
        var c: usize = 0;
        while (c < cols and i + c < g.identity_count) : (c += 1) {
            const idx = i + c;
            var cell = dvui.box(@src(), .{}, .{ .min_size_content = .{ .w = 56, .h = 56 }, .margin = .all(2), .id_extra = idx });
            defer cell.deinit();
            const crs = cell.data().contentRectScale();
            if (g.identities[idx].thumb.tex) |tex| {
                var tw = crs.r.w;
                var th = crs.r.h;
                if (tex.width > 0 and tex.height > 0) {
                    const aspect = @as(f32, @floatFromInt(tex.width)) / @as(f32, @floatFromInt(tex.height));
                    if (tw / th > aspect) tw = th * aspect else th = tw / aspect;
                }
                const r = dvui.Rect.Physical{ .x = crs.r.x + (crs.r.w - tw) * 0.5, .y = crs.r.y + (crs.r.h - th) * 0.5, .w = tw, .h = th };
                dvui.renderTexture(tex, .{ .r = r, .s = crs.s }, .{ .corners = .all(4) }) catch {};
            }
            if (widgetClicked(cell.data())) selectDefaultIdentity(idx);
            if (idx == g.default_identity) outlinePhysical(crs.r, 2, .{ .r = 0x4c, .g = 0x8b, .b = 0xf5 });

            dvui.tooltip(@src(), .{ .active_rect = crs.r, .delay = 400_000 }, "{s}\nclick = default · right-click = menu", .{
                g.identities[idx].labelSlice(),
            }, .{ .id_extra = idx });

            // Right-click menu: make default / remove / add.
            {
                const ctext = dvui.context(@src(), .{ .rect = crs.r }, .{ .id_extra = idx });
                defer ctext.deinit();
                if (ctext.activePoint()) |cp| {
                    var fw = dvui.floatingMenu(@src(), .{ .from = dvui.Rect.Natural.fromPoint(cp) }, .{});
                    defer fw.deinit();
                    if (dvui.menuItemLabel(@src(), "Set as default", .{}, .{ .expand = .horizontal }) != null) {
                        fw.close();
                        selectDefaultIdentity(idx);
                    }
                    if (dvui.menuItemLabel(@src(), "Remove identity", .{}, .{ .expand = .horizontal }) != null) {
                        fw.close();
                        removeIdentity(idx);
                    }
                    if (dvui.menuItemLabel(@src(), "Add identity…", .{}, .{ .expand = .horizontal }) != null) {
                        fw.close();
                        openIdentityDialog();
                    }
                }
            }
        }
    }
}

fn selectDefaultIdentity(idx: usize) void {
    if (idx >= g.identity_count) return;
    g.default_identity = idx;
    setStatusFmt("Default identity #{d}", .{idx + 1}, false);
    if (g.face_count > 0) spawnJob(.process_all, -1);
}

/// Drop one identity from the library. Faces pointing at it fall back to the
/// default; indices above it shift down so per-face choices stay correct.
fn removeIdentity(idx: usize) void {
    if (busy() or idx >= g.identity_count) return;
    freeThumb(&g.identities[idx].thumb);
    var k = idx;
    while (k + 1 < g.identity_count) : (k += 1) g.identities[k] = g.identities[k + 1];
    g.identities[g.identity_count - 1] = Identity{};
    g.identity_count -= 1;

    for (0..g.face_count) |i| {
        const v = g.face_identity[i];
        if (v < 0) continue;
        const vi: usize = @intCast(v);
        g.face_identity[i] = if (vi == idx) -1 else if (vi > idx) @intCast(vi - 1) else v;
    }
    if (g.default_identity == idx or g.default_identity >= g.identity_count) g.default_identity = 0;
    if (g.default_identity > idx) g.default_identity -= 1;

    // Identity indices moved, so every cached patch must be re-validated.
    g.identity_gen +%= 1;
    if (g.identity_count == 0) {
        clearPatches();
        if (g.base != null) spawnJob(.compose, -1);
    } else if (g.base != null) {
        spawnJob(.process_all, -1);
    }
    setStatusFmt("Removed identity #{d}", .{idx + 1}, false);
}

fn clearIdentities() void {
    if (busy()) return;
    for (&g.identities) |*id| {
        freeThumb(&id.thumb);
        id.* = Identity{};
    }
    g.identity_count = 0;
    g.default_identity = 0;
    g.identity_gen +%= 1;
    for (0..g.face_count) |i| {
        g.face_identity[i] = -1;
        g.face_mouth_over[i] = null;
    }
    clearPatches();
    if (g.base != null) spawnJob(.compose, -1);
    setStatus("Identity library cleared", false);
}

// ---------------------------------------------------------------------------
// Canvas
// ---------------------------------------------------------------------------

fn drawCanvas() void {
    var area = dvui.box(@src(), .{ .dir = .vertical }, .{
        .expand = .both,
        .padding = .all(8),
        .min_size_content = .{ .w = 300, .h = 300 },
    });
    defer area.deinit();

    // toolbar
    {
        var row = dvui.box(@src(), .{ .dir = .horizontal }, .{ .expand = .horizontal, .padding = .{ .y = 2, .h = 6 }, .min_size_content = .{ .h = 28 } });
        defer row.deinit();
        if (g.studio) {
            dvui.label(@src(), "STUDIO", .{}, .{
                .gravity_y = 0.5,
                .background = true,
                .color_fill = .{ .color = .{ .r = 0x35, .g = 0x2a, .b = 0x4a } },
                .color_text = .{ .color = .{ .r = 0xc9, .g = 0xb2, .b = 0xff } },
                .padding = .{ .x = 6, .y = 2, .w = 6, .h = 2 },
                .corners = .all(3),
            });
            if (g.studio_erase) {
                dvui.label(@src(), "erase -> original", .{}, .{
                    .gravity_y = 0.5,
                    .margin = .{ .x = 6, .y = 0, .w = 6, .h = 0 },
                    .color_text = .{ .color = .{ .r = 0xff, .g = 0x8b, .b = 0x7e } },
                });
            } else {
                dvui.label(@src(), "restore -> result", .{}, .{
                    .gravity_y = 0.5,
                    .margin = .{ .x = 6, .y = 0, .w = 6, .h = 0 },
                    .color_text = .{ .color = .{ .r = 0x6e, .g = 0xe0, .b = 0x93 } },
                });
            }
        } else {
            _ = dvui.checkbox(@src(), &g.show_overlay, "Overlay", .{ .gravity_y = 0.5 });
            _ = dvui.checkbox(@src(), &g.photo_view_original, "Original", .{ .gravity_y = 0.5, .margin = .{ .x = 8, .y = 0, .w = 4, .h = 0 } });
        }
        if (dvui.button(@src(), "Fit", .{}, .{})) {
            g.zoom = 1.0;
            g.pan_x = 0;
            g.pan_y = 0;
        }
        if (dvui.button(@src(), "1:1", .{}, .{})) setOneToOne();
        dvui.label(@src(), "zoom {d:.0}%", .{g.zoom * 100.0}, .{ .gravity_y = 0.5, .margin = .{ .x = 6, .y = 0, .w = 6, .h = 0 } });
        {
            var hint = dvui.box(@src(), .{ .dir = .horizontal }, .{ .expand = .horizontal, .gravity_x = 1.0, .gravity_y = 0.5 });
            defer hint.deinit();
            if (g.studio) {
                dvui.label(@src(), "drag = paint · right-click = flip · Ctrl+Z = undo", .{}, .{
                    .gravity_y = 0.5,
                    .color_text = .{ .color = .{ .r = 0x7d, .g = 0x86, .b = 0x93 } },
                });
            } else {
                dvui.label(@src(), "click face = toggle · right-click = menu · wheel = zoom · middle-drag = pan", .{}, .{
                    .gravity_y = 0.5,
                    .color_text = .{ .color = .{ .r = 0x7d, .g = 0x86, .b = 0x93 } },
                });
            }
        }
    }

    var canvas = dvui.box(@src(), .{}, .{
        .expand = .both,
        .background = true,
        .style = .content,
        .color_fill = .{ .color = .{ .r = 0x10, .g = 0x11, .b = 0x14 } },
        .min_size_content = .{ .w = 200, .h = 200 },
    });
    defer canvas.deinit();

    const crs = canvas.data().contentRectScale();
    const s = crs.s;
    const cw = crs.r.w / s;
    const ch = crs.r.h / s;
    g.view_w = cw;
    g.view_h = ch;

    const base = g.base orelse {
        dvui.label(@src(), "Drop an image, paste from the clipboard, or open a file to begin.", .{}, .{
            .gravity_x = 0.5,
            .gravity_y = 0.5,
            .color_text = .{ .color = .{ .r = 0x8b, .g = 0x93, .b = 0xa0 } },
        });
        return;
    };

    const bw: f32 = @floatFromInt(base.width);
    const bh: f32 = @floatFromInt(base.height);
    const fit = @min(cw / bw, ch / bh);
    g.view_fit = fit;
    const scale = fit * g.zoom;
    const iw = bw * scale;
    const ih = bh * scale;
    const ix = (cw - iw) * 0.5 + g.pan_x;
    const iy = (ch - ih) * 0.5 + g.pan_y;
    const rs = crs.rectToRectScale(.{ .x = ix, .y = iy, .w = iw, .h = ih });

    // --- events ----------------------------------------------------------
    const evts = dvui.events();
    for (evts) |*e| {
        if (!dvui.eventMatch(e, .{ .id = canvas.data().id, .r = crs.r })) continue;
        switch (e.evt) {
            .mouse => |me| switch (me.action) {
                .press => {
                    if (me.button == .middle) {
                        g.drag_pan = true;
                        dvui.captureMouse(canvas.data(), e.num);
                        e.handle(@src(), canvas.data());
                    } else if (g.studio and me.button == .right) {
                        // Right-click flips the brush direction, matching the
                        // quick erase/restore swap of the web editor.
                        g.studio_erase = !g.studio_erase;
                        setStatusFmt("Brush: {s}", .{if (g.studio_erase) "erase (reveal original)" else "restore result"}, false);
                        e.handle(@src(), canvas.data());
                    } else if (g.studio and me.button == .left) {
                        const lx = (me.p.x - crs.r.x) / s;
                        const ly = (me.p.y - crs.r.y) / s;
                        const bx = (lx - ix) / scale;
                        const by = (ly - iy) / scale;
                        if (bx >= 0 and by >= 0 and bx <= bw and by <= bh) {
                            strokeBegin(bx, by);
                            dvui.captureMouse(canvas.data(), e.num);
                        }
                        e.handle(@src(), canvas.data());
                    } else if (me.button == .left) {
                        const idx = hitFace(me.p, crs, ix, iy, scale, base);
                        if (idx >= 0) toggleFace(@intCast(idx));
                        e.handle(@src(), canvas.data());
                    }
                },
                .motion => |delta| {
                    if (g.studio and g.stroke_active and dvui.captured(canvas.data().id)) {
                        const lx = (me.p.x - crs.r.x) / s;
                        const ly = (me.p.y - crs.r.y) / s;
                        strokeTo((lx - ix) / scale, (ly - iy) / scale);
                        dvui.cursorSet(.crosshair);
                        e.handle(@src(), canvas.data());
                    } else if (g.drag_pan and dvui.captured(canvas.data().id)) {
                        g.pan_x += delta.x / s;
                        g.pan_y += delta.y / s;
                        dvui.cursorSet(.arrow_all);
                        e.handle(@src(), canvas.data());
                    }
                },
                .release => {
                    if (me.button == .middle and dvui.captured(canvas.data().id)) {
                        g.drag_pan = false;
                        dvui.captureMouse(null, e.num);
                        e.handle(@src(), canvas.data());
                    } else if (me.button == .left and g.studio and g.stroke_active) {
                        strokeEnd();
                        dvui.captureMouse(null, e.num);
                        e.handle(@src(), canvas.data());
                    }
                },
                .wheel_y => |dy| {
                    const anchor_x = (me.p.x - crs.r.x) / s;
                    const anchor_y = (me.p.y - crs.r.y) / s;
                    const old = g.zoom;
                    const factor = std.math.pow(f32, 1.0018, dy);
                    g.zoom = std.math.clamp(old * factor, 0.05, 32.0);
                    const ratio = g.zoom / old;
                    const cx = cw * 0.5;
                    const cy = ch * 0.5;
                    g.pan_x = anchor_x - cx - (anchor_x - cx - g.pan_x) * ratio;
                    g.pan_y = anchor_y - cy - (anchor_y - cy - g.pan_y) * ratio;
                    e.handle(@src(), canvas.data());
                },
                .position => {
                    if (g.studio) {
                        const lx = (me.p.x - crs.r.x) / s;
                        const ly = (me.p.y - crs.r.y) / s;
                        const bx = (lx - ix) / scale;
                        const by = (ly - iy) / scale;
                        if (bx >= 0 and by >= 0 and bx <= bw and by <= bh) {
                            g.studio_hover = me.p;
                            dvui.cursorSet(.crosshair);
                        } else {
                            g.studio_hover = null;
                        }
                    }
                },
                else => {},
            },
            else => {},
        }
    }

    // --- pixels ----------------------------------------------------------
    // Restrict everything the viewer draws (image, overlay boxes, badges) to
    // the canvas content rect, whatever the zoom and pan are. The context menu
    // is opened after this scope so it can extend past the canvas edge.
    {
        const prev_clip = dvui.clip(crs.r);
        defer dvui.clipSet(prev_clip);

        const tex: ?dvui.Texture = if (g.studio and g.studio_view_original)
            g.base_tex
        else if (!g.studio and g.photo_view_original)
            g.base_tex
        else
            (g.result_tex orelse g.base_tex);
        if (tex) |t| dvui.renderTexture(t, rs, .{}) catch {};

        // Retouch mask preview (red where the original shows through).
        if (g.studio and g.studio_show_mask and !g.studio_view_original and g.mask_used) {
            if (g.mask_tex) |mt| dvui.renderTexture(mt, rs, .{}) catch {};
        }

        // --- overlay ---------------------------------------------------------
        // Faces are stored in base-image pixels, so they map straight onto the
        // displayed (base-sized) texture. The face boxes are hidden in Studio so
        // they do not get in the way of painting.
        if (!g.studio and g.show_overlay and g.face_count > 0) {
            for (g.faces[0..g.face_count], 0..) |f, i| {
                const bx = ix + f.bbox[0] * scale;
                const by = iy + f.bbox[1] * scale;
                const bwd = (f.bbox[2] - f.bbox[0]) * scale;
                const bht = (f.bbox[3] - f.bbox[1]) * scale;
                const swapped = g.face_sel[i] and patchFresh(&g, i);
                const col: dvui.Color = if (swapped)
                    .{ .r = 0x54, .g = 0xd6, .b = 0x8a }
                else
                    .{ .r = 0x9a, .g = 0xa3, .b = 0xb0 };
                outline(crs, bx, by, bwd, bht, if (swapped) 2 else 1, col);
                faceBadge(bx, by, i, swapped);
            }
        }

        // Brush cursor: a ring the size of the actual base-pixel brush.
        if (g.studio and !g.studio_view_original) {
            if (g.studio_hover) |hp| {
                const bx = ((hp.x - crs.r.x) / s - ix) / scale;
                const by = ((hp.y - crs.r.y) / s - iy) / scale;
                const cx = crs.r.x + (ix + bx * scale) * s;
                const cy = crs.r.y + (iy + by * scale) * s;
                const radius = @max(1.0, g.studio_size * 0.5 * scale * s);
                const col: dvui.Color = if (g.studio_erase)
                    .{ .r = 0xff, .g = 0x6b, .b = 0x5e }
                else
                    .{ .r = 0x3e, .g = 0xcf, .b = 0x70 };
                var pb = dvui.Path.Builder.init(g.allocator);
                defer pb.deinit();
                pb.addArc(.{ .x = cx, .y = cy }, radius, std.math.pi * 2.0, 0.0, false);
                dvui.Path.stroke(pb.build(), .{ .thickness = 1.5, .color = .{ .color = col } });
                fillRect(.{ .x = cx - 1.0, .y = cy - 1.0, .w = 2.0, .h = 2.0 }, col);
            }
        }
    }

    // --- context menu ----------------------------------------------------
    // Studio owns the right button for brush direction, so the menu exists
    // only in the regular photo workspace.
    if (!g.studio) drawCanvasMenu(crs, ix, iy, scale, base);
}

/// Small numbered tag at the top-left corner of a detection box so the canvas
/// matches the face cards in the right rail.
fn faceBadge(x: f32, y: f32, i: usize, swapped: bool) void {
    var buf: [8]u8 = undefined;
    const txt = std.fmt.bufPrint(&buf, "#{d}", .{i}) catch return;
    const col: dvui.Color = if (swapped)
        .{ .r = 0x54, .g = 0xd6, .b = 0x8a }
    else
        .{ .r = 0xb0, .g = 0xb8, .b = 0xc4 };
    dvui.labelNoFmt(@src(), txt, .{}, .{
        .id_extra = 1000 + i,
        .rect = .{ .x = x, .y = y - 15, .w = 40, .h = 15 },
        .padding = .{ .x = 4, .y = 0, .w = 4, .h = 0 },
        .margin = .all(0),
        .background = true,
        .color_fill = .{ .color = .{ .r = 0x10, .g = 0x12, .b = 0x16, .a = 0xe0 } },
        .color_text = .{ .color = col },
        .border = .all(0),
        .font = .theme(.body),
    });
}

fn hitFace(p: dvui.Point.Physical, crs: dvui.RectScale, ix: f32, iy: f32, scale: f32, base: ff.Image) i32 {
    _ = base;
    const lx = (p.x - crs.r.x) / crs.s;
    const ly = (p.y - crs.r.y) / crs.s;
    // Faces are stored in base-image pixels.
    const px = (lx - ix) / scale;
    const py = (ly - iy) / scale;

    var best: i32 = -1;
    var best_area: f32 = std.math.floatMax(f32);
    for (g.faces[0..g.face_count], 0..) |f, i| {
        if (px >= f.bbox[0] and px <= f.bbox[2] and py >= f.bbox[1] and py <= f.bbox[3]) {
            const area = (f.bbox[2] - f.bbox[0]) * (f.bbox[3] - f.bbox[1]);
            if (area < best_area) {
                best_area = area;
                best = @intCast(i);
            }
        }
    }
    return best;
}

fn outline(crs: dvui.RectScale, x: f32, y: f32, w: f32, h: f32, t: f32, col: dvui.Color) void {
    const rx = crs.r.x + x * crs.s;
    const ry = crs.r.y + y * crs.s;
    const rw = w * crs.s;
    const rh = h * crs.s;
    const tt = @max(1, t * crs.s);
    fillRect(.{ .x = rx, .y = ry, .w = rw, .h = tt }, col);
    fillRect(.{ .x = rx, .y = ry + rh - tt, .w = rw, .h = tt }, col);
    fillRect(.{ .x = rx, .y = ry, .w = tt, .h = rh }, col);
    fillRect(.{ .x = rx + rw - tt, .y = ry, .w = tt, .h = rh }, col);
}

fn outlinePhysical(r: dvui.Rect.Physical, t: f32, col: dvui.Color) void {
    fillRect(.{ .x = r.x, .y = r.y, .w = r.w, .h = t }, col);
    fillRect(.{ .x = r.x, .y = r.y + r.h - t, .w = r.w, .h = t }, col);
    fillRect(.{ .x = r.x, .y = r.y, .w = t, .h = r.h }, col);
    fillRect(.{ .x = r.x + r.w - t, .y = r.y, .w = t, .h = r.h }, col);
}

fn fillRect(r: dvui.Rect.Physical, col: dvui.Color) void {
    r.fill(.{}, .{ .color = .{ .color = col } });
}

/// True on the frame a widget is left-clicked. Drawing-only widgets use this
/// to become interactive (thumbnail cells, previews).
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

fn setOneToOne() void {
    if (g.view_fit <= 0.0001) return;
    g.zoom = 1.0 / g.view_fit;
    g.pan_x = 0;
    g.pan_y = 0;
}

fn focusFace(i: usize) void {
    const base = g.base orelse return;
    if (i >= g.face_count) return;
    const f = g.faces[i];
    const w = @as(f32, @floatFromInt(base.width));
    const h = @as(f32, @floatFromInt(base.height));
    const cx = (f.bbox[0] + f.bbox[2]) * 0.5;
    const cy = (f.bbox[1] + f.bbox[3]) * 0.5;
    const bw = @max(1.0, f.bbox[2] - f.bbox[0]);
    const bh = @max(1.0, f.bbox[3] - f.bbox[1]);
    const fit = @min(g.view_w / w, g.view_h / h);
    const want = @min(g.view_w * 0.5 / bw, g.view_h * 0.5 / bh);
    g.zoom = std.math.clamp(want / @max(fit, 0.0001), 0.1, 24.0);
    const scale = fit * g.zoom;
    g.pan_x = g.view_w * 0.5 - cx * scale;
    g.pan_y = g.view_h * 0.5 - cy * scale;
}

// ---------------------------------------------------------------------------
// Canvas context menu
// ---------------------------------------------------------------------------

fn drawCanvasMenu(crs: dvui.RectScale, ix: f32, iy: f32, scale: f32, base: ff.Image) void {
    // ContextWidget consumes the right-press/focus events itself, so the menu
    // is opened exactly once per click and the canvas never has to guess when
    // to close it. `floatingMenu` closes on Escape, on a click outside, and
    // when an item calls `close()`; unlike the previous modal popup this can
    // never leave the app stuck behind an invisible open flag.
    const ctext = dvui.context(@src(), .{ .rect = crs.r }, .{});
    defer ctext.deinit();

    const cp = ctext.activePoint() orelse return;
    const pp = cp.scale(dvui.windowNaturalScale(), dvui.Point.Physical);
    const fi = hitFace(pp, crs, ix, iy, scale, base);

    var fw = dvui.floatingMenu(@src(), .{ .from = dvui.Rect.Natural.fromPoint(cp) }, .{
        .min_size_content = .{ .w = 230 },
    });
    defer fw.deinit();

    if (fi >= 0 and fi < @as(i32, @intCast(g.face_count))) {
        const i: usize = @intCast(fi);
        dvui.label(@src(), "Face #{d}", .{i}, .{ .font = .theme(.heading) });
        _ = dvui.separator(@src(), .{ .expand = .horizontal });
        const toggle_label = if (g.face_sel[i]) "Skip this face" else "Swap this face";
        if (dvui.menuItemLabel(@src(), toggle_label, .{}, .{ .expand = .horizontal })) |_| {
            toggleFace(i);
            fw.close();
        }
        if (dvui.menuItemLabel(@src(), "Reprocess this face", .{}, .{ .expand = .horizontal })) |_| {
            if (!busy()) spawnJob(.process_one, @intCast(i));
            fw.close();
        }
        if (dvui.menuItemLabel(@src(), "Zoom to face", .{}, .{ .expand = .horizontal })) |_| {
            focusFace(i);
            fw.close();
        }
        _ = dvui.separator(@src(), .{ .expand = .horizontal });
    }
    if (dvui.menuItemLabel(@src(), "Swap all faces", .{}, .{ .expand = .horizontal })) |_| {
        for (0..g.face_count) |i| g.face_sel[i] = true;
        if (!busy()) spawnJob(.process_all, -1);
        fw.close();
    }
    if (dvui.menuItemLabel(@src(), "Skip all faces", .{}, .{ .expand = .horizontal })) |_| {
        for (0..g.face_count) |i| g.face_sel[i] = false;
        if (!busy()) spawnJob(.compose, -1);
        fw.close();
    }
    _ = dvui.separator(@src(), .{ .expand = .horizontal });
    if (dvui.menuItemLabel(@src(), "Apply result as input", .{}, .{ .expand = .horizontal })) |_| {
        applyResultAsInput();
        fw.close();
    }
    if (dvui.menuItemLabel(@src(), "Undo", .{}, .{ .expand = .horizontal })) |_| {
        undoHistory();
        fw.close();
    }
}

// ---------------------------------------------------------------------------
// Right rail: target faces + stats
// ---------------------------------------------------------------------------

fn drawRightPanel() void {
    var panel = dvui.box(@src(), .{ .dir = .vertical }, .{
        .expand = .vertical,
        .min_size_content = .{ .w = 280 },
        .max_size_content = .{ .w = 320, .h = dvui.max_float_safe },
        .padding = .{ .x = 10, .y = 10, .w = 10, .h = 30 },
    });
    defer panel.deinit();

    sectionLabel("Detected faces");
    {
        var row = dvui.box(@src(), .{ .dir = .horizontal }, .{ .expand = .horizontal });
        defer row.deinit();
        if (dvui.button(@src(), "All", .{}, .{ .expand = .horizontal })) {
            for (0..g.face_count) |i| g.face_sel[i] = true;
            spawnJob(.process_all, -1);
        }
        if (dvui.button(@src(), "None", .{}, .{ .expand = .horizontal })) {
            for (0..g.face_count) |i| g.face_sel[i] = false;
            spawnJob(.compose, -1);
        }
        dvui.label(@src(), "{d} found", .{g.face_count}, .{ .gravity_y = 0.5, .margin = .{ .x = 6, .y = 0, .w = 0, .h = 0 } });
    }

    {
        // Only the face cards scroll; the statistics below stay pinned.
        var scroll = dvui.scrollArea(@src(), .{}, .{
            .expand = .vertical,
            .min_size_content = .{ .h = 120 },
            .max_size_content = .{ .w = dvui.max_float_safe, .h = @max(160.0, dvui.windowRect().h * 0.55) },
        });
        defer scroll.deinit();
        drawFaceCards();
    }

    _ = dvui.separator(@src(), .{ .expand = .horizontal, .margin = .{ .y = 6, .h = 2 } });
    drawStats();
}

fn drawFaceCards() void {
    if (g.face_count == 0) {
        dvui.label(@src(), "No faces detected yet.", .{}, .{ .color_text = .{ .color = .{ .r = 0x8b, .g = 0x93, .b = 0xa0 } } });
        return;
    }

    // Identity choices for the per-face pickers: index 0 is "use the default".
    var id_labels: [max_identities + 1][]const u8 = undefined;
    id_labels[0] = "(default)";
    for (0..g.identity_count) |k| id_labels[k + 1] = g.identities[k].labelSlice();

    for (0..g.face_count) |i| {
        var card = dvui.box(@src(), .{ .dir = .horizontal }, .{
            .expand = .horizontal,
            .margin = .{ .y = 3, .h = 3 },
            .padding = .all(4),
            .background = true,
            .id_extra = i,
            .color_fill = .{ .color = if (g.face_sel[i]) dvui.Color{ .r = 0x25, .g = 0x2a, .b = 0x32 } else dvui.Color{ .r = 0x1c, .g = 0x1f, .b = 0x24 } },
            .border = .all(1),
            .color_border = .{ .color = .{ .r = 0x33, .g = 0x38, .b = 0x41 } },
        });
        defer card.deinit();

        // thumbnail
        {
            var cell = dvui.box(@src(), .{}, .{ .min_size_content = .{ .w = 54, .h = 54 }, .id_extra = i });
            defer cell.deinit();
            const crs = cell.data().contentRectScale();
            if (g.face_thumb[i].tex) |tex| {
                var tw = crs.r.w;
                var th = crs.r.h;
                if (tex.width > 0 and tex.height > 0) {
                    const aspect = @as(f32, @floatFromInt(tex.width)) / @as(f32, @floatFromInt(tex.height));
                    if (tw / th > aspect) tw = th * aspect else th = tw / aspect;
                }
                const r = dvui.Rect.Physical{ .x = crs.r.x + (crs.r.w - tw) * 0.5, .y = crs.r.y + (crs.r.h - th) * 0.5, .w = tw, .h = th };
                dvui.renderTexture(tex, .{ .r = r, .s = crs.s }, .{ .corners = .all(4) }) catch {};
            }
            if (widgetClicked(cell.data())) focusFace(i);
        }

        // details
        {
            var col = dvui.box(@src(), .{ .dir = .vertical }, .{ .expand = .horizontal, .id_extra = i });
            defer col.deinit();
            const f = g.faces[i];
            dvui.label(@src(), "#{d} · score {d:.2} · {d:.0}×{d:.0} px", .{
                i, f.det_score, f.bbox[2] - f.bbox[0], f.bbox[3] - f.bbox[1],
            }, .{ .id_extra = i });
            {
                var row = dvui.box(@src(), .{ .dir = .horizontal }, .{ .expand = .horizontal, .id_extra = i });
                defer row.deinit();
                var sel = g.face_sel[i];
                if (dvui.checkbox(@src(), &sel, "Swap", .{ .id_extra = i })) setFace(i, sel);
                var mouth = effectiveMouth(&g, i);
                if (dvui.checkbox(@src(), &mouth, "Mouth", .{ .id_extra = i })) setFaceMouth(&g, i, mouth);
                if (dvui.button(@src(), "Redo", .{}, .{ .id_extra = i })) spawnJob(.process_one, @intCast(i));
            }

            // Identity picker: full width so the chosen face's label is readable.
            if (g.identity_count > 0) {
                var row = dvui.box(@src(), .{ .dir = .horizontal }, .{ .expand = .horizontal, .id_extra = i });
                defer row.deinit();
                dvui.label(@src(), "Face", .{}, .{
                    .gravity_y = 0.5,
                    .id_extra = i,
                    .color_text = .{ .color = .{ .r = 0x8b, .g = 0x93, .b = 0xa0 } },
                });
                var choice: usize = if (g.face_identity[i] >= 0) @as(usize, @intCast(g.face_identity[i])) + 1 else 0;
                if (dvui.dropdown(@src(), id_labels[0 .. g.identity_count + 1], .{ .choice = &choice }, .{}, .{ .expand = .horizontal, .id_extra = i })) {
                    g.face_identity[i] = if (choice == 0) -1 else @intCast(choice - 1);
                    spawnJob(.process_one, @intCast(i));
                }
            }
        }
    }
}

fn drawStats() void {
    sectionLabel("Statistics");
    const t = g.timings;
    dvui.label(@src(), "detect    {d:8.1} ms", .{t.detect_ms}, .{});
    dvui.label(@src(), "embed     {d:8.1} ms", .{t.embed_ms}, .{});
    dvui.label(@src(), "swap      {d:8.1} ms", .{t.swap_ms}, .{});
    dvui.label(@src(), "enhance   {d:8.1} ms", .{t.enhance_ms}, .{});
    dvui.label(@src(), "total     {d:8.1} ms", .{t.total_ms}, .{});
    if (g.base) |b| {
        if (g.result) |r| {
            dvui.label(@src(), "{d}×{d} -> {d}×{d}", .{ b.width, b.height, r.width, r.height }, .{});
        } else {
            dvui.label(@src(), "input     {d}×{d}", .{ b.width, b.height }, .{});
        }
    }
    dvui.label(@src(), "{s}", .{if (g.fast_preview) "fast preview" else "full resolution"}, .{
        .color_text = .{ .color = .{ .r = 0x8b, .g = 0x93, .b = 0xa0 } },
    });
}

fn setFace(i: usize, on: bool) void {
    g.face_sel[i] = on;
    // While a job is running spawnJob coalesces this into one follow-up pass.
    if (busy() or (on and !patchFresh(&g, i))) {
        spawnJob(.process_all, -1);
    } else {
        spawnJob(.compose, -1);
    }
}

fn toggleFace(i: usize) void {
    setFace(i, !g.face_sel[i]);
}

/// Set a face's mouth-restore override. Storing `null` when the value matches
/// the global default keeps the override semantics ("follow the global").
fn setFaceMouth(s: *State, i: usize, on: bool) void {
    s.face_mouth_over[i] = if (on == s.restore_mouth) null else on;
    spawnJob(.process_one, @intCast(i));
}

// ---------------------------------------------------------------------------
// Status bar
// ---------------------------------------------------------------------------

fn drawStatusBar() void {
    const wr = dvui.windowRect();
    const col: dvui.Color = if (g.status_is_error)
        .{ .r = 0xf0, .g = 0x8a, .b = 0x8a }
    else
        .{ .r = 0xa8, .g = 0xb2, .b = 0xbe };
    dvui.labelNoFmt(@src(), g.statusSlice(), .{}, .{
        .rect = .{ .x = 0, .y = wr.h - 26, .w = wr.w, .h = 26 },
        .padding = .{ .x = 12, .y = 4, .w = 12, .h = 4 },
        .background = true,
        .style = .window,
        .color_text = .{ .color = col },
    });
}

fn sectionLabel(text: []const u8) void {
    const id_extra: usize = @truncate(std.hash.Wyhash.hash(0, text));
    dvui.labelNoFmt(@src(), text, .{}, .{
        .id_extra = id_extra,
        .font = .theme(.heading),
        .margin = .{ .x = 0, .y = 12, .w = 0, .h = 4 },
    });
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------

fn openTargetDialog() void {
    const chosen = dvui.dialogNativeFileOpen(g.allocator, .{
        .title = "Open target image",
        .filters = &.{ "*.png", "*.jpg", "*.jpeg", "*.webp", "*.bmp", "*.tif", "*.tiff" },
        .filter_description = "Images",
    }) catch null;
    if (chosen) |path| {
        defer g.allocator.free(path);
        loadTargetImage(path);
    }
}

fn openIdentityDialog() void {
    const chosen = dvui.dialogNativeFileOpenMultiple(g.allocator, .{
        .title = "Add identity image(s)",
        .filters = &.{ "*.png", "*.jpg", "*.jpeg", "*.webp", "*.bmp", "*.tif", "*.tiff" },
        .filter_description = "Images",
    }) catch null;
    if (chosen) |paths| {
        defer {
            for (paths) |p| g.allocator.free(p);
            g.allocator.free(paths);
        }
        for (paths) |path| queueIdentityFile(path);
        spawnAddIdentity();
    }
}

fn pasteTarget() void {
    if (busy()) return;
    const img = readClipboardImage() orelse {
        setStatus("No image in clipboard", true);
        return;
    };
    const w = img.width;
    const h = img.height;
    replaceBase(img);
    setStatusFmt("Pasted target · {d}×{d}", .{ w, h }, false);
}

fn pasteIdentity() void {
    const img = readClipboardImage() orelse {
        setStatus("No image in clipboard", true);
        return;
    };
    queueIdentity(img, "clipboard");
    spawnAddIdentity();
    setStatus("Pasted identity from clipboard", false);
}

fn loadTargetImage(path: []const u8) void {
    if (busy()) return;
    const pz = dupeZ(path) orelse return;
    defer g.allocator.free(pz);
    const img = ff.Image.load(pz) catch {
        setStatus("Cannot load target image", true);
        return;
    };
    const w = img.width;
    const h = img.height;
    replaceBase(img);
    defaultSavePathFor(path);
    setStatusFmt("Loaded target · {d}×{d} · {s}", .{ w, h, baseName(path) }, false);
}

/// Point the output field at `<export_dir or input_dir>/<stem>_faceforge.png`.
/// An explicit `--save` (or a previous Save as...) is left untouched.
fn defaultSavePathFor(input: []const u8) void {
    if (g.save_explicit or g.selftest != .off) return;
    const name = baseName(input);
    const dot = std.mem.lastIndexOfScalar(u8, name, '.');
    const stem = if (dot) |d| name[0..d] else name;
    const base = if (stem.len == 0) "output" else stem;
    const dir = if (pathValue(g.export_dir).len > 0) pathValue(g.export_dir) else config.dirName(input);

    const file = std.fmt.allocPrint(g.allocator, "{s}_faceforge.png", .{base}) catch return;
    defer g.allocator.free(file);
    const out = config.join(g.allocator, dir, file) catch return;
    setStr(&g.save_path, out);
    g.allocator.free(out);
}

fn loadIdentityImage(path: []const u8) void {
    const pz = dupeZ(path) orelse return;
    defer g.allocator.free(pz);
    const img = ff.Image.load(pz) catch {
        setStatus("Cannot load identity image", true);
        return;
    };
    queueIdentity(img, baseName(path));
    spawnAddIdentity();
    setStatusFmt("Adding identity · {s}", .{baseName(path)}, false);
}

/// Decode an identity image directly into the pending queue (used at startup,
/// where `busy()` bookkeeping does not apply yet).
fn queueIdentityFile(path: []const u8) void {
    const pz = dupeZ(path) orelse return;
    defer g.allocator.free(pz);
    const img = ff.Image.load(pz) catch {
        setStatus("Cannot load identity image", true);
        return;
    };
    queueIdentity(img, baseName(path));
}

fn queueIdentity(img: ff.Image, label: []const u8) void {
    var p = PendingIdentity{ .img = img };
    const n = @min(label.len, p.label.len);
    @memcpy(p.label[0..n], label[0..n]);
    p.label_len = n;
    // The worker drains this list, so guard it.
    lockPending();
    defer g.pending_mu.unlock();
    g.pending_identities.append(g.allocator, p) catch {
        var x = img;
        x.deinit();
    };
}

/// Spin on the pending-identity list lock. Critical sections only touch the
/// list header, so contention is a handful of instructions.
fn lockPending() void {
    while (!g.pending_mu.tryLock()) std.atomic.spinLoopHint();
}

fn hasPendingIdentities() bool {
    lockPending();
    defer g.pending_mu.unlock();
    return g.pending_identities.items.len > 0;
}

fn spawnAddIdentity() void {
    if (!hasPendingIdentities()) return;
    spawnJob(.add_identity, -1);
}

/// Install a new pristine base image and start a fresh detection pass.
fn replaceBase(img: ff.Image) void {
    if (g.base) |*old| old.deinit();
    g.base = img;
    g.base_dirty = true;
    g.base_gen +%= 1;
    rebuildPma();
    clearResult();
    clearStudio();
    freeProc();
    clearPatches();
    g.face_count = 0;
    g.undo_len = 0;
    g.redo_len = 0;
    if (!g.booting and !busy()) spawnJob(.reload_target, -1);
}

fn clearResult() void {
    if (g.result) |*im| im.deinit();
    g.result = null;
    freePmaArr(&g.result_pma);
    freeDisplay();
    g.view_gen +%= 1;
    // Nothing to blend while there is no result; mark as built so the next
    // frame does not rebuild for no reason.
    g.display_gen = g.view_gen;
    g.result_dirty = true;
    g.result_sig = 0;
    g.result_level = -1;
    g.timings = .{};
}

fn saveResultDialog() void {
    if (g.result == null) {
        setStatus("Nothing to save yet", true);
        return;
    }
    const chosen = dvui.dialogNativeFileSave(g.allocator, .{
        .title = "Save result image",
        .path = pathValue(g.save_path),
        .filters = &.{ "*.png", "*.jpg", "*.jpeg", "*.webp", "*.bmp" },
        .filter_description = "Images",
    }) catch null;
    if (chosen) |path| {
        defer g.allocator.free(path);
        if (g.save_path.len > 0) g.allocator.free(g.save_path);
        g.save_path = g.allocator.dupe(u8, path) catch g.allocator.dupe(u8, "out.png") catch return;
        g.save_explicit = true;
        saveResultToPath(path);
    }
}

fn saveResultToPath(path: []const u8) void {
    if (beginFullResExport(.save, path)) return;
    saveNow(path);
}

fn saveNow(path: []const u8) void {
    const img = displayImage() orelse {
        setStatus("Nothing to save yet", true);
        return;
    };
    const pz = dupeZ(path) orelse return;
    defer g.allocator.free(pz);
    img.save(pz) catch {
        setStatus("Cannot save result", true);
        return;
    };
    setStatusFmt("Saved · {s}", .{baseName(path)}, false);
}

fn copyResult() void {
    if (beginFullResExport(.copy, null)) return;
    copyNow();
}

fn copyNow() void {
    const img = displayImage() orelse {
        setStatus("Nothing to copy yet", true);
        return;
    };
    const png = img.encodePng(g.allocator) catch {
        setStatus("PNG encode failed", true);
        return;
    };
    defer g.allocator.free(png);
    setClipboardPng(png);
}

/// A save/copy while the working image is downscaled must not export the
/// preview. Queue the export, rerun the pipeline at full resolution, and let
/// `finishExport` complete it once the job is done.
fn beginFullResExport(kind: ExportKind, path: ?[]const u8) bool {
    if (busy()) {
        setStatus("Busy — try again in a moment", true);
        return true;
    }
    if (!needsFullRes()) return false;
    if (g.export_path) |p| g.allocator.free(p);
    g.export_path = null;
    if (path) |p| g.export_path = g.allocator.dupe(u8, p) catch null;
    g.pending_export = kind;
    setStatus("Rendering full resolution for export…", false);
    spawnJob(.process_all, -1);
    return true;
}

fn needsFullRes() bool {
    if (!g.fast_preview) return false;
    const base = g.base orelse return false;
    if (g.proc_w <= 0 or g.proc_h <= 0) return true;
    return @max(base.width, base.height) > @max(g.proc_w, g.proc_h);
}

fn finishExport() void {
    const kind = g.pending_export;
    if (kind == .none) return;
    g.pending_export = .none;
    switch (kind) {
        .save => {
            if (g.export_path) |p| {
                saveNow(p);
                g.allocator.free(p);
                g.export_path = null;
            }
        },
        .copy => copyNow(),
        .apply_input => applyResultAsInput(),
        .none => {},
    }
}

fn applyResultAsInput() void {
    if (busy()) return;
    if (needsFullRes()) {
        if (g.export_path) |p| {
            g.allocator.free(p);
            g.export_path = null;
        }
        g.pending_export = .apply_input;
        setStatus("Rendering full resolution before applying…", false);
        spawnJob(.process_all, -1);
        return;
    }
    const res = displayImage() orelse {
        setStatus("Process an image first", true);
        return;
    };
    // Snapshot the current base for undo.
    if (g.base) |b| {
        const snap = b.clone() catch {
            setStatus("Out of memory", true);
            return;
        };
        pushUndo(snap);
    }
    clearRedo();
    const next = res.clone() catch {
        setStatus("Out of memory", true);
        return;
    };
    if (g.base) |*old| old.deinit();
    g.base = next;
    g.base_dirty = true;
    g.base_gen +%= 1;
    rebuildPma();
    clearResult();
    clearStudio();
    freeProc();
    clearPatches();
    g.face_count = 0;
    if (!busy()) spawnJob(.reload_target, -1);
    setStatus("Result applied as input", false);
}

fn pushUndo(img: ff.Image) void {
    if (g.undo_len == history_cap) {
        if (g.undo[0]) |*old| old.deinit();
        std.mem.copyForwards(?ff.Image, g.undo[0 .. history_cap - 1], g.undo[1..history_cap]);
        g.undo_len -= 1;
    }
    g.undo[g.undo_len] = img;
    g.undo_len += 1;
}

fn clearRedo() void {
    for (&g.redo) |*u| {
        if (u.*) |*im| im.deinit();
        u.* = null;
    }
    g.redo_len = 0;
}

fn undoHistory() void {
    if (busy() or g.undo_len == 0) {
        if (g.undo_len == 0) setStatus("Nothing to undo", true);
        return;
    }
    g.undo_len -= 1;
    const snap = g.undo[g.undo_len].?;
    g.undo[g.undo_len] = null;
    if (g.base) |b| {
        if (g.redo_len == history_cap) {
            if (g.redo[0]) |*old| old.deinit();
            std.mem.copyForwards(?ff.Image, g.redo[0 .. history_cap - 1], g.redo[1..history_cap]);
            g.redo_len -= 1;
        }
        const cur = b.clone() catch null;
        if (cur) |c| {
            g.redo[g.redo_len] = c;
            g.redo_len += 1;
        }
    }
    if (g.base) |*old| old.deinit();
    g.base = snap;
    g.base_dirty = true;
    g.base_gen +%= 1;
    rebuildPma();
    clearResult();
    clearStudio();
    freeProc();
    clearPatches();
    g.face_count = 0;
    spawnJob(.reload_target, -1);
    setStatus("Undo", false);
}

fn redoHistory() void {
    if (busy() or g.redo_len == 0) {
        if (g.redo_len == 0) setStatus("Nothing to redo", true);
        return;
    }
    g.redo_len -= 1;
    const snap = g.redo[g.redo_len].?;
    g.redo[g.redo_len] = null;
    if (g.base) |b| {
        const cur = b.clone() catch null;
        if (cur) |c| {
            if (g.undo_len == history_cap) {
                if (g.undo[0]) |*old| old.deinit();
                std.mem.copyForwards(?ff.Image, g.undo[0 .. history_cap - 1], g.undo[1..history_cap]);
                g.undo_len -= 1;
            }
            g.undo[g.undo_len] = c;
            g.undo_len += 1;
        }
    }
    if (g.base) |*old| old.deinit();
    g.base = snap;
    g.base_dirty = true;
    g.base_gen +%= 1;
    rebuildPma();
    clearResult();
    clearStudio();
    freeProc();
    clearPatches();
    g.face_count = 0;
    spawnJob(.reload_target, -1);
    setStatus("Redo", false);
}

fn requestReload() void {
    if (busy() or g.base == null) return;
    spawnJob(.reload_target, -1);
}

// ---------------------------------------------------------------------------
// Keyboard shortcuts
// ---------------------------------------------------------------------------

fn handleShortcuts() void {
    const evts = dvui.events();
    for (evts) |*e| {
        if (e.handled) continue;
        switch (e.evt) {
            .key => |ke| {
                if (ke.action != .down) continue;
                // Global panels work in both workspaces.
                if (ke.code == .f1) {
                    g.show_about = !g.show_about;
                    if (g.show_about) g.show_settings = false;
                    e.handled = true;
                    continue;
                }
                if (ke.mod.control() and ke.code == .comma) {
                    g.show_settings = !g.show_settings;
                    if (g.show_settings) g.show_about = false;
                    e.handled = true;
                    continue;
                }
                if (ke.code == .escape and (g.show_settings or g.show_about)) {
                    g.show_settings = false;
                    g.show_about = false;
                    e.handled = true;
                    continue;
                }
                if (!ke.mod.control() or g.mode == .video) continue;
                switch (ke.code) {
                    .z => {
                        if (g.studio) {
                            if (ke.mod.shift()) maskRedo() else maskUndo();
                        } else if (ke.mod.shift()) redoHistory() else undoHistory();
                        e.handled = true;
                    },
                    .y => {
                        if (g.studio) maskRedo() else redoHistory();
                        e.handled = true;
                    },
                    .o => {
                        openTargetDialog();
                        e.handled = true;
                    },
                    .s => {
                        if (pathValue(g.save_path).len > 0) saveResultToPath(pathValue(g.save_path)) else saveResultDialog();
                        e.handled = true;
                    },
                    else => {},
                }
            },
            else => {},
        }
    }
}

// ---------------------------------------------------------------------------
// Image / texture plumbing
// ---------------------------------------------------------------------------

fn deinitImage(slot: *?ff.Image) void {
    if (slot.*) |*im| im.deinit();
    slot.* = null;
}

fn freePmaArr(slot: *?[]dvui.Color.PMA) void {
    if (slot.*) |p| g.allocator.free(p);
    slot.* = null;
}

fn freeThumb(t: *Thumb) void {
    if (t.img) |*im| im.deinit();
    t.img = null;
    t.dirty = false;
    // texture is owned by dvui and destroyed via textureDestroyLater
    if (t.tex) |tex| {
        dvui.textureDestroyLater(tex);
        t.tex = null;
    }
}

fn freeProc() void {
    g.proc.deinit();
    g.proc_w = 0;
    g.proc_h = 0;
}

fn clearPatches() void {
    for (&g.patches) |*set| {
        for (set) |*p| {
            if (p.valid) p.img.deinit();
            p.valid = false;
        }
    }
}

fn rebuildPma() void {
    freePmaArr(&g.base_pma);
    if (g.base) |*img| {
        g.base_pma = toPma(g.allocator, img) catch null;
    }
}

fn rebuildResultPma() void {
    freePmaArr(&g.result_pma);
    const img: ?*const ff.Image = blk: {
        if (g.mask_used) {
            if (g.display) |*d| break :blk d;
        }
        if (g.result) |*r| break :blk r;
        break :blk null;
    };
    if (img) |im| g.result_pma = toPma(g.allocator, im) catch null;
    g.result_dirty = true;
}

fn toPma(allocator: std.mem.Allocator, img: *const ff.Image) ![]dvui.Color.PMA {
    const w: usize = @intCast(img.width);
    const h: usize = @intCast(img.height);
    const pma = try allocator.alloc(dvui.Color.PMA, w * h);
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

fn syncTextures() void {
    // Retouch edits bump `view_gen`; rebuild the blended composite once per
    // frame rather than on every mouse-motion event.
    if (g.display_gen != g.view_gen) updateDisplay();
    if (g.base_dirty) {
        g.base_dirty = false;
        if (g.base_pma) |pma| {
            const im = g.base.?;
            syncTexture(&g.base_tex, pma, @intCast(im.width), @intCast(im.height));
        }
    }
    if (g.result_dirty) {
        g.result_dirty = false;
        if (g.result) |*im| {
            if (g.result_pma) |pma| {
                syncTexture(&g.result_tex, pma, @intCast(im.width), @intCast(im.height));
            }
        } else if (g.result_tex) |tex| {
            dvui.textureDestroyLater(tex);
            g.result_tex = null;
        }
    }
    if (g.studio and g.studio_show_mask and g.mask_used and g.mask_tex_gen != g.mask_gen) buildMaskTexture();
    for (&g.face_thumb) |*t| uploadThumb(t);
    for (&g.identities) |*id| uploadThumb(&id.thumb);
}

fn uploadThumb(t: *Thumb) void {
    if (!t.dirty) return;
    t.dirty = false;
    const img = &(t.img orelse return);
    const pma = toPma(g.allocator, img) catch return;
    defer g.allocator.free(pma);
    syncTexture(&t.tex, pma, @intCast(img.width), @intCast(img.height));
}

fn syncTexture(slot: *?dvui.Texture, pma: []const dvui.Color.PMA, w: u32, h: u32) void {
    if (slot.*) |*tex| {
        if (tex.width == w and tex.height == h) {
            tex.update(pma) catch {};
            return;
        }
        dvui.textureDestroyLater(tex.*);
        slot.* = null;
    }
    slot.* = dvui.textureCreate(pma, .{ .width = w, .height = h, .interpolation = .linear }) catch null;
}

// ---------------------------------------------------------------------------
// Studio — retouch layer
//
// A base-resolution alpha mask paints the pristine original back over the
// composed result (erase added hair, blemishes, background bleed...). Painting
// never touches the worker output: `g.result` stays the raw composite and
// `g.display` is a blended copy rebuilt on demand. Saving, copying and
// "apply as input" all use `displayImage()`, so retouching survives quality
// changes and is baked in only when the user applies the result.
// ---------------------------------------------------------------------------

fn freeMaskTexture() void {
    if (g.mask_tex) |tex| dvui.textureDestroyLater(tex);
    g.mask_tex = null;
    g.mask_tex_gen = 0xffff_ffff;
}

fn freeDisplay() void {
    deinitImage(&g.display);
}

fn freeMaskEdit(e: *?MaskEdit) void {
    if (e.*) |*ed| g.allocator.free(ed.data);
    e.* = null;
}

fn clearMaskUndoRedo() void {
    for (&g.mask_undo) |*e| freeMaskEdit(e);
    for (&g.mask_redo) |*e| freeMaskEdit(e);
    g.mask_undo_len = 0;
    g.mask_redo_len = 0;
}

/// Allocate (or reallocate) the mask so it matches the base image. Returns
/// false when there is no base image or memory is exhausted.
fn maskEnsure() bool {
    const base = &(g.base orelse return false);
    if (base.width <= 0 or base.height <= 0) return false;
    if (g.mask.len > 0 and g.mask_w == base.width and g.mask_h == base.height) return true;

    if (g.mask.len > 0) g.allocator.free(g.mask);
    g.mask = &.{};
    g.mask_w = 0;
    g.mask_h = 0;
    clearMaskUndoRedo();

    const n = @as(usize, @intCast(base.width)) * @as(usize, @intCast(base.height));
    g.mask = g.allocator.alloc(u8, n) catch return false;
    @memset(g.mask, 0);
    g.mask_w = base.width;
    g.mask_h = base.height;
    g.mask_used = false;
    g.mask_gen +%= 1;
    g.view_gen +%= 1;
    return true;
}

/// Drop every retouch artefact. Called whenever the base image is replaced so
/// stale masks can never leak into a different picture.
fn clearStudio() void {
    if (g.stroke_before) |b| {
        g.allocator.free(b);
        g.stroke_before = null;
    }
    g.stroke_active = false;
    if (g.mask.len > 0) g.allocator.free(g.mask);
    g.mask = &.{};
    g.mask_w = 0;
    g.mask_h = 0;
    g.mask_used = false;
    g.mask_gen +%= 1;
    clearMaskUndoRedo();
    freeDisplay();
    freeMaskTexture();
    freeOrigCache();
    g.display_gen = g.view_gen;
    g.view_gen +%= 1;
}

fn freeOrigCache() void {
    deinitImage(&g.orig_cache);
    g.orig_cache_gen = 0xffff_ffff_ffff_ffff;
}

/// The pristine image used as the bottom retouch layer, at the result's
/// resolution. The working copy already has the right size; otherwise the base
/// is resized once and cached until it changes.
fn originalFor(w: i32, h: i32) ?*ff.Image {
    if (w <= 0 or h <= 0) return null;
    if (g.proc_w == w and g.proc_h == h and g.proc.img.width == w and g.proc.img.height == h) {
        return &g.proc.img;
    }
    if (g.orig_cache) |*im| {
        if (im.width == w and im.height == h and g.orig_cache_gen == g.base_gen) return im;
    }
    const base = &(g.base orelse return null);
    freeOrigCache();
    g.orig_cache = base.resized(w, h) catch return null;
    g.orig_cache_gen = g.base_gen;
    return &g.orig_cache.?;
}

/// Rebuild `g.display` (result + mask blend) if the visible generation changed.
/// Cheap to call from the UI thread: it is a no-op unless something changed.
fn updateDisplay() void {
    if (g.display_gen == g.view_gen) return;
    g.display_gen = g.view_gen;
    freeDisplay();

    if (g.mask_used and g.mask.len > 0) {
        if (g.result) |*res| {
            if (originalFor(res.width, res.height)) |orig| {
                if (res.clone()) |cloned| {
                    var disp = cloned;
                    ff.blendMask(&disp, orig, g.mask, g.mask_w, g.mask_h) catch {};
                    g.display = disp;
                } else |_| {}
            }
        }
    }
    rebuildResultPma();
}

/// The image the user sees and exports: the retouched composite when a mask is
/// active, otherwise the raw result.
fn displayImage() ?*ff.Image {
    if (g.display_gen != g.view_gen) updateDisplay();
    if (g.display) |*d| return d;
    if (g.result) |*r| return r;
    return null;
}

/// Red mask preview at a reduced resolution (purely visual, so it does not
/// need to match the base pixel for pixel).
fn buildMaskTexture() void {
    g.mask_tex_gen = g.mask_gen;
    const mw = g.mask_w;
    const mh = g.mask_h;
    if (g.mask.len == 0 or mw <= 0 or mh <= 0) return;

    const max_dim: f32 = 900.0;
    const longest: f32 = @floatFromInt(@max(mw, mh));
    const f: f32 = if (longest > max_dim) max_dim / longest else 1.0;
    const ow: usize = @max(1, @as(usize, @intFromFloat(@as(f32, @floatFromInt(mw)) * f)));
    const oh: usize = @max(1, @as(usize, @intFromFloat(@as(f32, @floatFromInt(mh)) * f)));

    const pma = g.allocator.alloc(dvui.Color.PMA, ow * oh) catch return;
    defer g.allocator.free(pma);

    const mwu: usize = @intCast(mw);
    const fx = @as(f32, @floatFromInt(mw)) / @as(f32, @floatFromInt(ow));
    const fy = @as(f32, @floatFromInt(mh)) / @as(f32, @floatFromInt(oh));
    const mxf = @as(f32, @floatFromInt(mw)) - 0.5;
    const myf = @as(f32, @floatFromInt(mh)) - 0.5;

    for (0..oh) |y| {
        const my: usize = @intFromFloat(@min(myf, (@as(f32, @floatFromInt(y)) + 0.5) * fy));
        const row = g.mask[my * mwu ..];
        for (0..ow) |x| {
            const mx: usize = @intFromFloat(@min(mxf, (@as(f32, @floatFromInt(x)) + 0.5) * fx));
            const a: f32 = @as(f32, @floatFromInt(row[mx])) * (0.42 / 255.0);
            pma[y * ow + x] = .{
                .r = @intFromFloat(255.0 * a),
                .g = @intFromFloat(70.0 * a),
                .b = @intFromFloat(58.0 * a),
                .a = @intFromFloat(255.0 * a),
            };
        }
    }
    syncTexture(&g.mask_tex, pma, @intCast(ow), @intCast(oh));
}

fn maskReset() void {
    if (g.mask.len > 0) @memset(g.mask, 0);
    g.mask_used = false;
    g.mask_gen +%= 1;
    clearMaskUndoRedo();
    g.view_gen +%= 1;
    setStatus("Retouch cleared", false);
}

/// One brush dab. `erase` reveals the original (mask -> 255); the restore tool
/// paints the result back (mask -> 0).
fn maskStampAt(cx: f32, cy: f32, erase: bool) void {
    const mw = g.mask_w;
    const mh = g.mask_h;
    if (mw <= 0 or mh <= 0 or g.mask.len == 0) return;

    const radius = @max(0.6, g.studio_size * 0.5);
    const hard = std.math.clamp(g.studio_hard, 0.0, 0.98);
    const opac = std.math.clamp(g.studio_opac, 0.01, 1.0);
    const inner = radius * hard;
    const falloff = 1.0 / @max(0.0001, radius - inner);

    const x0 = @max(0, @as(i32, @intFromFloat(@floor(cx - radius))));
    const y0 = @max(0, @as(i32, @intFromFloat(@floor(cy - radius))));
    const x1 = @min(mw - 1, @as(i32, @intFromFloat(@floor(cx + radius))));
    const y1 = @min(mh - 1, @as(i32, @intFromFloat(@floor(cy + radius))));
    if (x1 < x0 or y1 < y0) return;

    const mwu: usize = @intCast(mw);
    var yy = y0;
    while (yy <= y1) : (yy += 1) {
        const row = g.mask[@as(usize, @intCast(yy)) * mwu ..];
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
            const nv: f32 = if (erase)
                cur + (255.0 - cur) * a
            else
                cur * (1.0 - a);
            row[@intCast(xx)] = @intFromFloat(std.math.clamp(nv, 0.0, 255.0));
        }
    }

    g.stroke_min_x = @min(g.stroke_min_x, x0);
    g.stroke_min_y = @min(g.stroke_min_y, y0);
    g.stroke_max_x = @max(g.stroke_max_x, x1);
    g.stroke_max_y = @max(g.stroke_max_y, y1);
}

fn strokeBegin(px: f32, py: f32) void {
    if (!maskEnsure()) return;
    if (g.mask.len == 0) return;
    if (g.stroke_before) |b| g.allocator.free(b);
    g.stroke_before = g.allocator.dupe(u8, g.mask) catch null;

    g.stroke_active = true;
    g.stroke_min_x = g.mask_w;
    g.stroke_min_y = g.mask_h;
    g.stroke_max_x = -1;
    g.stroke_max_y = -1;
    g.stroke_last_x = px;
    g.stroke_last_y = py;

    maskStampAt(px, py, g.studio_erase);
    g.mask_used = true;
    g.mask_gen +%= 1;
    g.view_gen +%= 1;
}

fn strokeTo(px: f32, py: f32) void {
    if (!g.stroke_active) return;
    const spacing = @max(1.0, g.studio_size * 0.18);
    const dx = px - g.stroke_last_x;
    const dy = py - g.stroke_last_y;
    const dist = @sqrt(dx * dx + dy * dy);
    const steps: u32 = @intFromFloat(@max(1.0, @ceil(dist / spacing)));
    var k: u32 = 1;
    while (k <= steps) : (k += 1) {
        const t = @as(f32, @floatFromInt(k)) / @as(f32, @floatFromInt(steps));
        maskStampAt(g.stroke_last_x + dx * t, g.stroke_last_y + dy * t, g.studio_erase);
    }
    g.stroke_last_x = px;
    g.stroke_last_y = py;
    g.mask_gen +%= 1;
    g.view_gen +%= 1;
}

fn strokeEnd() void {
    if (!g.stroke_active) return;
    g.stroke_active = false;
    defer {
        if (g.stroke_before) |b| {
            g.allocator.free(b);
            g.stroke_before = null;
        }
    }
    if (g.stroke_max_x < g.stroke_min_x or g.stroke_max_y < g.stroke_min_y) return;
    const before = g.stroke_before orelse return;

    const x = g.stroke_min_x;
    const y = g.stroke_min_y;
    const w = g.stroke_max_x - x + 1;
    const h = g.stroke_max_y - y + 1;
    const data = copyMaskRegion(before, x, y, w, h) catch return;
    pushMaskUndo(.{ .x = x, .y = y, .w = w, .h = h, .data = data });
    setStatusFmt("Retouched · {d} stroke(s)", .{g.mask_undo_len}, false);
}

fn copyMaskRegion(src: []const u8, x: i32, y: i32, w: i32, h: i32) ![]u8 {
    const mwu: usize = @intCast(g.mask_w);
    const wu: usize = @intCast(w);
    const out = try g.allocator.alloc(u8, wu * @as(usize, @intCast(h)));
    var row: usize = 0;
    while (row < @as(usize, @intCast(h))) : (row += 1) {
        const sy = @as(usize, @intCast(y)) + row;
        const off = sy * mwu + @as(usize, @intCast(x));
        @memcpy(out[row * wu ..][0..wu], src[off..][0..wu]);
    }
    return out;
}

fn writeMaskRegion(edit: *const MaskEdit) void {
    const mwu: usize = @intCast(g.mask_w);
    const wu: usize = @intCast(edit.w);
    var row: usize = 0;
    while (row < @as(usize, @intCast(edit.h))) : (row += 1) {
        const dy = @as(usize, @intCast(edit.y)) + row;
        const off = dy * mwu + @as(usize, @intCast(edit.x));
        @memcpy(g.mask[off..][0..wu], edit.data[row * wu ..][0..wu]);
    }
}

fn pushMaskUndo(edit: MaskEdit) void {
    if (g.mask_undo_len == g.mask_undo.len) {
        freeMaskEdit(&g.mask_undo[0]);
        std.mem.copyForwards(?MaskEdit, g.mask_undo[0 .. g.mask_undo.len - 1], g.mask_undo[1..]);
        g.mask_undo_len -= 1;
    }
    g.mask_undo[g.mask_undo_len] = edit;
    g.mask_undo_len += 1;
    clearMaskRedo();
}

fn pushMaskRedo(edit: MaskEdit) void {
    if (g.mask_redo_len == g.mask_redo.len) {
        freeMaskEdit(&g.mask_redo[0]);
        std.mem.copyForwards(?MaskEdit, g.mask_redo[0 .. g.mask_redo.len - 1], g.mask_redo[1..]);
        g.mask_redo_len -= 1;
    }
    g.mask_redo[g.mask_redo_len] = edit;
    g.mask_redo_len += 1;
}

fn clearMaskRedo() void {
    for (&g.mask_redo) |*e| freeMaskEdit(e);
    g.mask_redo_len = 0;
}

fn maskUndo() void {
    if (g.mask_undo_len == 0) {
        setStatus("Nothing to undo", true);
        return;
    }
    const edit = g.mask_undo[g.mask_undo_len - 1].?;
    g.mask_undo[g.mask_undo_len - 1] = null;
    g.mask_undo_len -= 1;

    const after = copyMaskRegion(g.mask, edit.x, edit.y, edit.w, edit.h) catch null;
    if (after) |data| pushMaskRedo(.{ .x = edit.x, .y = edit.y, .w = edit.w, .h = edit.h, .data = data });
    writeMaskRegion(&edit);
    g.allocator.free(edit.data);

    g.mask_used = true;
    g.mask_gen +%= 1;
    g.view_gen +%= 1;
    setStatus("Retouch undo", false);
}

fn maskRedo() void {
    if (g.mask_redo_len == 0) {
        setStatus("Nothing to redo", true);
        return;
    }
    const edit = g.mask_redo[g.mask_redo_len - 1].?;
    g.mask_redo[g.mask_redo_len - 1] = null;
    g.mask_redo_len -= 1;

    const before = copyMaskRegion(g.mask, edit.x, edit.y, edit.w, edit.h) catch null;
    if (before) |data| {
        if (g.mask_undo_len == g.mask_undo.len) {
            freeMaskEdit(&g.mask_undo[0]);
            std.mem.copyForwards(?MaskEdit, g.mask_undo[0 .. g.mask_undo.len - 1], g.mask_undo[1..]);
            g.mask_undo_len -= 1;
        }
        g.mask_undo[g.mask_undo_len] = .{ .x = edit.x, .y = edit.y, .w = edit.w, .h = edit.h, .data = data };
        g.mask_undo_len += 1;
    }
    writeMaskRegion(&edit);
    g.allocator.free(edit.data);

    g.mask_used = true;
    g.mask_gen +%= 1;
    g.view_gen +%= 1;
    setStatus("Retouch redo", false);
}

/// Build thumbnail image buffers for detected target faces (cropped from the
/// pristine base so the preview resolution never changes them).
fn buildFaceThumbs() void {
    const base = &(g.base orelse return);
    for (0..max_faces) |i| {
        const t = &g.face_thumb[i];
        if (i >= g.face_count) {
            if (t.img) |*im| im.deinit();
            t.img = null;
            continue;
        }
        if (t.img) |*im| im.deinit();
        t.img = makeThumb(base, g.faces[i].bbox) catch null;
        t.dirty = true;
    }
}

fn makeThumb(img: *const ff.Image, bbox: [4]f32) !ff.Image {
    const w = @max(1.0, bbox[2] - bbox[0]);
    const h = @max(1.0, bbox[3] - bbox[1]);
    const cx = (bbox[0] + bbox[2]) * 0.5;
    const cy = (bbox[1] + bbox[3]) * 0.5;
    const side = @max(w, h) * 1.25;
    const x0f = std.math.clamp(cx - side * 0.5, 0, @as(f32, @floatFromInt(img.width)) - 1);
    const y0f = std.math.clamp(cy - side * 0.5, 0, @as(f32, @floatFromInt(img.height)) - 1);
    const x1f = std.math.clamp(x0f + side, 1, @as(f32, @floatFromInt(img.width)));
    const y1f = std.math.clamp(y0f + side, 1, @as(f32, @floatFromInt(img.height)));
    const rw: i32 = @intFromFloat(@max(1.0, x1f - x0f));
    const rh: i32 = @intFromFloat(@max(1.0, y1f - y0f));
    const rx: i32 = @intFromFloat(x0f);
    const ry: i32 = @intFromFloat(y0f);
    const size: i32 = 72;
    return img.resizeRegion(rx, ry, rw, rh, size, size);
}

// ---------------------------------------------------------------------------
// Worker
// ---------------------------------------------------------------------------

fn spawnJob(kind: JobKind, arg: i32) void {
    if (video.active()) {
        setStatus("A video job is running — finish or cancel it first", true);
        return;
    }
    if (busy()) {
        // Remember the edit so it is applied as soon as the worker is free.
        // One process_all pass re-validates and recomposes every face, so it
        // covers any number of per-face identity/mouth changes.
        switch (kind) {
            .none => {},
            .init, .reload_target, .add_identity => {
                if (g.queued_job == .none) {
                    g.queued_job = kind;
                    g.queued_arg = arg;
                }
            },
            .process_all, .process_one, .compose => {
                switch (g.queued_job) {
                    // A higher-priority job (detect/identity) is already queued
                    // and reprocesses everything too.
                    .init, .reload_target, .add_identity => {},
                    else => {
                        g.queued_job = .process_all;
                        g.queued_arg = -1;
                    },
                }
            },
        }
        return;
    }
    if (g.thread) |t| {
        t.join();
        g.thread = null;
    }

    const dir = pathValue(g.model_dir);
    const n = @min(dir.len, g.job_model_dir.len - 1);
    @memcpy(g.job_model_dir[0..n], dir[0..n]);
    g.job_model_dir[n] = 0;

    g.job_arg = arg;
    g.job_provider = switch (g.provider) {
        .cuda => 1,
        .cpu => 2,
        .auto => 0,
    };
    g.job_enhancer = @intCast(@backingInt(g.enhancer));
    g.job_restore_mouth = g.restore_mouth;
    g.job_fast_preview = g.fast_preview and g.pending_export == .none;
    g.job_unload = g.unload_after_run;

    g.job_error_len = 0;
    g.job_done.store(false, .release);
    g.job_ok.store(false, .release);
    g.job_kind.store(@backingInt(kind), .monotonic);
    g.progress_value.store(0, .monotonic);
    g.stage_value.store(@backingInt(Stage.loading), .monotonic);
    g.working.store(true, .release);

    g.thread = std.Thread.spawn(.{}, workerMain, .{ &g, @backingInt(kind) }) catch |err| {
        g.working.store(false, .release);
        setStatusFmt("Cannot start worker: {s}", .{@errorName(err)}, true);
        return;
    };
    setStatus("Working…", false);
}

fn pollJob() void {
    syncTextures();
    if (!g.working.load(.acquire)) return;
    if (!g.job_done.load(.acquire)) return;

    const ok = g.job_ok.load(.acquire);
    const kind: JobKind = @fromBackingInt(@intCast(g.job_kind.load(.monotonic)));

    if (ok) {
        takePendingResult();
        buildFaceThumbs();
        syncTextures();
        g.stage_value.store(@backingInt(Stage.done), .monotonic);
        g.progress_value.store(1000, .monotonic);
        setStatus("Ready", false);
    } else {
        const msg = g.job_error[0..g.job_error_len];
        std.debug.print("[faceforge] job failed: {s}\n", .{msg});
        if (msg.len > 0) setStatus(msg, true) else setStatus("Processing failed", true);
        g.pending_export = .none;
        if (g.export_path) |p| {
            g.allocator.free(p);
            g.export_path = null;
        }
    }

    if (ok and g.job_unload) {
        if (g.engine) |e| e.unload();
    }

    g.working.store(false, .release);
    if (g.thread) |t| {
        t.join();
        g.thread = null;
    }

    if (ok) finishExport();
    if (g.selftest != .off) selftestAdvance(kind, ok);

    // Identities dropped while a job was running stay queued until the worker
    // is free again; pick them up now instead of waiting for another action.
    if (hasPendingIdentities() and g.selftest == .off) {
        // Adding identities re-swaps everything, so it also covers any
        // per-face edit made while the job was running.
        g.queued_job = .none;
        spawnAddIdentity();
    } else if (g.queued_job != .none and g.selftest == .off) {
        const qk = g.queued_job;
        const qa = g.queued_arg;
        g.queued_job = .none;
        g.queued_arg = -1;
        spawnJob(qk, qa);
    }

    if (g.auto_run and ok) {
        switch (kind) {
            .init, .reload_target, .add_identity, .process_all => {
                if (!g.auto_saved) {
                    g.auto_saved = true;
                    g.auto_run = false;
                    saveResultToPath(pathValue(g.save_path));
                }
            },
            else => {},
        }
    }
}

/// Drives the FACEFORGE_SELFTEST sequence. Every step exercises one of the
/// interactive hot-update paths (skip face, reprocess one face, recompose,
/// apply-as-input, undo) and saves the final image.
fn selftestAdvance(kind: JobKind, ok: bool) void {
    if (!ok) {
        std.debug.print("[selftest] FAILED during {s}: {s}\n", .{
            @tagName(kind),
            g.job_error[0..g.job_error_len],
        });
        g.selftest = .off;
        std.process.exit(1);
    }
    const elapsed = nsToMs(nowNs() - g.selftest_t0);
    switch (g.selftest) {
        .init => {
            std.debug.print("[selftest] init ok: faces={d} identities={d} proc={d}x{d} result={?d}x{?d} provider={s}\n", .{
                g.face_count,
                g.identity_count,
                g.proc_w,
                g.proc_h,
                if (g.result) |r| r.width else null,
                if (g.result) |r| r.height else null,
                providerName(),
            });
            if (g.identity_count >= 2 and g.face_count >= 2) {
                // Hot per-face overrides: face 1 takes identity 2 and the
                // inverse of the global mouth setting.
                g.face_identity[1] = 1;
                g.face_mouth_over[1] = !g.restore_mouth;
                g.selftest = .per_face;
                g.selftest_t0 = nowNs();
                spawnJob(.process_one, 1);
            } else {
                std.debug.print("[selftest] per-face identity skipped (needs 2 identities, see FACEFORGE_IDENTITY)\n", .{});
                selftestSkipStart();
            }
        },
        .per_face => {
            const p = &g.patches[g.proc_level][1];
            std.debug.print("[selftest] per-face identity/mouth ok in {d:.1} ms: face1 identity={d} patch(identity={d}, mouth={}, proc={d}x{d})\n", .{
                elapsed,
                g.face_identity[1],
                p.identity,
                p.mouth,
                p.proc_w,
                p.proc_h,
            });
            selftestCheck(p.valid and p.identity == 1 and p.mouth == !g.restore_mouth, "per-face patch metadata mismatch", .{});
            selftestSkipStart();
        },
        .compose_skip => {
            std.debug.print("[selftest] compose (skipping face 0) ok in {d:.1} ms\n", .{elapsed});
            if (g.face_count > 0) g.face_sel[0] = true;
            g.selftest = .process_one;
            g.selftest_t0 = nowNs();
            spawnJob(.process_one, 0);
        },
        .process_one => {
            std.debug.print("[selftest] process_one(0) ok in {d:.1} ms\n", .{elapsed});
            g.selftest = .compose_all;
            g.selftest_t0 = nowNs();
            spawnJob(.compose, -1);
        },
        .compose_all => {
            std.debug.print("[selftest] compose (all faces) ok in {d:.1} ms\n", .{elapsed});
            // Warm the downscaled patch level, then come back to full: the
            // full-resolution patches from init must still be there.
            g.fast_preview = true;
            g.selftest = .level_preview;
            g.selftest_t0 = nowNs();
            spawnJob(.process_all, -1);
        },
        .level_preview => {
            std.debug.print("[selftest] preview level ok in {d:.1} ms (swap {d:.1} ms, {d}x{d})\n", .{
                elapsed, g.timings.swap_ms, g.proc_w, g.proc_h,
            });
            g.fast_preview = false;
            g.selftest = .level_full;
            g.selftest_t0 = nowNs();
            spawnJob(.process_all, -1);
        },
        .level_full => {
            std.debug.print("[selftest] back to full level ok in {d:.1} ms (swap {d:.1} ms — cached)\n", .{
                elapsed, g.timings.swap_ms,
            });
            selftestCheck(g.timings.swap_ms == 0, "full-resolution patches were not reused", .{});
            g.fast_preview = true;
            g.selftest = .level_cached;
            g.selftest_t0 = nowNs();
            spawnJob(.process_all, -1);
        },
        .level_cached => {
            std.debug.print("[selftest] preview level again ok in {d:.1} ms (swap {d:.1} ms — cached)\n", .{
                elapsed, g.timings.swap_ms,
            });
            selftestCheck(g.timings.swap_ms == 0, "preview patches were not reused", .{});
            g.selftest = .apply;
            g.selftest_t0 = nowNs();
            applyResultAsInput();
        },
        .apply => {
            // When the result was a preview, apply-as-input first renders at
            // full resolution and re-detects asynchronously; wait for that
            // reload so the printed sizes describe the new base.
            if (g.face_count == 0) return;
            std.debug.print("[selftest] apply-as-input ok: base={d}x{d} faces={d}\n", .{
                g.base.?.width,
                g.base.?.height,
                g.face_count,
            });
            g.selftest = .undo;
            g.selftest_t0 = nowNs();
            undoHistory();
        },
        .undo => {
            std.debug.print("[selftest] undo ok: base={d}x{d} faces={d} undo={d} redo={d}\n", .{
                g.base.?.width,
                g.base.?.height,
                g.face_count,
                g.undo_len,
                g.redo_len,
            });
            saveNow(pathValue(g.save_path));
            g.selftest = .done;
            std.debug.print("[selftest] OK\n", .{});
        },
        .off, .done => {},
    }
}

fn selftestSkipStart() void {
    if (g.face_count > 0) g.face_sel[0] = false;
    g.selftest = .compose_skip;
    g.selftest_t0 = nowNs();
    spawnJob(.compose, -1);
}

fn selftestCheck(cond: bool, comptime msg: []const u8, args: anytype) void {
    if (cond) return;
    std.debug.print("[selftest] FAILED: " ++ msg ++ "\n", args);
    g.selftest = .off;
    std.process.exit(1);
}

fn takePendingResult() void {
    const bytes = g.pending_result orelse return;
    g.pending_result = null;
    if (g.result) |*old| old.deinit();
    g.result = ff.Image.fromOwnedBuffer(bytes.ptr, g.pending_w, g.pending_h);
    g.view_gen +%= 1;
    updateDisplay();
}

fn workerMain(s: *State, kind_raw: u32) void {
    const kind: JobKind = @fromBackingInt(@intCast(kind_raw));
    const ok = blk: {
        runJob(s, kind) catch break :blk false;
        break :blk true;
    };
    s.job_ok.store(ok, .release);
    s.job_done.store(true, .release);
    if (s.win) |w| dvui.refresh(w, @src(), null);
}

fn runJob(s: *State, kind: JobKind) !void {
    const t0 = nowNs();
    s.timings = .{};
    // Identity images queued while another job was running are scanned first.
    const added = try drainPendingIdentities(s);
    switch (kind) {
        .init => {
            try ensureEngine(s);
            try prepareProc(s);
            try detectTarget(s);
            try swapAll(s);
            try composeResult(s);
        },
        .reload_target => {
            try ensureEngine(s);
            try prepareProc(s);
            try detectTarget(s);
            try swapAll(s);
            try composeResult(s);
        },
        .add_identity => {
            try ensureEngine(s);
            try prepareProc(s);
            if (s.face_count == 0) try detectTarget(s);
            try swapAll(s);
            try composeResult(s);
        },
        .process_all => {
            try ensureEngine(s);
            try prepareProc(s);
            if (s.face_count == 0) try detectTarget(s);
            try swapAll(s);
            try composeResult(s);
        },
        .process_one => {
            try ensureEngine(s);
            try prepareProc(s);
            if (s.face_count == 0) try detectTarget(s);
            if (s.job_arg >= 0 and @as(usize, @intCast(s.job_arg)) < s.face_count) {
                const ts = nowNs();
                try swapOneFace(s, @intCast(s.job_arg));
                s.timings.swap_ms = nsToMs(nowNs() - ts);
            }
            try composeResult(s);
        },
        .compose => {
            try ensureEngine(s);
            if (added) {
                // A freshly-added identity may be the default now; make sure
                // the faces using it are swapped before recomposing.
                try prepareProc(s);
                if (s.face_count == 0) try detectTarget(s);
                try swapAll(s);
            }
            try composeResult(s);
        },
        .none => {},
    }
    s.timings.total_ms = nsToMs(nowNs() - t0);
    std.debug.print("[job] {s} {d}x{d} faces={d} detect={d:.1} swap={d:.1} enhance={d:.1} total={d:.1} ms\n", .{
        @tagName(kind),      s.proc_w,          s.proc_h,             s.face_count,
        s.timings.detect_ms, s.timings.swap_ms, s.timings.enhance_ms, s.timings.total_ms,
    });
    s.stage_value.store(@backingInt(Stage.done), .monotonic);
    s.progress_value.store(1000, .monotonic);
}

fn ensureEngine(s: *State) !void {
    if (s.engine) |*e| {
        if (e.sequential == s.job_unload) {
            // Reuse the sessions but keep per-run options (mouth restore)
            // in sync with the current UI state.
            e.setRestoreMouth(s.job_restore_mouth);
            return;
        }
        e.deinit();
        s.engine = null;
    }
    setStage(s, .loading, 50);

    const dir_len = std.mem.indexOfScalar(u8, &s.job_model_dir, 0) orelse s.job_model_dir.len;
    if (dir_len == 0) return jobFail(s, "Model directory is empty");
    const dir: [:0]const u8 = s.job_model_dir[0..dir_len :0];

    var errbuf: [1024]u8 = @splat(0);
    s.engine = ff.Engine.create(dir, s.job_provider, s.job_unload, &errbuf) catch {
        return jobFail(s, std.mem.sliceTo(&errbuf, 0));
    };
    const name = s.engine.?.provider();
    const n = @min(name.len, s.provider_name.len);
    @memcpy(s.provider_name[0..n], name[0..n]);
    s.provider_len = n;
    s.engine.?.setRestoreMouth(s.job_restore_mouth);
    setProgress(s, 200, "models ready");
}

fn prepareProc(s: *State) !void {
    const base = &(s.base orelse return jobFail(s, "Load a target image first"));
    s.proc.deinit();
    const use_preview = s.job_fast_preview and @max(base.width, base.height) > preview_long_side;
    if (use_preview) {
        const longest: f32 = @floatFromInt(@max(base.width, base.height));
        const f = preview_long_side / longest;
        const nw: i32 = @max(1, @as(i32, @intFromFloat(@as(f32, @floatFromInt(base.width)) * f)));
        const nh: i32 = @max(1, @as(i32, @intFromFloat(@as(f32, @floatFromInt(base.height)) * f)));
        s.proc = .{ .img = base.resized(nw, nh) catch return jobFail(s, "Cannot downscale target"), .owned = true };
        s.proc_level = preview_level;
    } else {
        s.proc = .{
            .img = .{ .data = base.data, .width = base.width, .height = base.height, .stride = base.stride },
            .owned = false,
        };
        s.proc_level = full_level;
    }
    s.proc_w = s.proc.img.width;
    s.proc_h = s.proc.img.height;
}

/// Detect faces in every queued identity image and append them to the library.
/// Returns true when at least one identity was added.
fn drainPendingIdentities(s: *State) !bool {
    if (!hasPendingIdentities()) return false;
    try ensureEngine(s);
    setStage(s, .detecting, 300);
    const t0 = nowNs();
    var added = false;
    while (true) {
        lockPending();
        if (s.pending_identities.items.len == 0) {
            g.pending_mu.unlock();
            break;
        }
        var p = s.pending_identities.orderedRemove(0);
        g.pending_mu.unlock();
        defer p.img.deinit();
        if (s.identity_count >= max_identities) continue;
        var faces: [max_faces]ff.Face = undefined;
        const n = s.engine.?.detect(&p.img, faces[0..]) catch continue;
        var k: usize = 0;
        while (k < n and s.identity_count < max_identities) : (k += 1) {
            var emb: [512]f32 = @splat(0);
            s.engine.?.embed(&p.img, faces[k], &emb) catch continue;
            const idx = s.identity_count;
            s.identities[idx].embedding = emb;
            if (s.identities[idx].thumb.img) |*im| im.deinit();
            s.identities[idx].thumb.img = makeThumb(&p.img, faces[k].bbox) catch null;
            s.identities[idx].thumb.dirty = true;
            var buf: [64]u8 = undefined;
            const txt = std.fmt.bufPrint(&buf, "{s} #{d}", .{ p.labelSlice(), k }) catch buf[0..0];
            const ln = @min(txt.len, s.identities[idx].label.len);
            @memcpy(s.identities[idx].label[0..ln], txt[0..ln]);
            s.identities[idx].label_len = ln;
            s.identity_count += 1;
            added = true;
        }
    }
    if (added and s.default_identity >= s.identity_count) s.default_identity = 0;
    s.timings.detect_ms = nsToMs(nowNs() - t0);
    setProgress(s, 400, "identities ready");
    return added;
}

/// The patch table matching the resolution `proc` currently holds.
fn patchAt(s: *State, i: usize) *Patch {
    return &s.patches[s.proc_level][i];
}

/// Identity index a face should use: its override, else the default.
fn effectiveIdentity(s: *State, i: usize) usize {
    if (s.identity_count == 0) return 0;
    const v = s.face_identity[i];
    if (v >= 0 and @as(usize, @intCast(v)) < s.identity_count) return @intCast(v);
    return @min(s.default_identity, s.identity_count - 1);
}

/// Whether a face should get its original mouth restored.
fn effectiveMouth(s: *State, i: usize) bool {
    return s.face_mouth_over[i] orelse s.restore_mouth;
}

/// True when the cached patch for face `i` still matches all of its inputs.
fn patchFresh(s: *State, i: usize) bool {
    const p = patchAt(s, i);
    if (!p.valid) return false;
    if (p.proc_w != s.proc_w or p.proc_h != s.proc_h) return false;
    if (p.id_gen != s.identity_gen) return false;
    if (p.identity != @as(i32, @intCast(effectiveIdentity(s, i)))) return false;
    if (p.mouth != effectiveMouth(s, i)) return false;
    return true;
}

/// Map a detected face (stored in base pixels) into the working-image
/// resolution the swapper is currently running at.
fn scaledFace(s: *State, i: usize) ff.Face {
    var f = s.faces[i];
    const base = s.base.?;
    if (s.proc_w != base.width or s.proc_h != base.height) {
        const fx = @as(f32, @floatFromInt(s.proc_w)) / @as(f32, @floatFromInt(base.width));
        const fy = @as(f32, @floatFromInt(s.proc_h)) / @as(f32, @floatFromInt(base.height));
        f.bbox[0] *= fx;
        f.bbox[1] *= fy;
        f.bbox[2] *= fx;
        f.bbox[3] *= fy;
        var k: usize = 0;
        while (k < 5) : (k += 1) {
            f.kps[k * 2] *= fx;
            f.kps[k * 2 + 1] *= fy;
        }
    }
    return f;
}

fn detectTarget(s: *State) !void {
    const base = &(s.base orelse return jobFail(s, "Load a target image first"));
    setStage(s, .detecting, 500);
    const t0 = nowNs();
    var faces: [max_faces]ff.Face = undefined;
    const n = s.engine.?.detect(base, faces[0..]) catch {
        return jobFail(s, s.engine.?.lastError());
    };
    @memcpy(s.faces[0..n], faces[0..n]);
    s.face_count = n;
    for (0..max_faces) |i| {
        s.face_sel[i] = i < n;
        s.face_identity[i] = -1;
        s.face_mouth_over[i] = null;
    }
    clearPatches();
    s.timings.detect_ms = nsToMs(nowNs() - t0);
    if (n == 0) setProgress(s, 600, "no faces found");
}

/// Swap every selected face whose cached patch is missing or stale. Faces that
/// are already up to date (same identity, mouth, resolution) are left alone.
fn swapAll(s: *State) !void {
    if (s.identity_count == 0) return;
    if (s.face_count == 0) return;
    setStage(s, .swapping, 650);
    const t0 = nowNs();
    var total: usize = 0;
    for (0..s.face_count) |i| {
        if (s.face_sel[i] and !patchFresh(s, i)) total += 1;
    }
    if (total == 0) return;
    var done: usize = 0;
    for (0..s.face_count) |i| {
        if (!s.face_sel[i] or patchFresh(s, i)) continue;
        try swapOneFace(s, i);
        done += 1;
        const frac = @as(f32, @floatFromInt(done)) / @as(f32, @floatFromInt(total));
        setProgress(s, 650 + @as(u32, @intFromFloat(frac * 180.0)), "swapping");
    }
    s.timings.swap_ms = nsToMs(nowNs() - t0);
}

fn swapOneFace(s: *State, i: usize) !void {
    if (s.identity_count == 0) return jobFail(s, "Add an identity image first");
    const face = scaledFace(s, i);
    const id = effectiveIdentity(s, i);
    const mouth = effectiveMouth(s, i);
    s.engine.?.setRestoreMouth(mouth);
    var matrix: [6]f64 = @splat(0);
    const patch = s.engine.?.swapPatch(&s.proc.img, &s.identities[id].embedding, face, &matrix) catch {
        return jobFail(s, s.engine.?.lastError());
    };
    const slot = patchAt(s, i);
    if (slot.valid) slot.img.deinit();
    slot.* = .{
        .img = patch,
        .matrix = matrix,
        .valid = true,
        .identity = @intCast(id),
        .mouth = mouth,
        .id_gen = s.identity_gen,
        .proc_w = s.proc_w,
        .proc_h = s.proc_h,
    };
}

/// A fingerprint of everything that affects the composed image. When it is
/// unchanged at the same resolution, `composeResult` can reuse the previous
/// result (and skip the expensive enhancer pass entirely).
fn composeSignature(s: *State) u64 {
    var h = std.hash.Wyhash.init(0);
    h.update(std.mem.asBytes(&s.base_gen));
    h.update(std.mem.asBytes(&s.proc_w));
    h.update(std.mem.asBytes(&s.proc_h));
    h.update(std.mem.asBytes(&s.job_enhancer));
    for (0..s.face_count) |i| {
        const active = s.face_sel[i] and patchAt(s, i).valid;
        h.update(std.mem.asBytes(&active));
        if (active) {
            const id = effectiveIdentity(s, i);
            h.update(std.mem.asBytes(&id));
            const m = effectiveMouth(s, i);
            h.update(std.mem.asBytes(&m));
        }
    }
    return h.final();
}

fn composeResult(s: *State) !void {
    const proc = &s.proc.img;
    const w = proc.width;
    const h = proc.height;
    if (w <= 0 or h <= 0) return jobFail(s, "No target image to compose");

    const sig = composeSignature(s);
    if (s.result_level == s.proc_w and s.result_sig == sig) {
        setProgress(s, 1000, "ready");
        return;
    }

    const bytes = @as(usize, @intCast(w)) * @as(usize, @intCast(h)) * 3;

    const ping = c_alloc.alloc(u8, bytes) catch return jobFail(s, "out of memory");
    defer c_alloc.free(ping);
    const pong = c_alloc.alloc(u8, bytes) catch return jobFail(s, "out of memory");
    defer c_alloc.free(pong);
    @memcpy(ping, proc.data[0..bytes]);

    var cur = ff.Image{ .data = ping.ptr, .width = w, .height = h, .stride = w * 3 };
    var next = ff.Image{ .data = pong.ptr, .width = w, .height = h, .stride = w * 3 };

    setStage(s, .compositing, 850);
    var active: [max_faces]ff.Face = undefined;
    var active_n: usize = 0;
    for (0..s.face_count) |i| {
        const p = patchAt(s, i);
        if (!s.face_sel[i] or !p.valid) continue;
        ff.pastePatch(&cur, &p.img, &p.matrix, &next) catch {
            return jobFail(s, "patch paste failed");
        };
        std.mem.swap(ff.Image, &cur, &next);
        active[active_n] = scaledFace(s, i);
        active_n += 1;
    }

    if (s.job_enhancer != 0 and active_n > 0) {
        s.engine.?.setEnhancer(s.job_enhancer);
        setStage(s, .enhancing, 900);
        const t0 = nowNs();
        s.engine.?.enhance(&cur, active[0..active_n], &next, progressCb, s) catch {
            return jobFail(s, s.engine.?.lastError());
        };
        std.mem.swap(ff.Image, &cur, &next);
        s.timings.enhance_ms = nsToMs(nowNs() - t0);
    } else {
        s.engine.?.setEnhancer(0);
    }

    const out = c_alloc.dupe(u8, cur.data[0..bytes]) catch return jobFail(s, "out of memory");
    s.pending_result = out;
    s.pending_w = w;
    s.pending_h = h;
    s.result_sig = sig;
    s.result_level = s.proc_w;
    setProgress(s, 1000, "ready");
}

fn largestFace(faces: []const ff.Face) usize {
    var best: usize = 0;
    var best_area: f32 = -1;
    for (faces, 0..) |f, i| {
        const area = (f.bbox[2] - f.bbox[0]) * (f.bbox[3] - f.bbox[1]);
        if (area > best_area) {
            best_area = area;
            best = i;
        }
    }
    return best;
}

fn progressCb(stage_raw: i32, progress: f32, message: [*c]const u8, user: ?*anyopaque) callconv(.c) void {
    _ = message;
    const s: *State = @ptrCast(@alignCast(user orelse return));
    const p = @as(u32, @intFromFloat(std.math.clamp(progress, 0.0, 1.0) * 100.0));
    switch (stage_raw) {
        0 => {
            s.stage_value.store(@backingInt(Stage.detecting), .monotonic);
            s.progress_value.store(400 + p * 2, .monotonic);
        },
        2 => {
            s.stage_value.store(@backingInt(Stage.swapping), .monotonic);
            s.progress_value.store(650 + p, .monotonic);
        },
        3 => {
            s.stage_value.store(@backingInt(Stage.enhancing), .monotonic);
            s.progress_value.store(900 + p, .monotonic);
        },
        else => {},
    }
    if (s.win) |w| dvui.refresh(w, @src(), null);
}

fn setStage(s: *State, st: Stage, permille: u32) void {
    s.stage_value.store(@backingInt(st), .monotonic);
    setProgress(s, permille, st.text());
}

fn setProgress(s: *State, permille: u32, message: []const u8) void {
    _ = message;
    s.progress_value.store(@min(permille, 1000), .monotonic);
    if (s.win) |w| dvui.refresh(w, @src(), null);
}

fn jobFail(s: *State, message: []const u8) error{JobFailed} {
    const n = @min(message.len, s.job_error.len);
    @memcpy(s.job_error[0..n], message[0..n]);
    s.job_error_len = n;
    return error.JobFailed;
}

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

fn busy() bool {
    return g.working.load(.acquire);
}

fn providerName() []const u8 {
    return g.provider_name[0..g.provider_len];
}

fn rectRightNatural(rs: *const dvui.RectScale) f32 {
    if (rs.s == 0) return 0;
    return (rs.r.x + rs.r.w) / rs.s;
}

fn baseName(path: []const u8) []const u8 {
    var i = path.len;
    while (i > 0) : (i -= 1) {
        const ch = path[i - 1];
        if (ch == '/' or ch == '\\') return path[i..];
    }
    return path;
}

fn dupeZ(text: []const u8) ?[:0]u8 {
    return std.mem.Allocator.dupeSentinel(g.allocator, u8, text, 0) catch null;
}

fn nsToMs(ns: i128) f64 {
    return @as(f64, @floatFromInt(ns)) / 1_000_000.0;
}

fn nowNs() i128 {
    return @as(i128, ff.nowNs());
}

fn setStatus(text: []const u8, is_error: bool) void {
    const n = @min(text.len, g.status.len);
    @memcpy(g.status[0..n], text[0..n]);
    g.status_len = n;
    g.status_is_error = is_error;
}

fn setStatusFmt(comptime fmt: []const u8, args: anytype, is_error: bool) void {
    const text = std.fmt.bufPrint(g.status[0..], fmt, args) catch {
        setStatus("(message too long)", is_error);
        return;
    };
    g.status_len = text.len;
    g.status_is_error = is_error;
}

fn pollMemory() void {
    const now: i64 = @intCast(@divTrunc(nowNs(), std.time.ns_per_ms));
    if (now - g.last_memory_poll < 250) return;
    g.last_memory_poll = now;
    const m = ff.memoryInfo();
    g.vram_used = m.vram_used_mb;
    g.vram_total = m.vram_total_mb;
    g.rss = m.rss_mb;
}

// ---------------------------------------------------------------------------
// Video workspace bridge
// ---------------------------------------------------------------------------

fn isVideoPath(path: []const u8) bool {
    const exts = [_][]const u8{ ".mp4", ".mov", ".mkv", ".webm", ".avi", ".m4v", ".mpg", ".mpeg" };
    for (exts) |ext| {
        if (path.len > ext.len and std.ascii.eqlIgnoreCase(path[path.len - ext.len ..], ext)) return true;
    }
    return false;
}

/// Create (or reuse) an engine from the UI thread or a video worker. Photo
/// jobs are not running while video jobs run, so this cannot race with them.
fn ensureEngineSync() ?*ff.Engine {
    return ensureEngineSyncSeq(g.unload_after_run);
}

/// Create (or reuse) an engine with the requested session strategy. Video jobs
/// ask for sequential sessions so a 4 GB GPU can run detection, swapping and
/// enhancement in the same process.
fn ensureEngineSyncSeq(sequential: bool) ?*ff.Engine {
    if (g.engine) |*e| {
        if (e.sequential == sequential) {
            e.setRestoreMouth(g.restore_mouth);
            return e;
        }
        e.deinit();
        g.engine = null;
    }
    const models = pathValue(g.model_dir);
    if (models.len == 0) return null;
    var dir_buf: [1024]u8 = @splat(0);
    const n = @min(models.len, dir_buf.len - 1);
    @memcpy(dir_buf[0..n], models[0..n]);
    const dir: [:0]const u8 = dir_buf[0..n :0];
    const provider: i32 = switch (g.provider) {
        .cuda => 1,
        .cpu => 2,
        .auto => 0,
    };
    var errbuf: [1024]u8 = @splat(0);
    g.engine = ff.Engine.create(dir, provider, sequential, &errbuf) catch {
        const msg = std.mem.sliceTo(&errbuf, 0);
        setStatus(if (msg.len > 0) msg else "Cannot load the models", true);
        return null;
    };
    const name = g.engine.?.provider();
    const m = @min(name.len, g.provider_name.len);
    @memcpy(g.provider_name[0..m], name[0..m]);
    g.provider_len = m;
    g.engine.?.setRestoreMouth(g.restore_mouth);
    return &g.engine.?;
}

fn videoHost() video.Host {
    return .{
        .user = @ptrCast(&g),
        .allocator = g.allocator,
        .ensure_engine = vhEnsureEngine,
        .ensure_engine_seq = vhEnsureEngineSeq,
        .engine_busy = vhEngineBusy,
        .identity_count = vhIdentityCount,
        .identity_embedding = vhIdentityEmbedding,
        .identity_label = vhIdentityLabel,
        .identity_thumb = vhIdentityThumb,
        .default_identity = vhDefaultIdentity,
        .enhancer = vhEnhancer,
        .restore_mouth = vhRestoreMouth,
        .set_restore_mouth = vhSetRestoreMouth,
        .draw_identities = vhDrawIdentities,
        .status = vhStatus,
        .temp_dir = vhTempDir,
        .export_dir = vhExportDir,
    };
}

fn vhTempDir(_: *anyopaque) []const u8 {
    return pathValue(g.temp_dir);
}

fn vhExportDir(_: *anyopaque) []const u8 {
    return pathValue(g.export_dir);
}

fn vhEnsureEngine(_: *anyopaque) ?*ff.Engine {
    return ensureEngineSync();
}

fn vhEnsureEngineSeq(_: *anyopaque, sequential: bool) ?*ff.Engine {
    return ensureEngineSyncSeq(sequential);
}

fn vhEngineBusy(_: *anyopaque) bool {
    return g.working.load(.acquire);
}

fn vhIdentityCount(_: *anyopaque) usize {
    return g.identity_count;
}

fn vhIdentityEmbedding(_: *anyopaque, i: usize) *const [512]f32 {
    return &g.identities[i].embedding;
}

fn vhIdentityLabel(_: *anyopaque, i: usize) []const u8 {
    return g.identities[i].labelSlice();
}

fn vhIdentityThumb(_: *anyopaque, i: usize) ?dvui.Texture {
    if (i >= g.identity_count) return null;
    return g.identities[i].thumb.tex;
}

fn vhDefaultIdentity(_: *anyopaque) usize {
    return g.default_identity;
}

fn vhEnhancer(_: *anyopaque) i32 {
    return @intCast(@backingInt(g.enhancer));
}

fn vhRestoreMouth(_: *anyopaque) bool {
    return g.restore_mouth;
}

fn vhSetRestoreMouth(_: *anyopaque, on: bool) void {
    g.restore_mouth = on;
}

fn vhDrawIdentities(_: *anyopaque) void {
    drawIdentityGrid();
}

fn vhStatus(_: *anyopaque, text: []const u8, is_error: bool) void {
    setStatus(text, is_error);
}
