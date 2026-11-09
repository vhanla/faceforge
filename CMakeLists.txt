cmake_minimum_required(VERSION 3.21)
project(faceforge LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Export every public symbol from the shared library on Windows, so the C ABI
# does not need __declspec annotations in the header.
set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Hot loops already dispatch to AVX2/FMA at runtime, so packaged binaries stay
# portable and still use the vector paths on capable CPUs. This option is for
# local builds that want the compiler to tune every loop for the build host.
option(FF_NATIVE_BUILD "Optimize for the build machine's CPU" OFF)
if(FF_NATIVE_BUILD)
    if(MSVC)
        # MSVC has no -march=native; AVX2 is the closest portable equivalent.
        add_compile_options(/arch:AVX2)
    else()
        add_compile_options(-march=native -mtune=native)
    endif()
    message(STATUS "FF_NATIVE_BUILD: tuning for the build machine")
endif()

# Pin all artifacts to the build root so build.zig can find them regardless of
# the generator (Makefiles/Ninja vs. Visual Studio multi-config).
set(_ff_out ${CMAKE_BINARY_DIR})
foreach(_cfg "" _DEBUG _RELEASE _RELWITHDEBINFO _MINSIZEREL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY${_cfg} ${_ff_out})
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY${_cfg} ${_ff_out})
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY${_cfg} ${_ff_out})
endforeach()

set(FF_ORT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/onnxruntime"
    CACHE PATH "ONNX Runtime C/C++ SDK root")

find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs videoio)
# OpenCV 5 moved estimateAffinePartial2D/invertAffineTransform into the
# "geometry" module; OpenCV 4 still ships them in calib3d.
find_path(FF_OCV_GEOMETRY opencv2/geometry.hpp PATHS ${OpenCV_INCLUDE_DIRS} NO_DEFAULT_PATH)
if(FF_OCV_GEOMETRY)
    find_package(OpenCV REQUIRED COMPONENTS geometry)
else()
    find_package(OpenCV REQUIRED COMPONENTS calib3d)
endif()
find_package(Threads REQUIRED)

message(STATUS "OpenCV ${OpenCV_VERSION}: ${OpenCV_INCLUDE_DIRS}")
message(STATUS "ONNX Runtime: ${FF_ORT_ROOT}")

# On Windows the import library is onnxruntime.lib; on Linux libonnxruntime.so.
set(FF_ORT_LIB onnxruntime)

add_library(faceforge SHARED
    core/src/align.cpp
    core/src/engine.cpp
    core/src/models.cpp
    core/src/ort_session.cpp
    core/src/video.cpp
)

target_include_directories(faceforge
    PUBLIC  core/include
    PRIVATE core/src "${FF_ORT_ROOT}/include"
)
target_include_directories(faceforge PRIVATE ${OpenCV_INCLUDE_DIRS})
target_link_directories(faceforge PRIVATE "${FF_ORT_ROOT}/lib")

if(WIN32)
    target_link_libraries(faceforge PRIVATE
        ${FF_ORT_LIB}
        ${OpenCV_LIBS}
        Threads::Threads
        psapi
    )
else()
    target_link_libraries(faceforge PRIVATE
        ${FF_ORT_LIB}
        ${OpenCV_LIBS}
        Threads::Threads
        ${CMAKE_DL_LIBS}
    )
endif()

if(UNIX)
    set_target_properties(faceforge PROPERTIES
        BUILD_RPATH "${FF_ORT_ROOT}/lib"
        INSTALL_RPATH "$ORIGIN/../third_party/onnxruntime/lib"
    )
endif()

add_executable(ffbench bench/bench_main.cpp)
target_link_libraries(ffbench PRIVATE faceforge ${OpenCV_LIBS})
if(UNIX)
    set_target_properties(ffbench PROPERTIES
        BUILD_RPATH "${FF_ORT_ROOT}/lib;${CMAKE_CURRENT_BINARY_DIR}"
    )
endif()

# Models-free micro-benchmark for the CPU fast paths.
add_executable(ffblend bench/blend_main.cpp)
target_link_libraries(ffblend PRIVATE faceforge)
if(UNIX)
    set_target_properties(ffblend PROPERTIES
        BUILD_RPATH "${FF_ORT_ROOT}/lib;${CMAKE_CURRENT_BINARY_DIR}"
    )
endif()

# On Windows the ONNX Runtime and provider DLLs must sit next to the binaries.
if(WIN32)
    file(GLOB FF_ORT_DLLS "${FF_ORT_ROOT}/lib/*.dll")
    foreach(_target faceforge ffbench ffblend)
        # OpenCV ships as a normal imported target, so this copies its runtime
        # DLL next to the executable as well ($<TARGET_RUNTIME_DLLS> needs 3.21).
        add_custom_command(TARGET ${_target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_RUNTIME_DLLS:${_target}>
                    "$<TARGET_FILE_DIR:${_target}>"
            COMMAND_EXPAND_LISTS
            VERBATIM)
        add_custom_command(TARGET ${_target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${FF_ORT_DLLS} "$<TARGET_FILE_DIR:${_target}>"
            COMMAND_EXPAND_LISTS
            VERBATIM)
    endforeach()
endif()

install(TARGETS faceforge ffbench ffblend RUNTIME DESTINATION bin LIBRARY DESTINATION lib)
if(WIN32)
    install(FILES ${FF_ORT_DLLS} DESTINATION bin)
endif()
