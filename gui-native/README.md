# gui-native

Native C++ UI for FaceForge, replacing the Zig + DVUI desktop app. The goal of
this branch (`arenaui`) is to drive the unchanged C core
(`core/include/faceforge.h`) from a lightweight, arena-based immediate-mode UI
instead of Zig.

```
gui-native/
  amuie/          vendored AMUIE library (from ../arenaui/src) plus local
                  additions: texture support (CMD_DRAW_TEXTURE + OpenGL
                  registry + ui_image) and editable widgets (ui_text_edit,
                  ui_slider) with a platform input bridge
  amuie-tools/    asset baker + icon SVGs (regenerate the font atlas)
  amuie-tests/    library unit tests (headless; run with `amuie-tests`)
  app/            FaceForge itself: the UI-agnostic app layer (pipeline,
                  worker thread, settings, C ABI wrappers) and the AMUIE
                  screen that drives it, plus the entry point
  CMakeLists.txt  builds `amuie`, `faceforge-app`, `faceforge-ui`,
                  `faceforge-spike` and the test binary
```

`amuie/main.cpp` is the upstream library's demo entry point (kept for
reference); it is not compiled here. `UPSTREAM_CMakeLists.txt` is the library's
original build file.

The app layer is deliberately UI-agnostic and mirrors `gui/src/*.zig`: it owns
the engine, the pristine base image, the identity library, the per-face swap
patch cache, the composed result and the undo/redo history, and runs all
inference on one background thread. The video workspace lives in `app::Video`
(`app/video.*`): it owns the timeline, analysis data, frame caches, project
file and export jobs, borrowing the engine and identity library from
`app::App`; photo and video jobs never run at the same time. The FaceForge
screen (`app/ui_faceforge.*`) lives outside the vendored tree so it can call the
app layer directly and `amuie/` stays pristine.

## Entry point

`app/spike_main.cpp` builds `app::App`, `app::Video` and the screen, then runs
the frame loop. Beyond rendering, it owns texture lifetime: it uploads and
destroys the base, result, face-thumbnail, identity-thumbnail and video-frame
textures as the app's dirty flags and generations change. It also feeds the
video shortcuts (Space, Left/Right, Home/End, I/O) and drag-and-drop.

### Build and run

The core must be built first (the root project), then the UI:

```bash
cmake -S . -B build && cmake --build build            # core: build/libfaceforge.so
cmake -S gui-native -B build-native                   # native UI
cmake --build build-native -j
```

Run from the repository root so the default `assets/...` and `models` paths
resolve:

```bash
./build-native/faceforge-spike
# headless capture (writes a PPM; convert with ImageMagick if needed):
./build-native/faceforge-spike --screenshot /tmp/spike.ppm --frames 5 --wait
./build-native/faceforge-spike --identity assets/source_messi.jpg
# video workspace, optionally opening a clip:
./build-native/faceforge-spike --video [PATH]
```

`--help` lists the full flag set: `--target`, `--identity` (repeatable or a
`;`-separated list), `--models`, `--temp-dir`, `--export-dir`, `--provider`,
`--config`, `--no-config`, `--save`, `--screenshot`, `--frames`, `--wait`,
`--size WxH`, `--light` and `--video [PATH]`. `FACEFORGE_TARGET`,
`FACEFORGE_IDENTITY` and `FACEFORGE_VIDEO` provide the same inputs for scripted
runs; `FACEFORGE_VIDEOTEST=clip.mp4` runs the scripted analysis/process/export/
playback check (`FACEFORGE_VIDEOTEST_KEEP=1` keeps the window up afterwards).
Without `--identity` or `FACEFORGE_IDENTITY`, `assets/source_lena.jpg` is the
default identity, matching the Zig GUI.

Note that `ui_text_edit` edits an app-owned buffer in place, so a widget pass
must run each frame for the field to keep its focus, caret and selection. This
is why the `UIContext` is created once outside the loop in the entry points.

### Local changes to the vendor

Over `../arenaui/src` this branch adds two features:

Texture support:

- `core/types.hpp`: `CMD_DRAW_TEXTURE` plus `texture_id`/`uv` on `RenderCommand`.
- `backend/renderer_interface.hpp`: `create_texture_bgr` / `destroy_texture` /
  `draw_texture` (default backends render a flat tint).
- `backend/opengl3_backend.*`: a fixed texture registry, `GL_BGR` upload, and
  shader mode 4. Uploads are a cold path (once per image, not per frame).
- `widgets/ui_widgets.*`: `ui_image()` / `ui_image_uv()`.
- `app/ui_faceforge.*`: draws the result/base texture contain-fit.

Editable text, slider and input plumbing:

- `core/input.hpp`: `UIInput` (keyboard/text/clipboard/drag-drop) and the
  persistent per-field `UITextEditState` table; `UIContext` gains `input`.
- `platform/window.*`: GLFW key/char/drop callbacks and queues, `key_raw_down`,
  and clipboard get/set.
- `platform/input.*`: `platform::feed_ui_input(window, &ui)` drains the queues
  into `ui.input`, resetting the per-frame fields; call it after
  `poll_events()`.
- `widgets/ui_widgets.*`: `ui_text_edit()` (UTF-8, selection, Ctrl+A/C/V/X,
  caret blink, horizontal scroll, per-id focus) and `ui_slider()`.
- `app/ui_faceforge.*`: the Output field and the Settings model directory are
  editable.

Keep `amuie/` and upstream in sync by hand when re-vendoring.
