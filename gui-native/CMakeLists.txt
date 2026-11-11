cmake_minimum_required(VERSION 3.20)
project(faceforge_native_ui C CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Pin all artifacts to the build root so the packaging scripts (and the
# portable launcher, which expect faceforge-gui[.exe] next to its DLLs) work
# regardless of the generator. Visual Studio would otherwise emit them into
# build-native/<Config>/. Mirrors the root project's output-dir pinning.
set(_ff_out ${CMAKE_BINARY_DIR})
foreach(_cfg "" _DEBUG _RELEASE _RELWITHDEBINFO _MINSIZEREL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY${_cfg} ${_ff_out})
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY${_cfg} ${_ff_out})
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY${_cfg} ${_ff_out})
endforeach()

# Native UI: the vendored AMUIE library plus a FaceForge app that talks to the
# core over the pure C ABI (core/include/faceforge.h). The core itself is built
# by the repository's root CMake project; this one only builds the UI.
find_package(glfw3 3.3 QUIET)
if(NOT glfw3_FOUND)
    # Fall back to a pinned GLFW build when the system does not ship a CMake
    # package (e.g. the Windows CI runner). On Linux, installing libglfw3-dev
    # avoids this download.
    include(FetchContent)
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(glfw
        GIT_REPOSITORY https://github.com/glfw/glfw.git
        GIT_TAG 3.4
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(glfw)
endif()

set(AMUIE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/amuie")
set(FACEFORGE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

# ---- AMUIE: vendored engine + widgets + OpenGL backend ---------------------
add_library(amuie STATIC
    "${AMUIE_DIR}/core/font.cpp"
    "${AMUIE_DIR}/widgets/ui_widgets.cpp"
    "${AMUIE_DIR}/platform/window.cpp"
    "${AMUIE_DIR}/platform/input.cpp"
    "${AMUIE_DIR}/backend/opengl3_backend.cpp"
    "${AMUIE_DIR}/backend/glad/gl.c"
    "${AMUIE_DIR}/assets/font_atlas.cpp"
)
target_include_directories(amuie PUBLIC "${AMUIE_DIR}")
# Vendored glad loader (GL 3.3 core), private to the OpenGL backend.
target_include_directories(amuie PRIVATE "${AMUIE_DIR}/backend/glad")
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
# On Windows the import library is faceforge.lib, not the DLL, and it has no
# "lib" prefix.
if(WIN32)
    set(FACEFORGE_LIB "${FACEFORGE_ROOT}/build/faceforge.lib")
else()
    set(FACEFORGE_LIB
        "${FACEFORGE_ROOT}/build/${CMAKE_SHARED_LIBRARY_PREFIX}faceforge${CMAKE_SHARED_LIBRARY_SUFFIX}")
endif()
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

# ---- FaceForge executable --------------------------------------------------
add_executable(faceforge-gui "${CMAKE_CURRENT_SOURCE_DIR}/app/main.cpp")
target_include_directories(faceforge-gui PRIVATE "${FACEFORGE_ROOT}/core/include")
target_link_libraries(faceforge-gui PRIVATE faceforge-ui "${FACEFORGE_LIB}")

# Resolve the core and ONNX Runtime from the build tree.
set(_ff_rpath "${FACEFORGE_ROOT}/build;${FACEFORGE_ROOT}/third_party/onnxruntime/lib")
set_target_properties(faceforge-gui PROPERTIES
    BUILD_RPATH "${_ff_rpath}"
    INSTALL_RPATH "${_ff_rpath}"
)

if(NOT MSVC)
    target_compile_options(faceforge-gui PRIVATE -Wall -Wextra)
endif()

# On Windows the core, ONNX Runtime providers and OpenCV DLLs must sit next to
# the executable. The root project already collects them in build/.
if(WIN32)
    file(GLOB FF_CORE_DLLS "${FACEFORGE_ROOT}/build/*.dll")
    if(FF_CORE_DLLS)
        add_custom_command(TARGET faceforge-gui POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${FF_CORE_DLLS} "$<TARGET_FILE_DIR:faceforge-gui>"
            COMMAND_EXPAND_LISTS
            VERBATIM)
    endif()
endif()
