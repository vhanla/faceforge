const std = @import("std");

pub fn build(b: *std.Build) !void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const os_tag = target.result.os.tag;
    const is_windows = os_tag == .windows;

    // The C++ core is built by CMake (ONNX Runtime + OpenCV). Wiring it here
    // means `zig build` alone produces a complete, runnable application on
    // Linux and Windows.
    var configure_args: [8][]const u8 = undefined;
    const base_args = [_][]const u8{
        "cmake", "-S", ".", "-B", "build", "-DCMAKE_BUILD_TYPE=Release",
    };
    @memcpy(configure_args[0..base_args.len], &base_args);
    var argc: usize = base_args.len;

    // Use an explicit OpenCV_DIR when provided, otherwise use vcpkg on
    // Windows when it is available (the CI image and most developer machines
    // set VCPKG_ROOT / VCPKG_INSTALLATION_ROOT).
    if (envVar(b, "OpenCV_DIR")) |ocv| {
        configure_args[argc] = try std.fmt.allocPrint(b.allocator, "-DOpenCV_DIR={s}", .{ocv});
        argc += 1;
    } else if (envVar(b, "VCPKG_ROOT") orelse envVar(b, "VCPKG_INSTALLATION_ROOT")) |vcpkg| {
        configure_args[argc] = try std.fmt.allocPrint(
            b.allocator,
            "-DCMAKE_TOOLCHAIN_FILE={s}/scripts/buildsystems/vcpkg.cmake",
            .{vcpkg},
        );
        argc += 1;
    }

    const cmake_configure = b.addSystemCommand(configure_args[0..argc]);
    cmake_configure.setCwd(b.path("."));

    // `--config Release` is ignored by single-config generators (Make/Ninja)
    // and selects the right configuration with Visual Studio.
    const cmake_build = b.addSystemCommand(&.{ "cmake", "--build", "build", "--config", "Release", "-j", "4" });
    cmake_build.setCwd(b.path("."));
    cmake_build.step.dependOn(&cmake_configure.step);

    const dvui_dep = b.dependency("dvui", .{
        .target = target,
        .optimize = optimize,
        .backend = .sdl3,
    });

    // faceforge.h is translated to a Zig module at build time.
    const translate_c = b.addTranslateC(.{
        .root_source_file = b.path("core/include/faceforge.h"),
        .target = target,
        .optimize = optimize,
    });
    translate_c.addIncludePath(b.path("core/include"));

    const mod = b.createModule(.{
        .root_source_file = b.path("gui/src/main.zig"),
        .target = target,
        .optimize = optimize,
    });
    const build_options = b.addOptions();
    build_options.addOption([]const u8, "version", b.option([]const u8, "version", "Override the application version") orelse "0.1.0");
    mod.addOptions("build_options", build_options);
    mod.addImport("dvui", dvui_dep.module("dvui_sdl3"));
    mod.addImport("sdl-backend", dvui_dep.module("sdl3"));
    mod.addImport("ffi", translate_c.createModule());
    mod.addIncludePath(b.path("core/include"));
    mod.addLibraryPath(b.path("build"));
    mod.linkSystemLibrary("faceforge", .{});
    if (!is_windows) {
        mod.addRPathSpecial("$ORIGIN/../lib");
    }
    mod.link_libc = true;

    const exe = b.addExecutable(.{
        .name = "faceforge-gui",
        .root_module = mod,
    });
    exe.step.dependOn(&cmake_build.step);

    const install_exe = b.addInstallArtifact(exe, .{});
    b.getInstallStep().dependOn(&install_exe.step);

    if (is_windows) {
        // CMake copies faceforge.dll, the ONNX Runtime providers and the
        // OpenCV runtime DLL into build/, so installing that directory makes
        // zig-out/bin self-contained.
        const install_dlls = b.addInstallDirectory(.{
            .source_dir = b.path("build"),
            .install_dir = .bin,
            .install_subdir = "",
            .include_extensions = &.{".dll"},
        });
        install_dlls.step.dependOn(&cmake_build.step);
        b.getInstallStep().dependOn(&install_dlls.step);
    } else {
        const core_lib = switch (os_tag) {
            .macos => "build/libfaceforge.dylib",
            else => "build/libfaceforge.so",
        };
        const install_core = b.addInstallFileWithDir(
            b.path(core_lib),
            .lib,
            std.fs.path.basename(core_lib),
        );
        install_core.step.dependOn(&cmake_build.step);
        b.getInstallStep().dependOn(&install_core.step);
    }

    const run_cmd = b.addRunArtifact(exe);
    run_cmd.step.dependOn(b.getInstallStep());
    const run_step = b.step("run", "Run the faceforge GUI");
    run_step.dependOn(&run_cmd.step);

    const core_step = b.step("core", "Build the native core with CMake");
    core_step.dependOn(&cmake_build.step);
}

fn envVar(b: *std.Build, name: []const u8) ?[]const u8 {
    return b.graph.environ_map.get(name);
}
