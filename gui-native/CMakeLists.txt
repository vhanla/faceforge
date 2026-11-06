cmake_minimum_required(VERSION 3.20)
project(faceforge_native_ui CXX)

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
    "${AMUIE_DIR}/backend/opengl3_backend.cpp"
    "${AMUIE_DIR}/assets/font_atlas.cpp"
    "${AMUIE_DIR}/demo/faceforge.cpp"
)
target_include_directories(amuie PUBLIC "${AMUIE_DIR}")
target_link_libraries(amuie PUBLIC glfw)

if(WIN32)
    target_link_libraries(amuie PUBLIC Opengl32)
elseif(APPLE)
    target_link_libraries(amuie PUBLIC "-framework OpenGL")
else()
    target_link_libraries(amuie PUBLIC GL)
endif()

if(NOT MSVC)
    target_compile_options(amuie PRIVATE -Wall -Wextra)
endif()

# ---- FaceForge core (C ABI) ------------------------------------------------
# Build the root project first (`cmake --build build`); this target only links.
set(FACEFORGE_LIB "${FACEFORGE_ROOT}/build/libfaceforge${CMAKE_SHARED_LIBRARY_SUFFIX}")
if(NOT EXISTS "${FACEFORGE_LIB}")
    message(FATAL_ERROR
        "libfaceforge not found at ${FACEFORGE_LIB}. Build the core first: "
        "cmake -S . -B build && cmake --build build")
endif()

# ---- Spike executable ------------------------------------------------------
add_executable(faceforge-spike "${CMAKE_CURRENT_SOURCE_DIR}/app/spike_main.cpp")
target_include_directories(faceforge-spike PRIVATE "${FACEFORGE_ROOT}/core/include")
target_link_libraries(faceforge-spike PRIVATE amuie "${FACEFORGE_LIB}")

# Resolve the core and ONNX Runtime from the build tree.
set(_ff_rpath "${FACEFORGE_ROOT}/build;${FACEFORGE_ROOT}/third_party/onnxruntime/lib")
set_target_properties(faceforge-spike PROPERTIES
    BUILD_RPATH "${_ff_rpath}"
    INSTALL_RPATH "${_ff_rpath}"
)

if(NOT MSVC)
    target_compile_options(faceforge-spike PRIVATE -Wall -Wextra)
endif()
