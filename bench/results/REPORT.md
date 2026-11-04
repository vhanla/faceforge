# FaceForge benchmark report

Real measurements taken on the development machine with `build/ffbench` and the
Zig/DVUI application. No numbers in this file are estimates.

- **Date:** 2026-11-02
- **GPU:** NVIDIA GeForce GTX 1650, 4096 MiB, driver 615.78.08, compute 7.5
- **CPU:** Intel Core i5-4590, 4 cores / 4 threads, 3.3 GHz
- **RAM:** 11 GiB
- **CUDA:** 13.4 · **cuDNN:** 9.27 · **ONNX Runtime:** 1.31.1 (`gpu_cuda13`)
- **OpenCV:** 5.0.0 · **CMake:** 4.4.5 · **Zig:** 0.17.0
- **Input:** `assets/target_t1.jpg` (1280×886, 6 faces) and
  `assets/source_lena.jpg` (512×512, one identity), detection size 640.

## Methodology

`ffbench` runs each stage after unloading all ONNX Runtime sessions:

- **cold** = first run including session creation and CUDA graph/arena setup.
- **warm** = repetitions after the first (`--warmup 2`, `--repeats 5` by
  default). `median` and `p95` describe the warm samples.
- **VRAM** is read from NVML and is **whole-device usage**, so it includes the
  desktop and every other GPU application. The `base → peak` columns show the
  usage immediately before and at the peak of the measured stage.
- **RSS** is the process resident set size at the end of the stage.
- `enhance` uses a 512×512 crop of the first detected face; `pipeline` runs
  the complete swap + enhance chain over all 6 detected faces at full
  resolution.

```bash
scripts/run_bench.sh                       # CUDA + CPU matrix
# equivalent individual runs
build/ffbench --provider cuda --stage full --enhancer gfpgan --repeats 5
build/ffbench --provider cuda --stage full --enhancer gfpgan --repeats 5 --sequential
build/ffbench --provider cpu  --stage enhance --enhancer esrgan4 --repeats 1 --warmup 0
```

The CPU Real-ESRGAN x4 and full-pipeline rows use a single sample because one
pass takes minutes; they are labelled accordingly in the table (`Seq`,
`Faces`) and should be treated as order-of-magnitude figures.

## Results

| Stage | Provider | Enhancer | Faces | Seq | Cold (ms) | Median (ms) | p95 (ms) | VRAM base → peak (MiB) | RSS (MiB) |
|---|---|---|---:|:---:|---:|---:|---:|---:|---:|
| detect | CUDA | — | 6 | no | 199.3 | 27.2 | 27.8 | 1435 → 1543 | 1169 |
| embed | CUDA | — | 1 | no | 344.7 | 14.0 | 16.8 | 1344 → 1721 | 1269 |
| swap (1 face) | CUDA | — | 1 | no | 1872.1 | 148.3 | 149.5 | 1450 → 2209 | 1281 |
| enhance* | CUDA | GFPGAN 1.4 | 1 | no | 1622.7 | 176.7 | 177.8 | 1419 → 2808 | 1292 |
| enhance* | CUDA | Real-ESRGAN x2 | 1 | no | 3933.1 | 2047.2 | 2055.2 | 1369 → 1844 | 1422 |
| enhance* | CUDA | Real-ESRGAN x4 | 1 | no | 9016.5 | 8023.7 | 8023.8 | 1369 → 3101 | 1477 |
| **pipeline** | CUDA | GFPGAN 1.4 | 6 | no | 5482.1 | 2172.3 | 2250.8 | 1415 → **4041** | 1454 |
| **pipeline** | CUDA | GFPGAN 1.4 | 6 | yes | 5600.8 | 4701.1 | 4976.4 | 1352 → **3516** | 993 |
| detect | CPU | — | 6 | no | 218.2 | 155.5 | 274.2 | 1231 → 1308 | 305 |
| embed | CPU | — | 1 | no | 376.4 | 137.9 | 140.6 | 1308 → 1308 | 372 |
| swap (1 face) | CPU | — | 1 | no | 2691.8 | 1576.4 | 1701.6 | 1260 → 1284 | 1013 |
| enhance* | CPU | GFPGAN 1.4 | 1 | no | 3104.8 | 3019.7 | 3077.6 | 1231 → 1231 | 1395 |
| enhance* | CPU | Real-ESRGAN x2 | 1 | no | 38096.7 | 39980.6 | 40358.0 | 1231 → 1330 | 659 |
| enhance* | CPU | Real-ESRGAN x4 | 1 | no | 201894.0 | 269883.0 | 269883.0 | *noise* | 776 |
| **pipeline** | CPU | GFPGAN 1.4 | 6 | no | 34534.7 | 28269.2 | 28269.2 | *noise* | 2494 |

`*` = 512×512 face crop. CPU rows do not touch the GPU, so their VRAM columns
reflect other processes and are not meaningful.

## What the numbers mean

**GPU stage costs.** On the GTX 1650 a single face costs about 27 ms to detect,
14 ms to embed, 148 ms to swap and 177 ms to restore with GFPGAN. The full
6-face GFPGAN pipeline warms up to **2.17 s** and the 95th percentile stays
within 2.25 s, so the result is predictable frame to frame. A cold process
(engine creation + first inference) takes **5.5 s**, which is dominated by
loading the 340 MB GFPGAN graph and warming the CUDA allocator.

**The relighting is cheap; the enhancer is not.** Swapping all 6 faces takes
roughly 0.9 s, while GFPGAN restoration of those 6 crops costs another ~1.1 s.
Real-ESRGAN x2 and x4 are 11× and 45× slower than GFPGAN for a 512×512 crop,
and x4 also uses more VRAM (3101 MiB peak vs 2808 MiB). For interactive use on
a 4 GB card, GFPGAN is the right default.

**CPU fallback is correct but not interactive.** Detection and embedding stay
under 160 ms, but a single swap is 1.58 s and a single GFPGAN crop is 3.0 s.
The 6-face full pipeline takes **28.3 s**, and Real-ESRGAN x4 takes **4.5
min** for one 512×512 crop. More CPU threads would not change the order of
magnitude; the CPU path is meant for machines without a usable CUDA device and
for parity testing.

## VRAM analysis for a 4 GB card

Peak deltas observed per session:

| Session | Peak VRAM delta |
|---|---:|
| SCRFD `det_10g` | ~110 MB |
| ArcFace `w600k_r50` | ~380 MB |
| `inswapper_128` | ~760 MB |
| GFPGAN 1.4 | ~1.39 GB |
| Real-ESRGAN x2 | ~475 MB |
| Real-ESRGAN x4 | ~1.73 GB |

Keeping every session resident peaks at **4041 MiB used** on this machine,
with ~1415 MiB already consumed by the desktop. That is only 55 MiB below the
card's limit, and the GUI autorun screenshot shows **4028/4096 MiB** with a
browser open. It works, but there is no headroom for a second GPU application.

With `sequential` enabled, sessions are closed between stages and the peak
drops to **3516 MiB** (−525 MiB). The price is session reloading: the warm
6-face pipeline grows from 2.17 s to 4.70 s, because inswapper and GFPGAN are
rebuilt on every run. The `free` headroom is worth it on a 4 GB card when other
applications are open; the resident mode is faster when the GPU is dedicated.

## Numerical parity with InsightFace

`scripts/verify_parity.py` compares the native preprocessing, detection,
alignment, embedding and swap against the InsightFace Python reference on the
CPU execution provider:

- detections: **6/6**
- bbox and 5-point landmark error: **0.0000 px**
- ArcFace embedding cosine similarity: **0.9999277**
- swap patch mean absolute difference: **0.0113 / 255**, p99 = 1, max = 3

The residual difference comes from ONNX Runtime's CPU kernels versus
InsightFace's own numpy/OpenCV steps, not from a different algorithm.

## Reproducing

```bash
scripts/fetch_deps.sh       # ONNX Runtime 1.31.1 (245 MB)
scripts/fetch_models.sh     # weights (~1.2 GB)
scripts/build.sh            # core + GUI
scripts/run_bench.sh        # writes bench/results/bench.csv + latest.md
```

Hardware-dependent results: use `bench/results/bench.csv` from your own run
rather than comparing absolute milliseconds across GPUs. The CSV is stable and
machine-readable; `scripts/bench_to_md.py` renders it as Markdown.