//! Persistent application settings.
//!
//! Only rarely-modified options live here: execution provider, the folders for
//! models, temporary files and exports, and update-check preferences. The
//! frequently used controls (enhancer, mouth restoration, fast preview, canvas
//! overlay) stay in the workspace panels and are intentionally *not* persisted.
//!
//! Precedence at startup is CLI > environment > config file > defaults. The
//! file is JSON and is written next to the user's configuration directory:
//!   Linux/macOS  $XDG_CONFIG_HOME/faceforge/settings.json (or ~/.config/...)
//!   Windows      %APPDATA%\faceforge\settings.json
//!
//! A `null`/missing field keeps its default, so the file stays forward
//! compatible when new options are added.

const std = @import("std");
const ff = @import("ff.zig");

const builtin = @import("builtin");
const is_windows = builtin.os.tag == .windows;

pub const file_version: u32 = 1;

/// Live values collected from the config file, the environment and the CLI.
pub const Values = struct {
    provider: u8 = 0,
    models_dir: []const u8 = "models",
    temp_dir: []const u8 = "",
    export_dir: []const u8 = "",
    unload_after_run: bool = false,
    check_updates: bool = true,
    update_interval_hours: u32 = 24,
    last_update_check: i64 = 0,
};

/// On-disk representation. Optional fields distinguish "not set" from an
/// intentional empty string.
const Disk = struct {
    version: ?u32 = null,
    provider: ?[]const u8 = null,
    models_dir: ?[]const u8 = null,
    temp_dir: ?[]const u8 = null,
    export_dir: ?[]const u8 = null,
    unload_after_run: ?bool = null,
    check_updates: ?bool = null,
    update_interval_hours: ?u32 = null,
    last_update_check: ?i64 = null,
};

pub const Loaded = struct {
    values: Values,
    parsed: std.json.Parsed(Disk),
};

pub fn separator() []const u8 {
    return if (is_windows) "\\" else "/";
}

/// Join two path fragments, avoiding a doubled separator when `a` already ends
/// with one. The caller owns the returned slice.
pub fn join(allocator: std.mem.Allocator, a: []const u8, b: []const u8) ![]u8 {
    if (a.len == 0) return allocator.dupe(u8, b);
    if (b.len == 0) return allocator.dupe(u8, a);
    const sep = separator();
    const a_ends = std.mem.endsWith(u8, a, "/") or std.mem.endsWith(u8, a, "\\");
    if (a_ends) return std.fmt.allocPrint(allocator, "{s}{s}", .{ a, b });
    return std.fmt.allocPrint(allocator, "{s}{s}{s}", .{ a, sep, b });
}

/// Directory part of `path`, or "" when it has none.
pub fn dirName(path: []const u8) []const u8 {
    const idx = std.mem.lastIndexOfAny(u8, path, if (is_windows) "\\/" else "/") orelse return "";
    if (idx == 0) return path[0..1];
    return path[0..idx];
}

/// Platform-specific location of the settings file.
pub fn defaultPath(allocator: std.mem.Allocator, env: *const std.process.Environ.Map) ![]u8 {
    if (is_windows) {
        const base = env.get("APPDATA") orelse env.get("USERPROFILE") orelse ".";
        const dir = try join(allocator, base, "faceforge");
        defer allocator.free(dir);
        return join(allocator, dir, "settings.json");
    }

    const base = if (env.get("XDG_CONFIG_HOME")) |xdg|
        try allocator.dupe(u8, xdg)
    else blk: {
        const home = env.get("HOME") orelse ".";
        break :blk try join(allocator, home, ".config");
    };
    defer allocator.free(base);

    const dir = try join(allocator, base, "faceforge");
    defer allocator.free(dir);
    return join(allocator, dir, "settings.json");
}

/// Read the JSON file. Returns null when it is missing, unreadable or corrupt;
/// the caller simply keeps the defaults in that case.
pub fn load(allocator: std.mem.Allocator, path: []const u8) ?Loaded {
    if (path.len == 0) return null;
    const pz = allocator.dupeSentinel(u8, path, 0) catch return null;
    defer allocator.free(pz);
    const text = ff.readFile(pz, allocator) orelse return null;
    defer allocator.free(text);

    // `alloc_always` is required: with the default `.alloc_if_needed` the
    // string fields may point into `text`, which is freed before the caller
    // gets to copy them, leaving dangling paths.
    const parsed = std.json.parseFromSlice(Disk, allocator, text, .{
        .ignore_unknown_fields = true,
        .allocate = .alloc_always,
    }) catch return null;
    const d = parsed.value;

    var values = Values{};
    if (d.provider) |p| values.provider = providerFromName(p);
    if (d.models_dir) |p| values.models_dir = p;
    if (d.temp_dir) |p| values.temp_dir = p;
    if (d.export_dir) |p| values.export_dir = p;
    if (d.unload_after_run) |v| values.unload_after_run = v;
    if (d.check_updates) |v| values.check_updates = v;
    if (d.update_interval_hours) |v| values.update_interval_hours = std.math.clamp(v, 1, 24 * 365);
    if (d.last_update_check) |v| values.last_update_check = v;

    return .{ .values = values, .parsed = parsed };
}

/// Atomically-ish writes the settings file, creating its directory when
/// needed. Returns false when the path cannot be written.
pub fn save(allocator: std.mem.Allocator, path: []const u8, v: Values) bool {
    if (path.len == 0) return false;

    const dir = dirName(path);
    if (dir.len > 0) {
        const dz = allocator.dupeSentinel(u8, dir, 0) catch return false;
        defer allocator.free(dz);
        _ = ff.mkdirs(dz);
    }

    const disk = .{
        .version = file_version,
        .provider = providerName(v.provider),
        .models_dir = v.models_dir,
        .temp_dir = v.temp_dir,
        .export_dir = v.export_dir,
        .unload_after_run = v.unload_after_run,
        .check_updates = v.check_updates,
        .update_interval_hours = v.update_interval_hours,
        .last_update_check = v.last_update_check,
    };
    const text = std.json.Stringify.valueAlloc(allocator, disk, .{
        .whitespace = .indent_2,
        .emit_null_optional_fields = false,
    }) catch return false;
    defer allocator.free(text);

    const pz = allocator.dupeSentinel(u8, path, 0) catch return false;
    defer allocator.free(pz);
    return ff.writeFile(pz, text);
}

pub fn providerName(value: u8) []const u8 {
    return switch (value) {
        1 => "cuda",
        2 => "cpu",
        else => "auto",
    };
}

pub fn providerFromName(name: []const u8) u8 {
    if (std.ascii.eqlIgnoreCase(name, "cuda") or std.ascii.eqlIgnoreCase(name, "gpu")) return 1;
    if (std.ascii.eqlIgnoreCase(name, "cpu")) return 2;
    return 0;
}

/// `"1.2.3"` / `"v1.2.3"` (and pre-release suffixes) to a comparable triple.
pub fn parseVersion(text: []const u8) ?[3]u32 {
    var s = std.mem.trim(u8, text, " \t\r\n");
    if (s.len > 0 and (s[0] == 'v' or s[0] == 'V')) s = s[1..];
    var out: [3]u32 = .{ 0, 0, 0 };
    var it = std.mem.splitScalar(u8, s, '.');
    var i: usize = 0;
    while (it.next()) |part| {
        if (i >= out.len) break;
        var digits = part;
        if (std.mem.indexOfAny(u8, digits, "-+")) |cut| digits = digits[0..cut];
        out[i] = std.fmt.parseInt(u32, digits, 10) catch return null;
        i += 1;
    }
    if (i == 0) return null;
    return out;
}

pub fn versionNewer(candidate: []const u8, current: []const u8) bool {
    const a = parseVersion(candidate) orelse return false;
    const b = parseVersion(current) orelse return false;
    for (a, b) |x, y| {
        if (x != y) return x > y;
    }
    return false;
}

test "version comparison" {
    try std.testing.expect(versionNewer("v0.2.0", "0.1.9"));
    try std.testing.expect(versionNewer("1.0.0", "0.9.9"));
    try std.testing.expect(!versionNewer("v0.1.0", "0.1.0"));
    try std.testing.expect(!versionNewer("0.0.9", "0.1.0"));
    try std.testing.expect(parseVersion("v1.2.3-rc1") != null);
}
