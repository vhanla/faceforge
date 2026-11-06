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
  app/            FaceForge entry points (currently the spike)
  CMakeLists.txt  builds `amuie`, the spike and the test binary
```

`amuie/main.cpp` is the upstream library's demo entry point (kept for
reference); it is not compiled here. `UPSTREAM_CMakeLists.txt` is the library's
original build file.

## Spike

`app/spike_main.cpp` proves the migration is viable:

1. the vendored UI builds inside FaceForge with CMake,
2. the pure C ABI is called directly from C++ (no bindings layer), and
3. a real image loaded by the core renders on the canvas through the new
   texture command.

It reuses the AMUIE FaceForge demo screen and swaps the canvas empty state for
the image. Video, engine jobs and editing are later phases.

### Build and run

The core must be built first (the root project), then the UI:

```bash
cmake -S . -B build && cmake --build build            # core: build/libfaceforge.so
cmake -S gui-native -B build-native                   # UI spike
cmake --build build-native -j
```

Run from the repository root so the default `assets/...` and `models` paths
resolve:

```bash
./build-native/faceforge-spike
# headless capture (writes a PPM; convert with ImageMagick if needed):
./build-native/faceforge-spike --screenshot /tmp/spike.ppm --frames 3
./build-native/faceforge-spike --image assets/source_messi.jpg
```

`--help` lists `--image`, `--screenshot`, `--frames`, `--size WxH`, `--light`
and `--video`.

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
- `demo/faceforge.*`: optional `DemoState::canvas_tex` drawn contain-fit.

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
- `demo/faceforge.*`: the Output and video path fields are editable.

Keep `amuie/` and upstream in sync by hand when re-vendoring.
