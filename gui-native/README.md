# gui-native

Native C++ UI for FaceForge, replacing the Zig + DVUI desktop app. The goal of
this branch (`arenaui`) is to drive the unchanged C core
(`core/include/faceforge.h`) from a lightweight, arena-based immediate-mode UI
instead of Zig.

```
gui-native/
  amuie/          vendored AMUIE library (from ../arenaui/src), one local patch:
                  CMD_DRAW_TEXTURE + OpenGL texture registry + ui_image()
  amuie-tools/    asset baker + icon SVGs (regenerate the font atlas)
  amuie-tests/    library unit tests
  app/            FaceForge entry points (currently the spike)
  CMakeLists.txt  builds `amuie` and the spike
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

### Local patch to the vendor

The only functional change over `../arenaui/src` is texture support:

- `core/types.hpp`: `CMD_DRAW_TEXTURE` plus `texture_id`/`uv` on `RenderCommand`.
- `backend/renderer_interface.hpp`: `create_texture_bgr` / `destroy_texture` /
  `draw_texture` (default backends render a flat tint).
- `backend/opengl3_backend.*`: a fixed texture registry, `GL_BGR` upload, and
  shader mode 4. Uploads are a cold path (once per image, not per frame).
- `widgets/ui_widgets.*`: `ui_image()` / `ui_image_uv()`.
- `demo/faceforge.*`: optional `DemoState::canvas_tex` drawn contain-fit.

Keep `amuie/` and upstream in sync by hand when re-vendoring.
