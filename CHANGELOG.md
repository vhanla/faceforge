# Changelog

All notable changes to FaceForge are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).
Released versions match the git tags (`vX.Y.Z`).

## [Unreleased]

## [0.1.0] - 2026-11-04

### Added

- Photo editor: SCRFD detection, ArcFace identity embeddings and
  `inswapper_128` face swap with per-face identity, mouth and swap controls.
- Identity library, per-face hot updates from cached 128x128 patches, two-level
  patch cache and fast preview.
- Enhancers: GFPGAN 1.4 and Real-ESRGAN x2/x4, on top of a CPU/CUDA ONNX
  Runtime engine with a 4 GB-oriented memory profile.
- Studio retouch layer for both the still editor and the video workspace.
- Video workspace: chunked analysis, sequences, low/high-quality caching and
  ffmpeg export with audio mux and split chunks.
- Zig + DVUI desktop UI behind a stable C ABI (`core/include/faceforge.h`).
- Linux and Windows CPU builds in CI, plus release packaging.

[Unreleased]: https://github.com/vhanla/faceforge/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/vhanla/faceforge/releases/tag/v0.1.0
