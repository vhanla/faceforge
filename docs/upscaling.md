# Upscaling and enhancer separation

This page is the design note for splitting face restoration from frame
upscaling. It records why the current `Enhancer` combo misleads, the ABI shape
we want and the phased plan. It is deliberately a plan, not a spec of finished
behaviour.

## Why the current ESRGAN options do nothing

`enhance_esrgan()` in `core/src/models.cpp` upscales the whole frame by 2x/4x
and then immediately resizes the result back to the input size:

```cpp
cv::Mat upscaled(h * scale, w * scale, CV_8UC3);   // models.cpp:528
...
cv::resize(upscaled, out, cv::Size(w, h),           // models.cpp:597
           0, 0, cv::INTER_AREA);
```

The model hallucinates detail at 2x/4x and the next line discards it, leaving a
resample round trip that often looks softer than the input. That is why
"None" and "GFPGAN 1.4" are visibly different and the ESRGAN entries are not.

## Why it belongs in a different feature

The combo mixes two axes:

- **GFPGAN** restores faces. It runs on 512x512 aligned crops and blends the
  patch back with a soft mask (`paste_soft`).
- **Real-ESRGAN** is a generic whole-frame upscaler.

They are not alternative settings of the same control; "enhancer: x4" reads
like "colour: 1080p". The tiling machinery already exists and is configurable
through `ff_options.esrgan_tile` (`models.cpp:521`), so the expensive part is
already in place.

## Target pipeline

```
swap faces -> [Restore] -> [Upscale] -> (optional) resize to target size
```

- **Restore** (faces): `None / GFPGAN 1.4` (GFPGAN 1.3 is a possible extra if it
  measures better).
- **Upscale** (frame): `Off / x2 / x4` (`x1` denoise-only is not implemented).
- **Output preset**: `1080p / 1440p / 4K` derived from the input aspect ratio, or
  a free size (only the 16:9 presets are exposed today).
- Rule: when the user asks for an upscale, never resize back to `w, h`. Output
  is either the model's native scale or the requested preset.

### Smart scale choice

For a requested output, pick the smallest model scale `s` that can reach the
target (`h * s >= target_h`), run the model, then resize to the exact target
height with the width from the input aspect ratio:

- 320x180 -> 1080p: `180 * 2 = 360 < 1080`, so `s = 4` (1280x720), then resize
  to 1920x1080.
- 720x405 -> 1440p: `405 * 4 = 1620 >= 1440`, so `s = 4`, then resize to
  2560x1440.

The final resize always goes *down* from the model output, never up, so it does
not invent detail beyond what the model produced. The alternative considered was
a pre-downscale of the input (`d * s >= target / input`) so the model runs
smaller and lands almost exactly on the target; that trades real input detail for
model-invented detail and is unproven, so it is not the default.

### Cost and limits

- One status line, e.g. `1280x720 -> d 0.75 (960x540) -> x2 -> 1920x1080 - 30
  tiles - ~8 GB VRAM`. No panel of sliders.
- Warn above a pixel/VRAM threshold and offer the downscale fast path.
- Surface the real provider (`ff_engine_provider`) so a CPU fallback is not
  silent.
- Restore before upscale: GFPGAN works on aligned crops, so upscaling first
  only makes bigger crops without extra detail.
- Video: x4 per frame is very expensive. Keep upscaling as a render/export
  pass, never in the live preview.

## ABI shape

The core is a pure C ABI (`core/include/faceforge.h`). Phase 1 took the
standalone route: `ff_upscale(engine, in, w, h, stride, scale, out, out_w,
out_h, cb, user)` returns a newly allocated image at the model's native size
(freed with `ff_image_free`). It never resizes back, and it unloads the other
ESRGAN session so alternating x2/x4 stays within the memory budget. This kept
`ff_options` and `ff_enhance` untouched; the ABI version was bumped to 5 because
of the new symbol.

`ff_enhance` still resizes ESRGAN output back to the input (its `out_bgr` is
documented as full-size), so the bench harness keeps working. The GUI no longer
exposes that path.

Models: Real-ESRGAN x2/x4 already download (`scripts/fetch_models.sh:56`);
adding x1 and GFPGAN 1.3 is one line each. All remain non-commercial; check the
licence before shipping.

## Phases

0. **Do not mislead.** Drop x2/x4 from the Enhancer combo. Done.
1. **Standalone image upscaler.** `ff_upscale` (x2/x4, no resize-back) wired
   into the photo pipeline as an optional final stage on the composed result,
   with a dedicated Upscale combo, worker progress and a canvas that uses the
   result's real dimensions (faces and the Studio brush map from base space).
   Done.
2. **Output presets.** A single Upscale combo: `Off / 2x / 4x` plus `1080p /
   1440p / 4K`, shown only for 16:9 inputs (|AR - 16/9| < 0.02) and only when
   the preset would enlarge the input. A preset picks the smallest model scale
   reaching the target and resizes exactly to it. Done.
3. **Cost warnings and fallback.** A single note under the Upscale combo shows
   the predicted output size and megapixels, flags a CPU run as slow
   (`providerName() != "cuda"`) and turns red above ~16 MP (e.g. 4x on a 1080p
   input gives 7680x4320, 33 MP). Done. Video upscaling remains (export pass
   only, never the live preview).

## Video upscaling (plan)

Status: not implemented; design only. The photo path is done, the video path is
the last piece and it has different cost characteristics, so it is kept as a
deliberate export-only feature.

### Where it goes

The video pipeline already renders one frame at a time and caches the high
quality frame on disk (`Video::renderExportFrame`, `video.cpp`), then writes
it through `ff::VideoWriter`. Enhancement (GFPGAN) is applied inside
`Video::renderFrame` for `VideoQuality::High`. An upscale pass belongs *after*
that, i.e. on the finished frame and after any Studio-style edit
(`applyEditToImage`), exactly like the photo path composes first and upscales
last:

```
decode -> swap faces -> [GFPGAN] -> [edit mask] -> [Real-ESRGAN] -> writer
```

`ff_upscale` is reused as-is, so no ABI change. The writer must be opened at the
upscaled dimensions instead of `video.info().width/height`; the existing
`if (frame.width != w || frame.height != h) resize` guard then just becomes a
no-op because frames already match.

### Cache

`renderExportFrame` caches high quality frames at the *source* resolution
(`framePathInto`). Upscaling each frame on export means the existing cache stays
valid and reusable; storing upscaled frames would multiply disk use by
`scale^2` and is not worth it. So: upscale per frame during the export job, do
not add an upscaled cache.

### Options

One video-specific Upscale combo in the export panel, with the same labels as
photo (`Off / 2x / 4x`, plus `1080p / 1440p / 4K` only when the clip is 16:9 and
the preset enlarges it). It is separate from the photo setting so switching
workspaces does not silently change the other. Keep it out of the live preview:
playing a 4x preview is unusable; at most offer a one-frame "preview upscale"
button later.

### Cost

Cost is `frames x per-frame ESRGAN`. Use the same estimate line as photo,
computed from the clip duration: `1920x1080 -> 4K - 1800 frames - CPU (slow)`.
Warn (red) above ~16 MP output or when the run is on CPU. This is a genuine
"walk away" operation; do not hide it behind a spinner.

### Plumbing

- Add `video_upscale_scale` / `video_upscale_target_h` to `App` (or to `Video`),
  and mirror them into the worker handoff like `job_upscale_*_` in the photo
  path.
- Choose the model scale from the target exactly like photo (smallest of x2/x4
  reaching the target height, then resize to the exact size). Reuse the same
  helper rather than duplicating the arithmetic.
- In the export loop (both the chunked and the single-file branches) call
  `engine.upscale(frame_img, scale, &up)` then, for a preset, resize to the
  target. Open the writer with the final width/height.
- Audio muxing and concatenation are unaffected: they run on the finished file.

### Left out on purpose

- No live/video-preview upscaling.
- No per-frame upscale cache.
- No `x1` denoise-only model (not downloaded, unproven).
- No automatic CPU/GPU switch: only the notice, matching photo.
