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
- **Upscale** (frame): `Off / x1 / x2 / x4`. `x1` denoises and sharpens without
  changing size.
- **Output preset**: `1080p / 1440p / 4K` derived from the input aspect ratio,
  or a free size.
- Rule: when the user asks for an upscale, never resize back to `w, h`. Output
  is either the model's native scale or the requested preset.

### Smart scale choice

For a requested output, pick a downscale `d` and a model scale `s` so that
`d * s >= target / input`, with the least overshoot, then resize exactly to the
target:

- 1280x720 -> 1080p (ratio 1.5), `x2`: `d = 1920 / (2 * 1280) = 0.75`, so the
  model runs at 960x540 and 2x lands exactly on 1920x1080.
- 1280x720 -> 1080p, `x4`: `d = 0.375`, model runs at 480x270.

A pre-downscale of the input trades real detail for model-invented detail, so
the heuristic must be measured against the plain path (upscale the input and
resize the final result) before it becomes the default.

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

The core is a pure C ABI (`core/include/faceforge.h`). `ff_options` already has
`enhance` and `esrgan_tile`, plus a reserved `pad`. Options:

- Keep `enhance` = restoration, add `upscale` (enum `NONE/X1/X2/X4`) and
  `out_width/out_height` (0 = input), or
- expose a standalone `ff_upscale(engine, in, scale, out)` and let the GUI do
  the final resize to a preset.

Either way the ABI changes, so `FF_ABI_VERSION` must be bumped.

Models: Real-ESRGAN x2/x4 already download (`scripts/fetch_models.sh:56`);
adding x1 and GFPGAN 1.3 is one line each. All remain non-commercial; check the
licence before shipping.

## Phases

0. **Do not mislead.** Drop x2/x4 from the Enhancer combo (done) so the broken
   path is unreachable from the UI.
1. **Standalone image upscaler.** Real x2/x4 with no resize-back, its own
   control, run through a worker job with progress.
2. **Output presets.** Aspect-ratio-aware target sizes and the smart downscale
   choice.
3. **Cost warnings and fallback.** VRAM/pixel estimate, CPU fallback notice,
   then video.
