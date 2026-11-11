# Grain matching (face re-texturing)

This page explains the optional "Match input grain" control and why it is a
post-process rather than a model. It is off by default because it intentionally
makes the restored face noisier to match a noisy source.

## The problem

The swapper and GFPGAN both output smooth faces. On a grainy, high-ISO or
heavily compressed photo the restored face then looks clean and plastic next to
a noisy body, which gives the paste away. The grain in the original is real
signal that the models removed; we want to put a matching texture back.

## Why it is not part of GFPGAN

`enhance_gfpgan` runs on the *already swapped* image, so the face interior is
the swapper's output, not the pristine original. Extracting grain from that
crop would recover the model's own (smooth) texture, not the source's. The
pristine pixels only exist before the swap, so the reference has to be captured
from the original target. That is why this is a separate pass after enhancement,
called on the composed result with the pristine processing image as reference:

```
swap -> GFPGAN -> [grain match] -> [upscale]
```

## Algorithm

The reference and the result are normally the same size and the face sits at the
same place (the swap does not move it), so this works directly in image space
with no crop round-trip (a 512 crop would smooth the grain away):

1. `residual = reference - GaussianBlur(reference, sigma=1.2)`, per channel, in
   `int16`. This is the zero-mean high-frequency part.
2. For each face, take its bbox expanded by 10% and clip to the frame.
3. Add `strength * clamp(residual, +/-24)` to the result pixels inside the box.
4. Feather the box edges (linear over ~12% of the shorter side) so grain fades
   into the untouched pixels, which already carry their own grain.
5. Clamp to 0..255.

Only the padded face ROI is blurred, not the whole frame: the residual is
needed inside the face boxes and nowhere else. The ROI is grown by the Gaussian
kernel radius (4 px for sigma 1.2) so its interior is identical to a
full-frame blur, then copied into a contiguous buffer so OpenCV takes its
vectorized path. The kernel is reused across faces and frames. The output may
alias the result buffer, so a video frame is updated in place instead of
allocating and copying a second full frame.

The clamp drops strong structural edges (hair strands, glasses frames, JPEG
block edges) so they are not re-stamped from the old face onto the new one. The
per-channel residual keeps chromatic sensor noise chromatic, while monochrome
film grain stays monochrome because its residual is already equal on all three
channels.

Strength is a percentage (0 is a no-op). The UI exposes only an on/off toggle
that maps to 100.

## Where it runs

- Core: `ff_grain_match()` (`core/include/faceforge.h`), backed by
  `grain_match_faces()` in `core/src/models.cpp`. ABI bumped to 6.
- Photo: `App::composeResult()` calls it after GFPGAN and before the optional
  upscale, using `proc` (the pristine processing image) as the reference and the
  active faces in processing coordinates.
- Video: `Video::renderFrame()` calls it for high quality (export) only, using
  the pristine decoded frame as the reference. Low-quality preview frames skip
  it. Video has its own `Match input grain` toggle in *Process & cache*, so the
  photo checkbox never silently changes a clip; the choice is part of the cache
  signature, so toggling it re-renders cached high-quality frames. The pass
  writes into the frame already being assembled, with no extra full-frame
  buffer.

## Cost

Measured on the reference machine (4-core i5-4590, CPU, 6 large faces):

| Frame | Before (full-frame blur + copy) | After (face ROI + in place) |
| ----- | ------------------------------- | --------------------------- |
| 1080p | ~31 ms | ~16 ms |
| 4K    | ~122 ms | ~56 ms |

That is 2-2.5x faster, and it runs once per high-quality frame while the swap
and GFPGAN take seconds per frame on CPU. It is not threaded on purpose:
inference already saturates the four cores, and spreading a 10-50 ms memory
pass over the same cores would add contention rather than reduce wall time.

## Notes and limits

- The residual is spatial, so it also transfers the *look* of the original
  skin pores/compression, which is what we want, but it cannot invent grain that
  the source never had (a clean source yields a near-zero residual).
- Because it runs before the upscaler, an ESRGAN pass afterwards will partly
  smooth the added grain. Running it after the upscale would need a matching
  upscaled reference; not done today.
- It is a quality control and is intentionally not persisted in settings.