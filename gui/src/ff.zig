//! Thin Zig wrappers over the faceforge C ABI (see core/include/faceforge.h).
//! The translated header is provided by build.zig as the `ffi` module.

const std = @import("std");
const c = @import("ffi");

pub const Face = c.ff_face;
pub const Stats = c.ff_stats;

pub const Engine = struct {
    ptr: *c.ff_engine,
    /// True when sessions are closed between pipeline stages.
    sequential: bool,

    pub fn create(model_dir: [:0]const u8, provider_kind: i32, sequential: bool, err_buf: []u8) !Engine {
        var opt: c.ff_options = undefined;
        c.ff_options_default(&opt);
        opt.provider = provider_kind;
        opt.sequential = if (sequential) 1 else 0;

        var msg: [1024]u8 = @splat(0);
        const handle = c.ff_engine_create(model_dir.ptr, &opt, &msg, @intCast(msg.len));
        if (handle == null) {
            const text = std.mem.sliceTo(&msg, 0);
            const n = @min(text.len, err_buf.len);
            @memcpy(err_buf[0..n], text[0..n]);
            if (err_buf.len > n) err_buf[n] = 0;
            return error.EngineCreateFailed;
        }
        return .{ .ptr = handle.?, .sequential = sequential };
    }

    pub fn deinit(self: *Engine) void {
        c.ff_engine_destroy(self.ptr);
        self.* = undefined;
    }

    pub fn provider(self: Engine) []const u8 {
        return cString(c.ff_engine_provider(self.ptr));
    }

    pub fn lastError(self: Engine) []const u8 {
        return cString(c.ff_engine_last_error(self.ptr));
    }

    pub fn unload(self: Engine) void {
        _ = c.ff_engine_unload(self.ptr);
    }

    pub fn setEnhancer(self: Engine, enhancer: i32) void {
        _ = c.ff_engine_set_enhancer(self.ptr, enhancer);
    }

    pub fn setRestoreMouth(self: Engine, on: bool) void {
        _ = c.ff_engine_set_restore_mouth(self.ptr, if (on) 1 else 0);
    }

    pub fn detect(self: Engine, img: *const Image, out: []Face) !usize {
        var count: i32 = 0;
        const rc = c.ff_detect(self.ptr, img.data, img.width, img.height, img.stride, out.ptr, @intCast(out.len), &count);
        if (rc != c.FF_OK) return error.DetectFailed;
        return @intCast(count);
    }

    pub fn embed(self: Engine, img: *const Image, face: Face, out: *[512]f32) !void {
        const rc = c.ff_embed(self.ptr, img.data, img.width, img.height, img.stride, &face, out);
        if (rc != c.FF_OK) return error.EmbedFailed;
    }

    pub fn swapFace(self: Engine, img: *const Image, embedding: *const [512]f32, face: Face, out: *Image) !void {
        const rc = c.ff_swap_face(self.ptr, img.data, img.width, img.height, img.stride, embedding, &face, 1, out.data);
        if (rc != c.FF_OK) return error.SwapFailed;
    }

    /// Run the swapper for one face and return the raw 128x128 patch together
    /// with the affine matrix that places it in the source image.
    pub fn swapPatch(self: Engine, img: *const Image, embedding: *const [512]f32, face: Face, out_matrix: *[6]f64) !Image {
        var patch: [*c]u8 = null;
        const rc = c.ff_swap_patch(self.ptr, img.data, img.width, img.height, img.stride, embedding, &face, &patch, out_matrix);
        if (rc != c.FF_OK or patch == null) return error.SwapFailed;
        return Image.fromOwnedBuffer(@ptrCast(patch.?), 128, 128);
    }

    pub fn enhance(self: Engine, img: *const Image, faces: []const Face, out: *Image, progress: ?ProgressFn, user: ?*anyopaque) !void {
        const rc = c.ff_enhance(self.ptr, img.data, img.width, img.height, img.stride, if (faces.len > 0) faces.ptr else null, @intCast(faces.len), out.data, progress, user);
        if (rc != c.FF_OK) return error.EnhanceFailed;
    }
};

pub const ProgressFn = *const fn (stage: i32, progress: f32, message: [*c]const u8, user: ?*anyopaque) callconv(.c) void;

/// On-demand video decoder. One handle per thread; frames are decoded straight
/// from the container and returned as tightly packed BGR images.
pub const Video = struct {
    ptr: *c.ff_video,

    pub const Info = struct {
        width: i32 = 0,
        height: i32 = 0,
        fps: f64 = 25.0,
        frames: i64 = 0,
        duration: f64 = 0,
    };

    pub fn open(path: [:0]const u8) !Video {
        var err: [512]u8 = @splat(0);
        const handle = c.ff_video_open(path.ptr, &err, @intCast(err.len));
        if (handle == null) return error.VideoOpenFailed;
        return .{ .ptr = handle.? };
    }

    pub fn deinit(self: *Video) void {
        c.ff_video_close(self.ptr);
        self.* = undefined;
    }

    pub fn info(self: *const Video) Info {
        var out = Info{};
        _ = c.ff_video_info(self.ptr, &out.width, &out.height, &out.fps, &out.frames, &out.duration);
        return out;
    }

    /// Decode one frame. The returned image owns a malloc'd buffer and must be
    /// released with `deinit`.
    pub fn frame(self: *Video, index: i64) !Image {
        var data: [*c]u8 = null;
        var w: i32 = 0;
        var h: i32 = 0;
        var stride: i32 = 0;
        const rc = c.ff_video_frame(self.ptr, index, &data, &w, &h, &stride);
        if (rc != c.FF_OK or data == null) return error.VideoFrameFailed;
        return .{
            .data = @ptrCast(data),
            .width = w,
            .height = h,
            .stride = stride,
        };
    }
};

/// Streams raw BGR frames into ffmpeg (no intermediate image sequence).
pub const VideoWriter = struct {
    ptr: *c.ff_video_writer,

    pub fn open(path: [:0]const u8, width: i32, height: i32, fps: f64, encoder: [:0]const u8, crf: i32, preset: [:0]const u8, ffmpeg: [:0]const u8) !VideoWriter {
        var err: [512]u8 = @splat(0);
        const handle = c.ff_video_writer_open(path.ptr, width, height, fps, encoder.ptr, crf, preset.ptr, ffmpeg.ptr, &err, @intCast(err.len));
        if (handle == null) return error.WriterOpenFailed;
        return .{ .ptr = handle.? };
    }

    pub fn write(self: *VideoWriter, img: *const Image) !void {
        const rc = c.ff_video_writer_frame(self.ptr, img.data, img.width, img.height, img.stride);
        if (rc != c.FF_OK) return error.WriterFrameFailed;
    }

    /// Close the pipe; returns error when ffmpeg exited unsuccessfully.
    pub fn finish(self: *VideoWriter) !void {
        const rc = c.ff_video_writer_close(self.ptr, null, 0);
        self.* = undefined;
        if (rc != c.FF_OK) return error.WriterCloseFailed;
    }
};

pub fn concatVideos(list_path: [:0]const u8, out_path: [:0]const u8, ffmpeg: [:0]const u8) !void {
    const rc = c.ff_video_concat(list_path.ptr, out_path.ptr, ffmpeg.ptr, null, 0);
    if (rc != c.FF_OK) return error.ConcatFailed;
}

pub fn muxAudio(video_path: [:0]const u8, source_path: [:0]const u8, start_sec: f64, duration: f64, out_path: [:0]const u8, ffmpeg: [:0]const u8) !void {
    const rc = c.ff_video_mux_audio(video_path.ptr, source_path.ptr, start_sec, duration, out_path.ptr, ffmpeg.ptr, null, 0);
    if (rc != c.FF_OK) return error.MuxFailed;
}

pub fn ffmpegProbe(ffmpeg: [:0]const u8, out: []u8) bool {
    out[0] = 0;
    return c.ff_ffmpeg_probe(ffmpeg.ptr, out.ptr, @intCast(out.len)) == c.FF_OK;
}

pub fn mkdirs(path: [:0]const u8) bool {
    return c.ff_fs_mkdirs(path.ptr) == c.FF_OK;
}

pub fn fileExists(path: [:0]const u8) bool {
    return c.ff_file_exists(path.ptr) != 0;
}

pub fn readFile(path: [:0]const u8, allocator: std.mem.Allocator) ?[]u8 {
    var data: [*c]u8 = null;
    var size: i32 = 0;
    if (c.ff_file_read(path.ptr, &data, &size) != c.FF_OK or data == null) return null;
    defer c.ff_image_free(data);
    const out = allocator.alloc(u8, @intCast(size)) catch return null;
    @memcpy(out, data[0..@intCast(size)]);
    return out;
}

pub fn writeFile(path: [:0]const u8, data: []const u8) bool {
    return c.ff_file_write(path.ptr, data.ptr, @intCast(data.len)) == c.FF_OK;
}

pub fn removeFile(path: [:0]const u8) void {
    _ = c.ff_file_remove(path.ptr);
}

pub fn copyFile(src: [:0]const u8, dst: [:0]const u8) bool {
    return c.ff_file_copy(src.ptr, dst.ptr) == c.FF_OK;
}

pub const Image = struct {
    data: [*]u8,
    width: i32,
    height: i32,
    stride: i32,

    pub fn load(path: [:0]const u8) !Image {
        var data: [*c]u8 = null;
        var w: i32 = 0;
        var h: i32 = 0;
        var stride: i32 = 0;
        const rc = c.ff_image_load(path.ptr, &data, &w, &h, &stride);
        if (rc != c.FF_OK or data == null) return error.ImageLoadFailed;
        return .{
            .data = @ptrCast(data),
            .width = w,
            .height = h,
            .stride = stride,
        };
    }

    /// Decode an encoded image (PNG/JPEG/WEBP/...) held in memory.
    pub fn decode(bytes: []const u8) !Image {
        var data: [*c]u8 = null;
        var w: i32 = 0;
        var h: i32 = 0;
        var stride: i32 = 0;
        const rc = c.ff_image_decode(bytes.ptr, @intCast(bytes.len), &data, &w, &h, &stride);
        if (rc != c.FF_OK or data == null) return error.ImageDecodeFailed;
        return .{
            .data = @ptrCast(data),
            .width = w,
            .height = h,
            .stride = stride,
        };
    }

    /// Take ownership of a tightly packed BGR buffer allocated with malloc
    /// (which is what ff_image_free uses).
    pub fn fromOwnedBuffer(data: [*]u8, width: i32, height: i32) Image {
        return .{ .data = data, .width = width, .height = height, .stride = width * 3 };
    }

    pub fn deinit(self: *Image) void {
        c.ff_image_free(self.data);
        self.* = undefined;
    }

    pub fn byteLen(self: *const Image) usize {
        return @as(usize, @intCast(self.stride)) * @as(usize, @intCast(self.height));
    }

    pub fn save(self: *const Image, path: [:0]const u8) !void {
        const rc = c.ff_image_save(path.ptr, self.data, self.width, self.height, self.stride);
        if (rc != c.FF_OK) return error.ImageSaveFailed;
    }

    /// Encode to PNG. The returned slice is owned by `allocator`.
    pub fn encodePng(self: *const Image, allocator: std.mem.Allocator) ![]u8 {
        var data: [*c]u8 = null;
        var size: i32 = 0;
        const rc = c.ff_image_encode_png(self.data, self.width, self.height, self.stride, &data, &size);
        if (rc != c.FF_OK or data == null) return error.ImageEncodeFailed;
        defer c.ff_image_free(data);
        const out = try allocator.alloc(u8, @intCast(size));
        @memcpy(out, data[0..@intCast(size)]);
        return out;
    }

    pub fn clone(self: *const Image) !Image {
        var data: [*c]u8 = null;
        var stride: i32 = 0;
        const rc = c.ff_image_clone(self.data, self.width, self.height, self.stride, &data, &stride);
        if (rc != c.FF_OK or data == null) return error.ImageCloneFailed;
        return Image.fromOwnedBuffer(@ptrCast(data.?), self.width, self.height);
    }

    pub fn resized(self: *const Image, new_w: i32, new_h: i32) !Image {
        var data: [*c]u8 = null;
        var stride: i32 = 0;
        const rc = c.ff_image_resize(self.data, self.width, self.height, self.stride, new_w, new_h, &data, &stride);
        if (rc != c.FF_OK or data == null) return error.ImageResizeFailed;
        return Image.fromOwnedBuffer(@ptrCast(data.?), new_w, new_h);
    }

    /// Resize a sub-rectangle of this image (no intermediate copy).
    pub fn resizeRegion(self: *const Image, x: i32, y: i32, w: i32, h: i32, new_w: i32, new_h: i32) !Image {
        const off = @as(usize, @intCast(y)) * @as(usize, @intCast(self.stride)) +
            @as(usize, @intCast(x)) * 3;
        var data: [*c]u8 = null;
        var stride: i32 = 0;
        const rc = c.ff_image_resize(self.data + off, w, h, self.stride, new_w, new_h, &data, &stride);
        if (rc != c.FF_OK or data == null) return error.ImageResizeFailed;
        return Image.fromOwnedBuffer(@ptrCast(data.?), new_w, new_h);
    }
};

/// Alpha-blend a 128x128 patch (from `Engine.swapPatch`) into `canvas`.
pub fn pastePatch(canvas: *const Image, patch: *const Image, matrix: *const [6]f64, out: *Image) !void {
    const rc = c.ff_paste_patch(canvas.data, canvas.width, canvas.height, canvas.stride, patch.data, matrix, out.data);
    if (rc != c.FF_OK) return error.PasteFailed;
}

/// Blend `original` over `dst` in place using a retouch mask (0 = keep the
/// result, 255 = show the original). The mask may be at a different
/// resolution; it is sampled bilinearly.
pub fn blendMask(dst: *Image, original: *const Image, mask: []const u8, mask_w: i32, mask_h: i32) !void {
    const rc = c.ff_blend_mask(dst.data, original.data, dst.width, dst.height, mask.ptr, mask_w, mask_h);
    if (rc != c.FF_OK) return error.BlendFailed;
}

pub const Memory = struct {
    vram_used_mb: f32 = 0,
    vram_total_mb: f32 = 0,
    rss_mb: f32 = 0,
};

pub fn memoryInfo() Memory {
    var m = Memory{};
    _ = c.ff_memory_info(&m.vram_used_mb, &m.vram_total_mb, &m.rss_mb);
    return m;
}

/// Monotonic nanoseconds from the native core (portable across platforms).
pub fn nowNs() i64 {
    return @intCast(c.ff_now_ns());
}

fn cString(p: [*c]const u8) []const u8 {
    if (p == null) return "";
    return std.mem.span(@as([*:0]const u8, @ptrCast(p)));
}
