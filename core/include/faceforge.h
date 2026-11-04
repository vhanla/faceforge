/*
 * faceforge - native face restoration / face swap pipeline
 *
 * Pure C ABI. The implementation is C++17 (ONNX Runtime + OpenCV) and can be
 * consumed from C, C++, Zig, Rust, Python (ctypes), etc.
 *
 * Image format: 8-bit BGR, interleaved, row-major. `stride` is in bytes.
 * All functions are synchronous and an engine serializes calls internally.
 */
#ifndef FACEFORGE_H
#define FACEFORGE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FF_ABI_VERSION 4

/* Return codes */
#define FF_OK 0
#define FF_ERR_INVALID -1
#define FF_ERR_IO -2
#define FF_ERR_MODEL -3
#define FF_ERR_RUNTIME -4
#define FF_ERR_UNSUPPORTED -5
#define FF_ERR_NOMEM -6

/* Execution providers */
typedef enum ff_provider {
    FF_PROVIDER_AUTO = 0, /* CUDA when available, otherwise CPU */
    FF_PROVIDER_CUDA = 1,
    FF_PROVIDER_CPU = 2
} ff_provider;

/* Pipeline stages reported through ff_progress_fn */
typedef enum ff_stage {
    FF_STAGE_DETECT = 0,
    FF_STAGE_EMBED = 1,
    FF_STAGE_SWAP = 2,
    FF_STAGE_ENHANCE = 3,
    FF_STAGE_DONE = 4
} ff_stage;

/* Enhancer selection */
typedef enum ff_enhancer {
    FF_ENHANCE_NONE = 0,
    FF_ENHANCE_GFPGAN = 1,
    FF_ENHANCE_ESRGAN_X2 = 2,
    FF_ENHANCE_ESRGAN_X4 = 3
} ff_enhancer;

typedef struct ff_options {
    int32_t provider;      /* ff_provider; default FF_PROVIDER_AUTO */
    int32_t device_id;     /* CUDA device; default 0 */
    int32_t intra_threads; /* CPU threads; 0 = ORT default */
    int32_t sequential;    /* 1 = close sessions between stages to bound VRAM */
    int32_t det_size;      /* SCRFD input side; 0 = 640 */
    float det_thresh;      /* 0 = 0.5 */
    float nms_thresh;      /* 0 = 0.4 */
    int32_t max_faces;     /* 0 = 10 */
    int32_t enhance;       /* ff_enhancer; default FF_ENHANCE_NONE */
    int32_t esrgan_tile;   /* 0 = 256 */
    int64_t gpu_mem_limit; /* bytes; 0 = runtime default */
    int32_t restore_mouth; /* 1 = blend the original mouth region back into swaps */
    int32_t pad;           /* reserved, keep zero for future growth */
} ff_options;

typedef struct ff_face {
    float bbox[4];   /* x1, y1, x2, y2 in source-image pixels */
    float kps[10];   /* five (x, y) landmark pairs */
    float det_score;
    int32_t face_id; /* stable index for the current detection call */
} ff_face;

typedef void (*ff_progress_fn)(int32_t stage, float progress,
                               const char* message, void* user_data);

typedef struct ff_stats {
    double detect_ms;
    double embed_ms;
    double swap_ms;
    double enhance_ms;
    double enhance2_ms; /* per-face enhancement pass */
    double total_ms;
    int32_t faces_detected;
    int32_t faces_swapped;
    int32_t provider; /* actual ff_provider used */
    int32_t sessions_loaded;
    double vram_peak_mb;
} ff_stats;

typedef struct ff_engine ff_engine;

/* Fill `opt` with defaults. */
void ff_options_default(ff_options* opt);

/* Create an engine. `model_dir` must contain the model files described in
 * README.md. On failure returns NULL and writes a message into `err`. */
ff_engine* ff_engine_create(const char* model_dir, const ff_options* opt,
                            char* err, int32_t err_len);
void ff_engine_destroy(ff_engine* engine);

/* Last error for this engine (never NULL). */
const char* ff_engine_last_error(const ff_engine* engine);

/* Actual provider in use ("cuda" or "cpu"). */
const char* ff_engine_provider(const ff_engine* engine);

/* Close all lazily-created sessions. They are recreated on demand. */
int32_t ff_engine_unload(ff_engine* engine);

/* Change the enhancer for subsequent ff_enhance/ff_process calls. Sessions of
 * the previous enhancer are released so the 4 GB budget is respected. */
int32_t ff_engine_set_enhancer(ff_engine* engine, int32_t enhancer);

/* Toggle mouth restoration for swaps (0/1) at runtime. */
int32_t ff_engine_set_restore_mouth(ff_engine* engine, int32_t on);

/* Number of models currently resident. */
int32_t ff_engine_sessions_loaded(const ff_engine* engine);

/* Run SCRFD on a BGR image. Writes at most `max_faces` into `out_faces`.
 * `out_count` receives the total written. Faces are sorted by score. */
int32_t ff_detect(ff_engine* engine,
                  const uint8_t* bgr, int32_t width, int32_t height, int32_t stride,
                  ff_face* out_faces, int32_t max_faces, int32_t* out_count);

/* Align a face (5 landmarks) to 112x112, run ArcFace and write a normalized
 * 512-float embedding. */
int32_t ff_embed(ff_engine* engine,
                 const uint8_t* bgr, int32_t width, int32_t height, int32_t stride,
                 const ff_face* face, float* out_embedding);

/* Swap one face. `source_embedding` is a normalized 512-float ArcFace
 * embedding. `out_bgr` must be at least height*stride bytes; the whole target
 * image is copied and, when `paste_back` is non-zero, the face is blended in. */
int32_t ff_swap_face(ff_engine* engine,
                     const uint8_t* target_bgr, int32_t width, int32_t height, int32_t stride,
                     const float* source_embedding, const ff_face* face,
                     int32_t paste_back,
                     uint8_t* out_bgr);

/* Low-level swap used by interactive editors. Runs inswapper for a single face
 * and returns the raw 128x128 tightly-packed BGR patch plus the 2x3 affine
 * matrix (double[6], row-major) that maps source pixels into the aligned patch.
 * The caller owns *out_patch and releases it with ff_image_free.
 *
 * ff_paste_patch then alpha-blends that patch into a full-size canvas exactly
 * like the paste-back step of ff_swap_face. Keeping patches around lets callers
 * recompose a scene without re-running the model. */
int32_t ff_swap_patch(ff_engine* engine,
                      const uint8_t* bgr, int32_t width, int32_t height, int32_t stride,
                      const float* source_embedding, const ff_face* face,
                      uint8_t** out_patch, double* out_matrix);

int32_t ff_paste_patch(const uint8_t* canvas_bgr, int32_t width, int32_t height,
                       int32_t stride, const uint8_t* patch, const double* matrix,
                       uint8_t* out_bgr);

/* Enhance the faces in `faces` (may be NULL to detect again) using the
 * enhancer configured in ff_options. `out_bgr` is a full-size BGR image. */
int32_t ff_enhance(ff_engine* engine,
                   const uint8_t* bgr, int32_t width, int32_t height, int32_t stride,
                   const ff_face* faces, int32_t face_count,
                   uint8_t* out_bgr,
                   ff_progress_fn cb, void* user_data);

/* End-to-end: detect the best source face, swap every target face with it,
 * then optionally enhance. Output has the target dimensions. */
int32_t ff_process(ff_engine* engine,
                   const uint8_t* target_bgr, int32_t target_w, int32_t target_h, int32_t target_stride,
                   const uint8_t* source_bgr, int32_t source_w, int32_t source_h, int32_t source_stride,
                   uint8_t* out_bgr,
                   ff_progress_fn cb, void* user_data);

/* Copy the stats of the last operation. */
int32_t ff_last_stats(const ff_engine* engine, ff_stats* out_stats);

/* Image I/O helpers (BGR, tightly packed: stride == width * 3).
 * ff_image_load allocates the buffer; release it with ff_image_free. */
int32_t ff_image_load(const char* path, uint8_t** out_bgr, int32_t* out_width,
                      int32_t* out_height, int32_t* out_stride);
void ff_image_free(uint8_t* data);
int32_t ff_image_save(const char* path, const uint8_t* bgr, int32_t width,
                      int32_t height, int32_t stride);

/* In-memory image helpers (useful for clipboard and drag-and-drop). All
 * allocated buffers are released with ff_image_free.
 *
 * ff_image_decode turns an encoded PNG/JPEG/WEBP/... buffer into BGR pixels.
 * ff_image_encode_png turns BGR pixels into a PNG buffer.
 * ff_image_resize produces a bilinear resample with stride == new_w * 3. */
int32_t ff_image_decode(const uint8_t* data, int32_t size, uint8_t** out_bgr,
                        int32_t* out_width, int32_t* out_height, int32_t* out_stride);
int32_t ff_image_encode_png(const uint8_t* bgr, int32_t width, int32_t height,
                            int32_t stride, uint8_t** out_data, int32_t* out_size);
int32_t ff_image_resize(const uint8_t* bgr, int32_t width, int32_t height,
                        int32_t stride, int32_t new_width, int32_t new_height,
                        uint8_t** out_bgr, int32_t* out_stride);

/* Copy an image into a new tightly-packed buffer. */
int32_t ff_image_clone(const uint8_t* bgr, int32_t width, int32_t height,
                       int32_t stride, uint8_t** out_bgr, int32_t* out_stride);

/* Retouch mask compositing (used by the editor's Studio/retouch layer).
 *
 * A mask holds one byte per pixel: 0 keeps `dst_bgr` untouched, 255 replaces
 * it with `original_bgr`. `dst_bgr` is modified in place and both images must
 * be tightly packed (stride == width * 3). The mask may have a different
 * resolution than the images (e.g. a full-resolution mask applied to a
 * downscaled preview); it is sampled bilinearly. */
int32_t ff_blend_mask(uint8_t* dst_bgr, const uint8_t* original_bgr,
                      int32_t width, int32_t height,
                      const uint8_t* mask, int32_t mask_w, int32_t mask_h);

/* -------------------------------------------------------------------------
 * Video decoding (on-demand frames, no extraction to disk)
 *
 * The handle keeps the container open and remembers the decoder position, so
 * sequential playback/analysis is a pure decode. Random seeks are honoured by
 * seeking the container; callers that need maximum throughput should read
 * frames in increasing order.
 * ------------------------------------------------------------------------- */

typedef struct ff_video ff_video;

/* Open a video file. Returns NULL and writes a message into `err` on failure.
 * The returned handle is not thread-safe; open one handle per thread. */
ff_video* ff_video_open(const char* path, char* err, int32_t err_len);
void ff_video_close(ff_video* video);

/* Container metadata. Any output pointer may be NULL. `fps` is the average
 * frame rate, `frames` the reported frame count (0 if unknown) and `duration`
 * the duration in seconds. */
int32_t ff_video_info(const ff_video* video, int32_t* out_width,
                      int32_t* out_height, double* out_fps, int64_t* out_frames,
                      double* out_duration);

/* Decode frame `index` (0-based) into a newly allocated tightly packed BGR
 * buffer (stride == width * 3), released with ff_image_free. */
int32_t ff_video_frame(ff_video* video, int64_t index, uint8_t** out_bgr,
                       int32_t* out_width, int32_t* out_height,
                       int32_t* out_stride);

/* -------------------------------------------------------------------------
 * Video encoding (ffmpeg pipe)
 *
 * The GUI renders frames one by one; the core streams raw BGR frames into an
 * ffmpeg process, so no temporary image sequence is ever written to disk.
 * `encoder` is an ffmpeg video encoder name (e.g. "libx264"); `preset` may be
 * NULL/empty for the encoder default, and `crf` 0 selects the default (18).
 * ------------------------------------------------------------------------- */

typedef struct ff_video_writer ff_video_writer;

ff_video_writer* ff_video_writer_open(const char* path, int32_t width,
                                      int32_t height, double fps,
                                      const char* encoder, int32_t crf,
                                      const char* preset, const char* ffmpeg,
                                      char* err, int32_t err_len);
int32_t ff_video_writer_frame(ff_video_writer* writer, const uint8_t* bgr,
                              int32_t width, int32_t height, int32_t stride);
/* Closes the pipe and returns the ffmpeg exit status as FF_OK / FF_ERR_IO. */
int32_t ff_video_writer_close(ff_video_writer* writer, char* err,
                              int32_t err_len);

/* Concatenate same-codec parts listed in `list_path` (ffmpeg concat demuxer)
 * without re-encoding. */
int32_t ff_video_concat(const char* list_path, const char* out_path,
                        const char* ffmpeg, char* err, int32_t err_len);

/* Mux the audio track of `source_path` (trimmed to [start_sec, +duration]) into
 * `video_path` and write `out_path`. The video stream is copied bit-exactly;
 * audio is encoded to AAC. */
int32_t ff_video_mux_audio(const char* video_path, const char* source_path,
                           double start_sec, double duration,
                           const char* out_path, const char* ffmpeg,
                           char* err, int32_t err_len);

/* Returns FF_OK when `ffmpeg` can be executed; writes its first version line
 * into `out` when provided. */
int32_t ff_ffmpeg_probe(const char* ffmpeg, char* out, int32_t out_len);

/* Small cross-platform filesystem helpers used by the video workspace.
 * ff_file_read allocates with malloc and is released with ff_image_free. */
int32_t ff_fs_mkdirs(const char* path);
int32_t ff_file_read(const char* path, uint8_t** out_data, int32_t* out_size);
int32_t ff_file_write(const char* path, const uint8_t* data, int32_t size);
int32_t ff_file_exists(const char* path);
int32_t ff_file_remove(const char* path);
int32_t ff_file_copy(const char* src, const char* dst);

/* Process/GPU memory in MiB. Any pointer may be NULL. *total_mb is 0 when
 * NVML is unavailable. */
int32_t ff_memory_info(float* vram_used_mb, float* vram_total_mb, float* rss_mb);

/* Portable monotonic clock in nanoseconds. Intended for GUI/benchmark timing;
 * the epoch is unspecified. */
int64_t ff_now_ns(void);

#ifdef __cplusplus
}
#endif
#endif /* FACEFORGE_H */
