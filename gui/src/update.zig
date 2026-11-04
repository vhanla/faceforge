//! GitHub release checker.
//!
//! The request runs on a detached worker thread so a slow or offline network
//! never blocks the UI. Results are handed back through a mutex-protected
//! snapshot that the About panel reads once per frame.
//!
//! Endpoint: https://api.github.com/repos/vhanla/faceforge/releases/latest
//! A repository with no releases replies 404; that is treated as "up to date"
//! rather than an error.

const std = @import("std");
const config = @import("config.zig");

pub const repo_url = "https://github.com/vhanla/faceforge";
pub const releases_url = repo_url ++ "/releases";
const api_url = "https://api.github.com/repos/vhanla/faceforge/releases/latest";

pub const Status = enum(u8) {
    idle,
    checking,
    up_to_date,
    available,
    failed,
};

pub const Snapshot = struct {
    status: Status = .idle,
    tag_buf: [64]u8 = @splat(0),
    tag_len: usize = 0,
    url_buf: [512]u8 = @splat(0),
    url_len: usize = 0,
    message_buf: [192]u8 = @splat(0),
    message_len: usize = 0,

    pub fn tag(self: *const Snapshot) []const u8 {
        return self.tag_buf[0..self.tag_len];
    }

    pub fn url(self: *const Snapshot) []const u8 {
        return self.url_buf[0..self.url_len];
    }

    pub fn message(self: *const Snapshot) []const u8 {
        return self.message_buf[0..self.message_len];
    }
};

var mutex: std.atomic.Mutex = .unlocked;
var shared: Snapshot = .{};
var worker_alive = std.atomic.Value(bool).init(false);
var worker_started = std.atomic.Value(i64).init(0);

/// A check without a result after this long is treated as stalled so the user
/// can retry instead of staring at "Checking..." forever.
const stale_after_s = 30;

fn lockMutex() void {
    while (!mutex.tryLock()) std.Thread.yield() catch {};
}

/// Copy of the latest result, safe to call from the GUI thread.
pub fn snapshot() Snapshot {
    lockMutex();
    defer mutex.unlock();
    return shared;
}

/// True while a check is actually in flight. `now` is Unix seconds (see
/// `nowSeconds` in the caller); a worker that has been silent for too long is
/// reported as finished so the UI can offer a retry.
pub fn isCheckingAt(now: i64) bool {
    if (!worker_alive.load(.acquire)) return false;
    const started = worker_started.load(.acquire);
    if (started <= 0) return true;
    return now - started < stale_after_s;
}

fn realSeconds(io: std.Io) i64 {
    const ts = std.Io.Clock.real.now(io);
    return @intCast(@divTrunc(ts.nanoseconds, std.time.ns_per_s));
}

/// Start a check when one is not already running. `current` is the running app
/// version (for example `"0.1.0"`) and `io` is the process Io implementation
/// taken from `dvui.App.main_init`.
pub fn check(current: []const u8, io: std.Io) void {
    const now = realSeconds(io);
    if (worker_alive.load(.acquire)) {
        const started = worker_started.load(.acquire);
        if (started <= 0 or now - started < stale_after_s) return;
        // The previous attempt is wedged in a network call. Let the user try
        // again; its eventual result is harmless if it ever arrives.
    }

    const owned = std.heap.c_allocator.dupeSentinel(u8, current, 0) catch return;

    worker_alive.store(true, .release);
    worker_started.store(now, .release);
    set(.{ .status = .checking, .message = "Contacting GitHub..." });

    const thread = std.Thread.spawn(.{}, worker, .{ io, owned, now }) catch {
        std.heap.c_allocator.free(owned);
        worker_alive.store(false, .release);
        worker_started.store(0, .release);
        set(.{ .status = .failed, .message = "Could not start the update check" });
        return;
    };
    thread.detach();
}

/// True when the periodic check is enabled and enough time has passed.
/// `now` is the current Unix time in seconds (see `nowSeconds` in the caller).
pub fn due(enabled: bool, interval_hours: u32, last_check: i64, now: i64) bool {
    if (!enabled) return false;
    if (last_check <= 0) return true;
    const hours = std.math.clamp(interval_hours, 1, 24 * 365);
    const elapsed = now - last_check;
    return elapsed >= @as(i64, @intCast(hours)) * 3600;
}

fn worker(io: std.Io, current: [:0]const u8, started: i64) void {
    defer {
        std.heap.c_allocator.free(current);
        // Only clear the flag when a newer check has not replaced this one.
        if (worker_started.cmpxchgStrong(started, 0, .acq_rel, .acquire) == null) {
            worker_alive.store(false, .release);
        }
    }

    const alloc = std.heap.c_allocator;
    var client = std.http.Client{ .allocator = alloc, .io = io };
    defer client.deinit();

    var body: std.Io.Writer.Allocating = .init(alloc);
    defer body.deinit();

    const response = client.fetch(.{
        .location = .{ .url = api_url },
        .response_writer = &body.writer,
        .extra_headers = &.{
            .{ .name = "User-Agent", .value = "faceforge-update-check" },
            .{ .name = "Accept", .value = "application/vnd.github+json" },
        },
    }) catch |err| {
        std.debug.print("[update] fetch failed: {s}\n", .{@errorName(err)});
        set(.{ .status = .failed, .message = "Could not reach GitHub" });
        return;
    };

    if (response.status == .not_found) {
        set(.{ .status = .up_to_date, .message = "No releases published yet" });
        return;
    }
    if (response.status != .ok) {
        var buf: [96]u8 = undefined;
        const msg = std.fmt.bufPrint(&buf, "GitHub replied {d}", .{@backingInt(response.status)}) catch "GitHub request failed";
        set(.{ .status = .failed, .message = msg });
        return;
    }

    var parsed = std.json.parseFromSlice(std.json.Value, alloc, body.written(), .{}) catch {
        set(.{ .status = .failed, .message = "Unexpected response from GitHub" });
        return;
    };
    defer parsed.deinit();

    const obj = switch (parsed.value) {
        .object => |o| o,
        else => {
            set(.{ .status = .failed, .message = "Unexpected response from GitHub" });
            return;
        },
    };

    const tag = if (obj.get("tag_name")) |v| switch (v) {
        .string => |s| s,
        else => {
            set(.{ .status = .failed, .message = "Release has no tag" });
            return;
        },
    } else {
        set(.{ .status = .failed, .message = "Release has no tag" });
        return;
    };

    const html = if (obj.get("html_url")) |v| switch (v) {
        .string => |s| s,
        else => releases_url,
    } else releases_url;

    if (config.versionNewer(tag, current)) {
        set(.{ .status = .available, .tag = tag, .url = html, .message = "A newer release is available" });
    } else {
        set(.{ .status = .up_to_date, .tag = tag, .url = html, .message = "You are running the latest version" });
    }
}

const Update = struct {
    status: Status,
    tag: []const u8 = "",
    url: []const u8 = "",
    message: []const u8 = "",
};

fn set(u: Update) void {
    lockMutex();
    defer mutex.unlock();
    shared.status = u.status;
    shared.tag_len = @min(u.tag.len, shared.tag_buf.len);
    @memcpy(shared.tag_buf[0..shared.tag_len], u.tag[0..shared.tag_len]);
    shared.url_len = @min(u.url.len, shared.url_buf.len);
    @memcpy(shared.url_buf[0..shared.url_len], u.url[0..shared.url_len]);
    shared.message_len = @min(u.message.len, shared.message_buf.len);
    @memcpy(shared.message_buf[0..shared.message_len], u.message[0..shared.message_len]);
}
