cmake_minimum_required(VERSION 3.20)
project(faceforge_native_ui CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Native UI spike: the vendored AMUIE library plus a FaceForge app that talks to
# the core over the pure C ABI (core/include/faceforge.h). The core itself is
# built by the repository's root CMake project; this one only builds the UI.
find_package(glfw3 3.3 REQUIRED)

set(AMUIE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/amuie")
set(FACEFORGE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

# ---- AMUIE: vendored engine + widgets + OpenGL backend ---------------------
add_library(amuie STATIC
    "${AMUIE_DIR}/core/font.cpp"
    "${AMUIE_DIR}/widgets/ui_widgets.cpp"
    "${AMUIE_DIR}/platform/window.cpp"
    "${AMUIE_DIR}/platform/input.cpp"
    "${AMUIE_DIR}/backend/opengl3_backend.cpp"
    "${AMUIE_DIR}/assets/font_atlas.cpp"
)
target_include_directories(amuie PUBLIC "${AMUIE_DIR}")
target_link_libraries(amuie PUBLIC glfw)

if(WIN32)
    target_link_libraries(amuie PUBLIC Opengl32 dwmapi)
elseif(APPLE)
    target_link_libraries(amuie PUBLIC "-framework OpenGL")
else()
    target_link_libraries(amuie PUBLIC GL)
    # X11 is used for the KDE/KWin blur-behind hint (see platform/window.cpp).
    find_package(X11 QUIET)
    if(X11_FOUND)
        target_link_libraries(amuie PUBLIC X11::X11)
        target_compile_definitions(amuie PUBLIC AMUIE_HAVE_X11=1)
    endif()
endif()

if(NOT MSVC)
    target_compile_options(amuie PRIVATE -Wall -Wextra)
endif()

# ---- Headless widget tests (no window / GL context needed) -----------------
add_executable(amuie-tests "${CMAKE_CURRENT_SOURCE_DIR}/amuie-tests/test_suite.cpp")
target_link_libraries(amuie-tests PRIVATE amuie)
# Release builds define NDEBUG, which would silently strip the assert-based
# checks in the suite; keep them active for this target.
if(MSVC)
    target_compile_options(amuie-tests PRIVATE /UNDEBUG)
else()
    target_compile_options(amuie-tests PRIVATE -Wall -Wextra -UNDEBUG)
endif()
enable_testing()
add_test(NAME amuie-tests COMMAND amuie-tests)

# ---- FaceForge core (C ABI) ------------------------------------------------
# Build the root project first (`cmake --build build`); this target only links.
set(FACEFORGE_LIB "${FACEFORGE_ROOT}/build/libfaceforge${CMAKE_SHARED_LIBRARY_SUFFIX}")
if(NOT EXISTS "${FACEFORGE_LIB}")
    message(FATAL_ERROR
        "libfaceforge not found at ${FACEFORGE_LIB}. Build the core first: "
        "cmake -S . -B build && cmake --build build")
endif()

# ---- App layer (UI-agnostic state, worker, settings, C ABI wrappers) --------
find_package(Threads REQUIRED)
add_library(faceforge-app STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/app/ff.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/app/config.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/app/app.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/app/video.cpp"
)
target_include_directories(faceforge-app PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/app"
    "${FACEFORGE_ROOT}/core/include"
)
target_link_libraries(faceforge-app PUBLIC "${FACEFORGE_LIB}" Threads::Threads)
if(NOT MSVC)
    target_compile_options(faceforge-app PRIVATE -Wall -Wextra)
endif()

# ---- FaceForge screen (AMUIE widgets bound to the app layer) ----------------
# The FaceForge screen is app-specific, so it lives outside the vendored library
# and calls app::App directly. It is a separate static library so the entry
# point only links the screen, not the whole app internals.
add_library(faceforge-ui STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/app/ui_faceforge.cpp"
)
target_include_directories(faceforge-ui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/app")
target_link_libraries(faceforge-ui PUBLIC amuie faceforge-app)
if(NOT MSVC)
    target_compile_options(faceforge-ui PRIVATE -Wall -Wextra)
endif()

# ---- Spike executable ------------------------------------------------------
add_executable(faceforge-spike "${CMAKE_CURRENT_SOURCE_DIR}/app/spike_main.cpp")
target_include_directories(faceforge-spike PRIVATE "${FACEFORGE_ROOT}/core/include")
target_link_libraries(faceforge-spike PRIVATE faceforge-ui "${FACEFORGE_LIB}")

# Resolve the core and ONNX Runtime from the build tree.
set(_ff_rpath "${FACEFORGE_ROOT}/build;${FACEFORGE_ROOT}/third_party/onnxruntime/lib")
set_target_properties(faceforge-spike PROPERTIES
    BUILD_RPATH "${_ff_rpath}"
    INSTALL_RPATH "${_ff_rpath}"
)

if(NOT MSVC)
    target_compile_options(faceforge-spike PRIVATE -Wall -Wextra)
endif()
