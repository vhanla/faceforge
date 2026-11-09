# CPU optimization notes

This page records the feasibility analysis behind the CPU fast paths in the
core, the measured effect on the development machine and the ideas that are
deliberately left for newer hardware. The packaged build remains generic: hot
loops probe the CPU once at runtime and fall back to scalar code, so binaries
still run on any x86-64 machine.

## Development machine

- Intel Core i5-4590 (Haswell), 4 cores / 4 threads, 3.3 GHz (3.7 GHz turbo).
- 128 KiB L1d and 1 MiB L2 per core, 6 MiB shared L3, dual-channel DDR3.
- ISA: SSE4.2, AVX, AVX2, FMA3, BMI1/BMI2, F16C. **No AVX-512.**
- Reference build: GCC 16, CMake 4, generic flags (`-O3`, no `-march`).

Inference (ONNX Runtime, CUDA) dominates wall time on this machine, so the
targets here are the loops the app itself runs on the UI thread and the CPU
pre/post-processing around inference.

## What changed

### `ff_blend_mask` (Studio retouch)

The same-size mask blend runs every time a brush stroke invalidates the
display. It is a classic read-modify-write over three byte streams (dst, original
and mask), which measured **2.94 ms** for a 1532x1027 frame in the scalar build.
The mask is mostly zero outside the painted area, so the vector path checks
eight mask bytes at once and skips untouched blocks entirely.

- AVX2/FMA kernel: eight pixels per iteration. The eight mask bytes become
  eight alpha floats, three constant permutes expand them to the 24 interleaved
  channels, and FMA blends two vectors per group.
- Runtime dispatch via `__builtin_cpu_supports("avx2")` / `("fma")`, scalar
  fallback otherwise (including MSVC, which is not targeted by the dispatch).
- The non-same-size path (downscaled preview mask) now resamples the mask once
  with OpenCV and reuses the same kernel instead of doing a float bilinear
  sample per destination pixel.

Measured on the reference machine (1532x1027, ~20% of the frame painted):

| path | before | after | speedup |
| --- | ---: | ---: | ---: |
| same-size | 2.94 ms | 0.58 ms | 5.1x |
| scaled mask (4x down) | 30.4 ms | 2.97 ms | 10.2x |

The vector output is bit-identical to the scalar formula. Mask alpha is a
multiple of 1/255, so the blended value can never land close enough to `.5` for
FMA rounding to change the rounded byte; a differential test over random sizes
reports zero mismatches.

Reproduce the numbers without models or ONNX Runtime:

```bash
cmake --build build -j            # builds build/ffblend
build/ffblend 1532 1027 500       # same-size and 4x downscaled mask
```

### `emap` projection (ArcFace -> inswapper latent)

The 512x512 `emap.bin` projection walked the matrix column by column, touching
every other cache line (2 KiB stride) for a 1 MiB working set. Iterating rows
instead and accumulating keep the summation order per output unchanged, so the
result stays bit-identical while the matrix is streamed once.

Measured: **0.172 ms -> 0.089 ms** per face.

### Video decode prefetch

Playback used to decode every frame on the UI thread the moment it was needed:
`ff_video_frame` seeked the `cv::VideoCapture`, read, converted and copied, all
inside the frame the GUI was about to draw. That blocked the UI for the full
decode (4-6 ms at 1080p, ~17-22 ms at 4K on this machine) and made it compete
with inference on the same cores.

Each `ff_video` handle now runs a decode worker with a small ordered window of
`cv::Mat` frames:

- The worker decodes forward from the last requested frame and keeps up to
  96 MiB (clamped to 2..12 frames, so 1080p keeps ~12 and 4K keeps ~4) ready.
  A sequential reader pops frames the worker already finished.
- Backward requests and forward jumps beyond twice the window reset the
  capture instead of decoding every frame in between, which keeps scrubbing
  responsive.
- The worker hands the caller a refcounted shallow copy of the Mat and releases
  the lock, so the single caller-owned `malloc`+`memcpy` no longer stalls new
  decodes.
- OpenCV 5 opens the FFmpeg backend with `CAP_PROP_N_THREADS = min(hw-1, 8)`;
  OpenCV 4 keeps the backend default. Pacing the calls at 30 fps, direct
  decode+copy measured 2.77 ms median per call at 1080p versus 2.30 ms with
  prefetch; at 4K both stay dominated by the ~9.8 ms copy. The win is less the
  raw average than moving the decode (and its seeks) off the UI thread and
  overlapping it with analysis inference, at the cost of at most ~96 MiB per
  decoder.

`ensureFaceThumbs()` in the GUI skips its seek while playing, so the thumbnail
strip cannot pull the playback decoder away from its window.

Reproduce the numbers with a throwaway loop: plain `cv::VideoCapture::read` plus
a `memcpy` per frame versus `ff_video_frame` on the same file, both paced to the
container frame rate with `sleep_for(1000/fps)` between calls and the first
frame dropped as warm-up. The ad-hoc benchmarks used for this note live outside
the repository.

### Loops examined and left alone

- `to_blob_rect` (BGR interleaved -> planar float NCHW) is memory-bound: a
  640x640 frame measured ~1.5 ms scalar and a 4-pixel SSSE3/AVX2 deinterleave
  only reached ~1.4 ms (0-15%, noisy). Not worth the extra code for now.
- GFPGAN/ESRGAN output conversion loops (float planes -> BGR bytes) run once
  per face/tile after inference; the inference call dominates by orders of
  magnitude.
- Per-row `memcpy` paths are already handled by the C library.

## Builds

`FF_NATIVE_BUILD=ON` adds `-march=native -mtune=native` (MSVC: `/arch:AVX2`)
for local benchmarking:

```bash
cmake -S . -B build -DFF_NATIVE_BUILD=ON
```

Packaged builds must stay OFF so they remain portable; the runtime dispatch
already gives them the vector kernels on AVX2-class CPUs.

## TODO for newer hardware

The dispatch pattern makes it cheap to add wider paths later. Keep the generic
build as the baseline and select kernels at runtime.

- **AVX-512 (Skylake-X and later)**: wider blend and blob-conversion kernels,
  and 512-bit emap accumulation. The current i5 has none of it, so it cannot be
  tested here.
- **AVX-512 VNNI (Cascade Lake+) and AVX512-BF16 (Cooper Lake+)**: helpful only
  if the models are quantized to int8/bf16; would need ORT quantized models and
  matching pre/post-processing. Not applicable to the current FP32 ONNX files.
- **AMX (Sapphire Rapids+)**: int8/bf16 tile matmul for the emap projection and
  any future CPU-side convolution, again gated on quantized models.
- **AVX512-FP16 (Sapphire Rapids+)**: half-precision emap/blob paths.
- **AVX2 gather and wider deinterleave**: revisit `to_blob_rect` on a machine
  with more memory bandwidth; the loop is bandwidth-bound, not compute-bound,
  so it only pays off when paired with a fused resize.
- **Hybrid CPUs (Alder Lake+ P/E cores)**: pin ONNX Runtime worker threads to
  P-cores or let ORT 1.31+ thread-affinity options handle it; verify that the
  UI thread is not scheduled on an E-core.
- **Newer ONNX Runtime execution providers**: XNNPACK, ACL/ArmNN, OpenVINO,
  DirectML, QNN, WebGPU. This build ships ORT 1.31 with CUDA; the provider
  selection already goes through `ff_options`, so adding one is a localized
  change in `core/src/ort_session.cpp`.
- **GPU side (separate track)**: CUDA graphs, TensorRT EP, FP16 IO and arena
  reuse. The current i5 machine has a small 4 GB card, so memory strategy
  matters more than instruction selection there.

## GUI-side TODO

- Cache the resampled mask at result resolution instead of resampling it on
  every painted frame (the core now does one `cv::resize`, but it still happens
  per stroke).
- Blend into the existing `display` allocation instead of `result.clone()`
  followed by a full blend; that removes one 4.7 MiB copy per stroke.
- Consider tiling the blend with non-temporal stores if profiling on a newer
  machine shows the write stream is the limit.
