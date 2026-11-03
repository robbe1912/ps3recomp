cmake_minimum_required(VERSION 3.20)
# Keep in step with the changelog in README.md. This sat at 0.3.0 until August
# 2026 while the project shipped 0.6.x through 0.8.0, so every build printed
# "ps3recomp runtime v0.3.0" and was believed. v0.9.0 is the first TAGGED one.
project(ps3recomp_runtime
    VERSION 0.11.0
    LANGUAGES C CXX
    DESCRIPTION "ps3recomp - Static recompilation runtime for PS3 games"
)

# ---------------------------------------------------------------------------
# Standards
# ---------------------------------------------------------------------------
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(PS3RECOMP_BUILD_TESTS "Build unit tests" OFF)
option(PS3RECOMP_ATOMIC_OBSERVATION "Compile opt-in atomic commit probes" OFF)
option(PS3RECOMP_EXECUTION_OBSERVATION "Compile opt-in SPU execution observations" OFF)
option(PS3RECOMP_SPURS_SCHEDULER_OBSERVATION "Compile opt-in SPURS admission/worker observations" OFF)

# ---------------------------------------------------------------------------
# Collect source files
# ---------------------------------------------------------------------------

# HLE library sources
file(GLOB_RECURSE HLE_SOURCES
    "libs/system/*.c"
    "libs/filesystem/*.c"
    "libs/input/*.c"
    "libs/audio/*.c"
    "libs/video/*.c"
    "libs/network/*.c"
    "libs/spurs/*.c"
    "libs/sync/*.c"
    "libs/codec/*.c"
    "libs/font/*.c"
    "libs/misc/*.c"
    "libs/hardware/*.c"
    "runtime/diag/*.c"      # yz_frontier_trace, used by the live draw engine
)
# Exclude test harnesses (e.g. libs/video/tests/*): they define their own main().
list(FILTER HLE_SOURCES EXCLUDE REGEX "/tests/")

# The Vulkan RSX backend is opt-in and uses the shared guest draw engine.
# It loads libvulkan at run time, so building needs only the Vulkan headers --
# no target-architecture libvulkan, which keeps cross-compiling simple.
option(PS3RECOMP_RSX_VULKAN "Build the Vulkan RSX backend (Windows/Linux)" OFF)
list(FILTER HLE_SOURCES EXCLUDE REGEX "rsx_vulkan_backend\\.c$")
if(PS3RECOMP_RSX_VULKAN)
    if(APPLE)
        message(FATAL_ERROR "PS3RECOMP_RSX_VULKAN is not enabled on macOS yet")
    endif()
    find_path(PS3RECOMP_VULKAN_INCLUDE_DIR vulkan/vulkan.h)
    if(NOT PS3RECOMP_VULKAN_INCLUDE_DIR)
        message(FATAL_ERROR "Vulkan headers not found. Debian/Ubuntu: "
            "libvulkan-dev, Fedora: vulkan-headers -- or pass "
            "-DPS3RECOMP_VULKAN_INCLUDE_DIR=<dir containing vulkan/vulkan.h>")
    endif()
    list(APPEND HLE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/libs/video/rsx_vulkan_backend.c")
    # HLSL -> SPIR-V for guest programs: a stub unless glslang is found below.
    list(APPEND HLE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/libs/video/rsx_shader_spirv.cpp")
endif()

# The Metal backend is Objective-C and Apple-only, so it is added explicitly
# rather than globbed. It needs no platform #ifdef of its own as a result.
if(APPLE)
    enable_language(OBJC)
    list(APPEND HLE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/libs/video/rsx_metal_backend.m")
endif()
# The HLSL -> MSL translator is C++ (it drives glslang's C++ API) and compiles
# to a stub wherever the shader tools are absent, so it is part of the
# library everywhere. See the "Guest shaders on Metal" section below.
list(APPEND HLE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/libs/video/rsx_shader_msl.cpp")
# cellAtrac decodes with FFmpeg's ATRAC3plus decoder as extracted by PPSSPP
# (LGPL-2.1, kept separate in third_party/at3_standalone -- see its README).
file(GLOB AT3_SOURCES "third_party/at3_standalone/*.cpp")
set_source_files_properties(${AT3_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")  # upstream code
list(APPEND HLE_SOURCES ${AT3_SOURCES})

# Runtime core (syscalls, PPU, SPU, memory)
file(GLOB_RECURSE RUNTIME_SOURCES
    "runtime/*.c"
    "runtime/*.cpp"
)
# Exclude:
#  - test harnesses (runtime/*/tests/*): they have their own main() and #include
#    per-game generated headers (spu_recomp.h) that a plain library build lacks.
#  - the PPU boot scaffold (runtime/ppu/, runtime/host/): these are compiled
#    per-game against the lifter-generated ppu_recomp.h (and the game's RSX/D3D12
#    backend), not as part of the game-agnostic runtime library. See
#    docs/PPU_RECOMP.md.
#  - spu_dispatch_mt.c: the musttail SPU indirect-branch fast path. It uses
#    __attribute__((musttail)), which MSVC cannot parse, and it must be compiled
#    *inside the game build* by clang so the tail call is guaranteed -- a plain
#    call leaks one resolver frame per guest loop iteration. A port adds it to
#    its own target; see templates/project/CMakeLists.txt.
list(FILTER RUNTIME_SOURCES EXCLUDE REGEX "/tests/|/runtime/ppu/|/runtime/host/|/spu_dispatch_mt\.c$")

# ---------------------------------------------------------------------------
# Static library
# ---------------------------------------------------------------------------
add_library(ps3recomp_runtime STATIC
    ${HLE_SOURCES}
    ${RUNTIME_SOURCES}
)
target_compile_definitions(ps3recomp_runtime PUBLIC
    PS3_ATOMIC_OBSERVATION=$<BOOL:${PS3RECOMP_ATOMIC_OBSERVATION}>
    PS3_EXECUTION_OBSERVATION=$<BOOL:${PS3RECOMP_EXECUTION_OBSERVATION}>
    PS3_SPURS_SCHEDULER_OBSERVATION=$<BOOL:${PS3RECOMP_SPURS_SCHEDULER_OBSERVATION}>)

target_include_directories(ps3recomp_runtime PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

# HLE modules include their own headers
target_include_directories(ps3recomp_runtime PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/system"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/filesystem"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/input"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/audio"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/video"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/network"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/spurs"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/sync"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/codec"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/font"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/misc"
    "${CMAKE_CURRENT_SOURCE_DIR}/runtime/spu"
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

# Platform-specific link libraries
if(WIN32)
    target_link_libraries(ps3recomp_runtime PRIVATE
        ws2_32      # Winsock for network modules
        xinput      # XInput for gamepad
        ole32       # COM for WASAPI audio
        bcrypt      # BCryptGenRandom for SSL RNG
    )
else()
    # cellPad and cellAudio select their SDL2 backends unconditionally off
    # Windows (see PAD_BACKEND_SDL2 / AUDIO_BACKEND_SDL2), so SDL2 is required
    # here -- docs/BUILDING.md already lists it as the Linux/macOS dependency.
    find_package(SDL2 QUIET)
    if(SDL2_FOUND)
        if(TARGET SDL2::SDL2)
            target_link_libraries(ps3recomp_runtime PRIVATE SDL2::SDL2)
        else()
            target_include_directories(ps3recomp_runtime PRIVATE ${SDL2_INCLUDE_DIRS})
            target_link_libraries(ps3recomp_runtime PRIVATE ${SDL2_LIBRARIES})
        endif()
        message(STATUS "  SDL2:         found (input + audio backends enabled)")
    else()
        message(FATAL_ERROR
            "SDL2 not found. It provides the cellPad and cellAudio backends on "
            "non-Windows hosts.\n"
            "  macOS:  brew install sdl2\n"
            "  Debian: apt install libsdl2-dev\n"
            "If SDL2 is installed in a non-standard prefix, pass "
            "-DCMAKE_PREFIX_PATH=<prefix>.")
    endif()

    find_package(Threads REQUIRED)
    target_link_libraries(ps3recomp_runtime PRIVATE Threads::Threads)
endif()

if(APPLE)
    target_link_libraries(ps3recomp_runtime PUBLIC
        "-framework Metal"
        "-framework QuartzCore"
        "-framework Foundation"
        "-framework AppKit"
    )
    set_source_files_properties(
        "${CMAKE_CURRENT_SOURCE_DIR}/libs/video/rsx_metal_backend.m"
        PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    message(STATUS "  Metal:        RSX backend enabled (rsx_metal_backend.m)")
endif()

if(PS3RECOMP_RSX_VULKAN)
    if(WIN32)
        find_package(SDL2 CONFIG REQUIRED)
        target_link_libraries(ps3recomp_runtime PRIVATE SDL2::SDL2)
    endif()
    target_include_directories(ps3recomp_runtime PRIVATE "${PS3RECOMP_VULKAN_INCLUDE_DIR}")
    # PUBLIC: ps3recomp_host selects its backend on this definition.
    target_compile_definitions(ps3recomp_runtime PUBLIC PS3RECOMP_RSX_VULKAN)
    target_link_libraries(ps3recomp_runtime PUBLIC ${CMAKE_DL_LIBS})
    message(STATUS "  Vulkan:       RSX backend enabled (rsx_vulkan_backend.c, headers in ${PS3RECOMP_VULKAN_INCLUDE_DIR})")
endif()

# cellVdec decodes H.264 / MPEG-2 through FFmpeg when this is on. libavcodec
# is loaded at run time (dlopen / LoadLibrary), never linked, so building needs
# only the FFmpeg headers, and a binary run where FFmpeg is absent -- or with
# PS3RECOMP_FFMPEG=0 -- keeps the callback-only decoders. FFmpeg stays a
# separate, user-supplied LGPL library. See libs/codec/ffmpeg_runtime.h.
option(PS3RECOMP_FFMPEG_RUNTIME "Decode media through FFmpeg loaded at run time (needs the FFmpeg headers)" OFF)
if(PS3RECOMP_FFMPEG_RUNTIME)
    find_path(PS3RECOMP_FFMPEG_INCLUDE_DIR libavcodec/avcodec.h)
    if(NOT PS3RECOMP_FFMPEG_INCLUDE_DIR)
        message(FATAL_ERROR "FFmpeg headers not found. Debian/Ubuntu: "
            "libavcodec-dev, Fedora: ffmpeg-free-devel, MSYS2: mingw-w64-x86_64-ffmpeg "
            "-- or pass -DPS3RECOMP_FFMPEG_INCLUDE_DIR=<dir containing libavcodec/avcodec.h>")
    endif()
    target_include_directories(ps3recomp_runtime PRIVATE "${PS3RECOMP_FFMPEG_INCLUDE_DIR}")
    target_compile_definitions(ps3recomp_runtime PRIVATE PS3RECOMP_FFMPEG_RUNTIME)
    target_link_libraries(ps3recomp_runtime PUBLIC ${CMAKE_DL_LIBS})
    message(STATUS "  FFmpeg:       run-time loaded (headers in ${PS3RECOMP_FFMPEG_INCLUDE_DIR})")
endif()

# ---------------------------------------------------------------------------
# Guest shaders on Metal: HLSL -> SPIR-V -> MSL
# ---------------------------------------------------------------------------
# The RSX decompilers emit HLSL for the D3D12 backend. Metal cannot compile
# that, so libs/video/rsx_shader_msl.cpp lowers it with glslang (HLSL ->
# SPIR-V) and spirv-cross (SPIR-V -> MSL). Both are optional: without them the
# module compiles to a stub, rsx_hlsl_to_msl_available() says so, and the
# Metal backend stays on its fixed-function path. The translation has no GPU
# in it, so it is built and tested on Linux as well -- that is where the
# decompilers' HLSL meets glslang's front end on every push.
#
#   macOS:  brew install glslang spirv-cross
#   Debian: apt install glslang-dev glslang-tools spirv-tools \
#                       libspirv-cross-c-shared-dev
#           (glslang-tools only because the packaged CMake config declares
#           the standalone binary as an imported target and fails to load
#           without it)
set(PS3RECOMP_MSL_TRANSLATION OFF)
if(UNIX OR PS3RECOMP_RSX_VULKAN)
    find_package(glslang CONFIG QUIET)
endif()

if(UNIX)
    # spirv-cross's C API. Debian ships it as a shared library and Homebrew as
    # a static one, under different pkg-config names -- and pkg-config itself
    # is not a given on a Mac, so fall back to a plain header + library search.
    set(SPVC_OK OFF)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(SPVC QUIET spirv-cross-c-shared)
        if(NOT SPVC_FOUND)
            pkg_check_modules(SPVC QUIET spirv-cross-c)
        endif()
        if(SPVC_FOUND)
            set(SPVC_INC     ${SPVC_INCLUDE_DIRS})
            set(SPVC_LIBS    ${SPVC_LINK_LIBRARIES})
            set(SPVC_LIBDIRS ${SPVC_LIBRARY_DIRS})
            set(SPVC_OK ON)
        endif()
    endif()
    if(NOT SPVC_OK)
        find_path(SPVC_INC_DIR spirv_cross_c.h PATH_SUFFIXES spirv_cross)
        find_library(SPVC_LIB NAMES spirv-cross-c-shared spirv-cross-c)
        if(SPVC_INC_DIR AND SPVC_LIB)
            set(SPVC_INC  ${SPVC_INC_DIR})
            set(SPVC_LIBS ${SPVC_LIB})
            get_filename_component(SPVC_LIBDIRS "${SPVC_LIB}" DIRECTORY)
            set(SPVC_OK ON)
        endif()
    endif()

    if(glslang_FOUND AND SPVC_OK
       AND TARGET glslang::glslang AND TARGET glslang::SPIRV)
        add_library(ps3recomp_msl_translation INTERFACE)
        target_compile_definitions(ps3recomp_msl_translation INTERFACE
            PS3RECOMP_HAVE_MSL_TRANSLATION=1)
        target_include_directories(ps3recomp_msl_translation INTERFACE ${SPVC_INC})
        target_link_libraries(ps3recomp_msl_translation INTERFACE
            glslang::glslang glslang::SPIRV ${SPVC_LIBS})
        if(TARGET glslang::glslang-default-resource-limits)
            target_link_libraries(ps3recomp_msl_translation INTERFACE
                glslang::glslang-default-resource-limits)
        endif()
        # A static spirv-cross C API (Homebrew's) needs the backends it was
        # built from, and neither pkg-config nor the library name says so:
        # pick up the static siblings next to it. The shared one carries
        # its own.
        if(SPVC_LIBS MATCHES "\\.a$")
            foreach(part msl glsl hlsl cpp reflect util core)
                find_library(SPVC_PART_${part} NAMES spirv-cross-${part}
                    HINTS ${SPVC_LIBDIRS} NO_DEFAULT_PATH)
                if(SPVC_PART_${part})
                    target_link_libraries(ps3recomp_msl_translation INTERFACE
                        ${SPVC_PART_${part}})
                endif()
            endforeach()
        endif()
        target_link_libraries(ps3recomp_runtime PRIVATE ps3recomp_msl_translation)
        set(PS3RECOMP_MSL_TRANSLATION ON)
        message(STATUS "  Shaders:      guest programs translated to MSL (glslang ${glslang_VERSION}, ${SPVC_LIBS})")
    else()
        message(STATUS "  Shaders:      glslang/spirv-cross not found; Metal backend stays fixed-function")
    endif()
endif()

# Guest shaders on Vulkan: HLSL -> SPIR-V. glslang only -- Vulkan consumes
# SPIR-V, so spirv-cross is not needed. Without glslang
# libs/video/rsx_shader_spirv.cpp is a stub and the Vulkan backend stays on
# its fixed-function path.
if(PS3RECOMP_RSX_VULKAN)
    if(glslang_FOUND AND TARGET glslang::glslang AND TARGET glslang::SPIRV)
        set_source_files_properties(
            "${CMAKE_CURRENT_SOURCE_DIR}/libs/video/rsx_shader_spirv.cpp"
            PROPERTIES COMPILE_DEFINITIONS PS3RECOMP_HAVE_SPIRV_TRANSLATION=1)
        target_link_libraries(ps3recomp_runtime PRIVATE glslang::glslang glslang::SPIRV)
        if(TARGET glslang::glslang-default-resource-limits)
            target_link_libraries(ps3recomp_runtime PRIVATE
                glslang::glslang-default-resource-limits)
        endif()
        message(STATUS "  Vulkan:       guest programs available (glslang ${glslang_VERSION}); "
                       "opt in with PS3RECOMP_VK_GUEST_PROGRAMS=1")
    else()
        message(STATUS "  Vulkan:       glslang not found; guest programs unavailable")
    endif()
endif()

# ---------------------------------------------------------------------------
# Compiler flags
# ---------------------------------------------------------------------------
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(ps3recomp_runtime PRIVATE
        -Wall -Wextra -Wno-unused-parameter
        -fno-strict-aliasing
        # No spontaneous fused multiply-add. MSVC never contracts, and x86-64
        # without -mfma has nothing to contract into, so every validated
        # result came from separate multiplies and adds; arm64 has FMA in
        # the base ISA and Clang fuses by default there. A PPC fmadd is
        # spelt as one where the lifter means one.
        -ffp-contract=off
        # Explicit, because the per-config defaults cannot be trusted: this
        # tree's caches carry CMAKE_C_FLAGS_RELEASE="" and CMAKE_CXX_FLAGS_RELEASE="",
        # and clang-cl with no /O flag compiles at /Od -- a "Release" runtime
        # was unoptimised without a word. Duplicating a default -O2 is harmless.
        $<$<CONFIG:Release,RelWithDebInfo>:-O2>
    )
    # Optional shuffle ISA is confined to the capability-checked helper.
elseif(MSVC)
    # Real cl.exe -- clang-cl reports its id as Clang and is handled above.
    #
    # This does not currently build. The runtime and the lifted output use GCC/
    # Clang builtins that cl has no equivalent for, and the failure lands as a
    # wall of errors in whichever file the build happens to reach first rather
    # than as anything that names the cause. Say it here instead.
    message(WARNING
        "Building with cl.exe (MSVC). This is not currently supported.\n"
        "  The runtime uses __atomic_* builtins, __int128 and __builtin_bswap*, "
        "and the lifted output uses __int128 for the PPC 64x64 multiplies. "
        "cl has none of them.\n"
        "  Use clang-cl instead -- it consumes the MSVC headers and libraries, "
        "so nothing else about a Visual Studio build changes:\n"
        "    cmake -S . -B build -G Ninja "
        "-DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl\n"
        "  See docs/BUILDING.md. Porting the intrinsics to cl is a welcome "
        "contribution; nothing else stands in the way.")
    target_compile_options(ps3recomp_runtime PRIVATE
        /W3 /wd4100 /wd4244 /wd4267 /wd4996
        /Zc:__cplusplus
        /experimental:c11atomics   # Enable C11 <stdatomic.h> support
    )
    target_compile_definitions(ps3recomp_runtime PRIVATE
        _CRT_SECURE_NO_WARNINGS
    )
endif()

if(NOT MSVC AND NOT WIN32)
    # dladdr/Dl_info (runtime/platform/win32_backtrace.h) are GNU extensions
    # that glibc hides unless _GNU_SOURCE is set. g++ defines it implicitly,
    # so the C++ boot scaffold never noticed; a C file including the same
    # shim does not get it for free.
    target_compile_definitions(ps3recomp_runtime PRIVATE _GNU_SOURCE)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(PS3RECOMP_BUILD_TESTS)
    include(CTest)
    if(UNIX AND CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        add_executable(test_spu_dispatch_checks runtime/spu/tests/test_dispatch_checks.c runtime/diag/spu_execution_observer.c)
        target_compile_definitions(test_spu_dispatch_checks PRIVATE PS3_EXECUTION_OBSERVATION=1)
        target_include_directories(test_spu_dispatch_checks PRIVATE include runtime runtime/spu)
        target_compile_options(test_spu_dispatch_checks PRIVATE -UNDEBUG -ffunction-sections -fdata-sections)
        if(APPLE)
            target_link_options(test_spu_dispatch_checks PRIVATE -Wl,-dead_strip)
        else()
            target_link_options(test_spu_dispatch_checks PRIVATE -Wl,--gc-sections)
        endif()
        add_test(NAME spu_dispatch_checks COMMAND test_spu_dispatch_checks)
    endif()
    # sync_stress is written directly against the Win32 threading API
    # (WaitForMultipleObjects, joinable HANDLEs, Interlocked*, CRITICAL_SECTION).
    # Off Windows all of that comes from runtime/platform/win32_compat.h, so
    # the suite runs everywhere -- and is the arm64 memory-model regression
    # net for the lv2 sync primitives.
    add_subdirectory(tests/sync_stress)
    # cellFiber switches contexts with Windows fibers on Windows and ucontext
    # everywhere else. Apple deprecated the ucontext routines and gates both
    # them and the shape of ucontext_t on _XOPEN_SOURCE, so whether that path
    # works on arm64 is a question to answer rather than assume.
    add_subdirectory(tests/fiber_switch)
    # cellVdec through its guest API: the callback-only decoder always, and
    # real H.264 / MPEG-2 decode when FFmpeg is on (libs/codec/tests).
    add_subdirectory(libs/codec/tests)
    # cellGcm's FIFO walker without a backend: where GET_REPORT writes for
    # each report DMA context. Links the runtime like the POSIX host below.
    if(UNIX)
        add_subdirectory(tests/gcm_reports)
    endif()

    find_package(Python3 3.10 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME spurs_task_exit_code_initialize
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_spurs_task_exit_code_initialize.py")
        add_test(NAME spurs_taskset_size
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_spurs_taskset_size.py")
        add_test(NAME spu_sdcrz
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_spu_sdcrz.py")
    endif()
    if(NOT WIN32)
        find_package(Python3 3.10 COMPONENTS Interpreter QUIET)
        if(Python3_Interpreter_FOUND)
            add_test(NAME spu_fnms_zero_extended
                COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_spu_fnms_zero_extended.py")
        endif()
    endif()
endif()

# ---------------------------------------------------------------------------
# Backend host harness
# ---------------------------------------------------------------------------
# Drives the cellGcm -> RSX -> backend bridge with no lifted game, so a backend
# can be brought up and regression-tested without a game binary or a recompiler
# run. Uses Metal on Apple, opt-in Vulkan on Windows/Linux, or the headless
# null contract where no graphics backend is selected.
if(UNIX OR (WIN32 AND PS3RECOMP_RSX_VULKAN))
    add_executable(ps3recomp_host
        runtime/host/host_posix.c
        runtime/host/host_stubs.c
    )
    target_link_libraries(ps3recomp_host PRIVATE ps3recomp_runtime)
    if(NOT WIN32)
        # glibc keeps the math functions in a separate library; MSVC does not.
        target_link_libraries(ps3recomp_host PRIVATE m)
    endif()
    target_include_directories(ps3recomp_host PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/libs/video"
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    # The runtime carries C++ (and, with translation on, spirv-cross), so the
    # C++ driver must do the final link or the standard library goes missing.
    set_target_properties(ps3recomp_host PROPERTIES LINKER_LANGUAGE CXX)
    if(WIN32 AND PS3RECOMP_RSX_VULKAN)
        add_custom_command(TARGET ps3recomp_host POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "$<TARGET_FILE:SDL2::SDL2>" "$<TARGET_FILE_DIR:ps3recomp_host>")
    endif()

    # Guest shader translation, decompilers -> HLSL -> MSL, with no GPU: the
    # regression net for the Metal backend's shader path on every platform.
    if(PS3RECOMP_MSL_TRANSLATION)
        add_executable(test_shader_msl
            libs/video/tests/test_shader_msl.c
            libs/video/rsx_shader_msl.cpp
            libs/video/rsx_vp_decompiler.c
            libs/video/rsx_fp_decompiler.c
        )
        target_include_directories(test_shader_msl PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/libs/video"
            "${CMAKE_CURRENT_SOURCE_DIR}/include"
        )
        target_link_libraries(test_shader_msl PRIVATE ps3recomp_msl_translation)
    endif()
endif()

# ---------------------------------------------------------------------------
# PPU boot smoke title
# ---------------------------------------------------------------------------
# The one target that LINKS AND RUNS the PPU boot scaffold. runtime/ppu/ is the
# game-agnostic boot path every port is built on, and nothing here has ever
# built it: it #includes the lifter's per-game ppu_recomp.h, so
# tools/check_ppu_scaffold.py compiles it against a stub header and stops
# there. Compiling is not running, and on POSIX the loader, the NID bridge, the
# lv2 syscalls, guest threads, the frame clock and the RSX backend had never
# once run together.
#
# runtime/ppu/tests/smoke/ supplies what was missing: a hand-written program in
# the lifter's ABI, and tools/make_smoke_elf.py writes both the PPU ELF it is
# loaded from and the ppu_recomp.h it compiles against -- the header from
# ppu_lifter.py's own HEADER_PREAMBLE, so the synthetic title cannot drift away
# from the ABI a real one gets.
#
# The HLE NID table is generated exactly as a port generates it, so the smoke
# title's firmware imports resolve through the same bridge a game's do.
if(UNIX)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(NOT Python3_Interpreter_FOUND)
        message(STATUS "  Boot smoke:   no python3; ps3recomp_boot_smoke not built")
    else()
        set(SMOKE_GEN "${CMAKE_CURRENT_BINARY_DIR}/smoke")

        add_custom_command(
            OUTPUT "${SMOKE_GEN}/smoke.elf" "${SMOKE_GEN}/ppu_recomp.h"
            COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/make_smoke_elf.py"
                    --out    "${SMOKE_GEN}/smoke.elf"
                    --header "${SMOKE_GEN}/ppu_recomp.h"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/make_smoke_elf.py"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/ppu_lifter.py"
            COMMENT "Generating the smoke title's image and ppu_recomp.h")

        add_custom_command(
            OUTPUT "${SMOKE_GEN}/ppu_hle_nids.cpp"
            COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_hle_nids.py"
                    --out "${SMOKE_GEN}/ppu_hle_nids.cpp" --all
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_hle_nids.py"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/nid_database.py"
            COMMENT "Generating the HLE NID -> handler table")

        add_executable(ps3recomp_boot_smoke
            runtime/ppu/tests/boot_main.cpp
            runtime/ppu/tests/smoke/smoke_recomp.c
            runtime/ppu/ppu_loader.cpp
            runtime/memory/guest_read_sync.c
            runtime/ppu/ppu_hle.cpp
            runtime/ppu/ppu_fs.cpp
            runtime/ppu/ppu_sysprx.cpp
            "${SMOKE_GEN}/ppu_hle_nids.cpp"
            "${SMOKE_GEN}/ppu_recomp.h"
        )
        target_include_directories(ps3recomp_boot_smoke PRIVATE
            "${SMOKE_GEN}"                                  # the generated ppu_recomp.h
            "${CMAKE_CURRENT_SOURCE_DIR}/include"
            "${CMAKE_CURRENT_SOURCE_DIR}/runtime/ppu"
            "${CMAKE_CURRENT_SOURCE_DIR}/runtime/spu"
            "${CMAKE_CURRENT_SOURCE_DIR}/runtime/memory"
            "${CMAKE_CURRENT_SOURCE_DIR}/runtime/syscalls"
            "${CMAKE_CURRENT_SOURCE_DIR}/runtime/platform"
            "${CMAKE_CURRENT_SOURCE_DIR}/libs/video"
        )
        target_link_libraries(ps3recomp_boot_smoke PRIVATE ps3recomp_runtime)
        set_target_properties(ps3recomp_boot_smoke PROPERTIES LINKER_LANGUAGE CXX)

        # Where the runnable image lands, so CI and a developer both say
        #   ./build/ps3recomp_boot_smoke build/smoke/smoke.elf
        message(STATUS "  Boot smoke:   ps3recomp_boot_smoke + smoke/smoke.elf")

        # The same lift again, arranged the way a port's own tree arranges one:
        # the generated ppu_recomp.h and the lifted source in one directory,
        # which is what templates/project wants for -DRECOMP_DIR. The header is
        # generated into the build tree and the source lives in the repository,
        # so a directory holding both has to be made rather than pointed at.
        #
        # This is what lets CI build the project template against a real lift
        # and run it -- the check that a port started from templates/project
        # still boots, on a platform where it can be run.
        set(SMOKE_LIFT "${SMOKE_GEN}/lift")
        set(SMOKE_LIFT_SRC
            "${CMAKE_CURRENT_SOURCE_DIR}/runtime/ppu/tests/smoke/smoke_recomp.c")
        add_custom_command(
            OUTPUT "${SMOKE_LIFT}/ppu_recomp.h" "${SMOKE_LIFT}/smoke_recomp.c"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${SMOKE_LIFT}"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${SMOKE_GEN}/ppu_recomp.h" "${SMOKE_LIFT}/ppu_recomp.h"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${SMOKE_LIFT_SRC}" "${SMOKE_LIFT}/smoke_recomp.c"
            DEPENDS "${SMOKE_GEN}/ppu_recomp.h" "${SMOKE_LIFT_SRC}"
            COMMENT "Staging the smoke title's lift as a RECOMP_DIR")
        add_custom_target(ps3recomp_smoke_lift ALL
            DEPENDS "${SMOKE_LIFT}/ppu_recomp.h" "${SMOKE_LIFT}/smoke_recomp.c")
        message(STATUS "  Smoke lift:   smoke/lift (a RECOMP_DIR for templates/project)")
    endif()
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
install(TARGETS ps3recomp_runtime
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)

install(DIRECTORY include/ps3emu
    DESTINATION include
    FILES_MATCHING PATTERN "*.h"
)

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
message(STATUS "")
message(STATUS "ps3recomp runtime v${PROJECT_VERSION}")
message(STATUS "  C compiler:   ${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}")
message(STATUS "  C++ compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  Build type:   ${CMAKE_BUILD_TYPE}")
message(STATUS "")

# Authored device pixels exercise actual HLE accessors and row conversion.
if(PS3RECOMP_BUILD_TESTS OR BUILD_TESTING)
    enable_testing()
    add_executable(test_gcm_draw_trace tests/test_gcm_draw_trace.c)
    target_include_directories(test_gcm_draw_trace PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    if(MSVC)
        target_compile_definitions(test_gcm_draw_trace PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
    add_test(NAME gcm_draw_trace
        COMMAND "${CMAKE_COMMAND}" "-DPROGRAM=$<TARGET_FILE:test_gcm_draw_trace>"
                "-DWORK=${CMAKE_CURRENT_BINARY_DIR}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_gcm_draw_trace.cmake.in")
    add_executable(test_rsx_record_batch tests/test_rsx_record_batch.c)
    add_test(NAME rsx_record_batch COMMAND test_rsx_record_batch)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME rsx_triangle_capacity COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_triangle_capacity.py" --cc "${CMAKE_C_COMPILER}")
    set_tests_properties(rsx_triangle_capacity PROPERTIES TIMEOUT 180)
    add_executable(test_rsx_texture_remap tests/test_rsx_texture_remap.c
        libs/video/rsx_texture_layout.c)
    target_include_directories(test_rsx_texture_remap PRIVATE include libs/video)
    add_test(NAME rsx_texture_remap COMMAND test_rsx_texture_remap)
    add_executable(test_rsx_timeline_select tests/test_rsx_timeline_select.c)
    target_include_directories(test_rsx_timeline_select PRIVATE libs/video)
    add_test(NAME rsx_timeline_select COMMAND test_rsx_timeline_select)
    add_executable(test_rsx_mrt_masks tests/test_rsx_mrt_masks.c libs/video/rsx_commands.c)
    target_include_directories(test_rsx_mrt_masks PRIVATE include libs/video)
    add_test(NAME rsx_mrt_masks COMMAND test_rsx_mrt_masks)
    add_executable(test_rsx_mrt_observer tests/test_rsx_mrt_observer.c)
    target_include_directories(test_rsx_mrt_observer PRIVATE libs/video)
    add_test(NAME rsx_mrt_observer COMMAND test_rsx_mrt_observer)
    if(WIN32)
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        if(PS3RECOMP_RSX_VULKAN AND NOT glslang_FOUND)
            add_test(NAME rsx_vulkan_translation_required
                COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_rsx_vulkan_host.py"
                    "$<TARGET_FILE:ps3recomp_host>" --missing-translation
                    --output "${CMAKE_CURRENT_BINARY_DIR}/vulkan-unavailable-check")
            set_tests_properties(rsx_vulkan_translation_required PROPERTIES TIMEOUT 60)
        endif()
        add_executable(test_rsx_mrt_masks_gpu tests/rsx_mrt_masks_gpu.cpp)
        target_include_directories(test_rsx_mrt_masks_gpu PRIVATE libs/video)
        target_link_libraries(test_rsx_mrt_masks_gpu PRIVATE d3d12 dxgi d3dcompiler)
        add_test(NAME rsx_mrt_masks_gpu COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_rsx_mrt_masks.py" "$<TARGET_FILE:test_rsx_mrt_masks_gpu>")
        set_tests_properties(rsx_mrt_masks_gpu PROPERTIES TIMEOUT 90)

        add_executable(test_rsx_fp_pack_gpu tests/rsx_fp_pack_gpu.cpp
            libs/video/rsx_fp_decompiler.c)
        target_include_directories(test_rsx_fp_pack_gpu PRIVATE include libs/video)
        target_compile_features(test_rsx_fp_pack_gpu PRIVATE cxx_std_17)
        target_link_libraries(test_rsx_fp_pack_gpu PRIVATE d3d11 d3dcompiler)
        add_test(NAME rsx_fp_pack COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_rsx_fp_pack.py" "$<TARGET_FILE:test_rsx_fp_pack_gpu>")
        set_tests_properties(rsx_fp_pack PROPERTIES TIMEOUT 180)

        add_executable(test_rsx_fp_source_sign_gpu tests/rsx_fp_source_sign_gpu.cpp
            libs/video/rsx_fp_decompiler.c)
        target_include_directories(test_rsx_fp_source_sign_gpu PRIVATE include libs/video)
        target_compile_features(test_rsx_fp_source_sign_gpu PRIVATE cxx_std_17)
        target_link_libraries(test_rsx_fp_source_sign_gpu PRIVATE d3d11 d3dcompiler)
        add_test(NAME rsx_fp_source_sign COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_rsx_fp_source_sign.py" "$<TARGET_FILE:test_rsx_fp_source_sign_gpu>")
        set_tests_properties(rsx_fp_source_sign PROPERTIES TIMEOUT 180)

        add_executable(test_rsx_depth_surfaces_gpu tests/rsx_depth_surfaces_gpu.c
            libs/video/rsx_commands.c)
        target_include_directories(test_rsx_depth_surfaces_gpu PRIVATE include libs/video)
        target_link_libraries(test_rsx_depth_surfaces_gpu PRIVATE d3d12 dxgi dxguid d3dcompiler)
        add_test(NAME rsx_depth_required COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_rsx_depth_required.py" "$<TARGET_FILE:test_rsx_depth_surfaces_gpu>")
        set_tests_properties(rsx_depth_required PROPERTIES TIMEOUT 180)

        add_executable(test_rsx_surface_readback_gpu tests/rsx_surface_readback_gpu.cpp)
        target_include_directories(test_rsx_surface_readback_gpu PRIVATE libs/video)
        target_link_libraries(test_rsx_surface_readback_gpu PRIVATE d3d12 dxgi dxguid)
        add_test(NAME rsx_surface_readback_gpu COMMAND test_rsx_surface_readback_gpu)
        set_tests_properties(rsx_surface_readback_gpu PROPERTIES TIMEOUT 45)

        add_executable(test_rsx_depth_alias_gpu tests/rsx_depth_alias_gpu.c)
        target_include_directories(test_rsx_depth_alias_gpu PRIVATE libs/video)
        target_link_libraries(test_rsx_depth_alias_gpu PRIVATE d3d12 dxgi dxguid d3dcompiler)
        add_test(NAME rsx_depth_alias COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_rsx_depth_alias.py" "$<TARGET_FILE:test_rsx_depth_alias_gpu>")
        set_tests_properties(rsx_depth_alias PROPERTIES TIMEOUT 180)

        add_executable(test_rsx_address_gpu tests/rsx_address_d3d12_gpu.cpp)
        target_include_directories(test_rsx_address_gpu PRIVATE libs/video)
        target_link_libraries(test_rsx_address_gpu PRIVATE d3d12 dxgi d3dcompiler)
        add_executable(test_rsx_address_tables tests/rsx_address_tables_test.c)
        target_include_directories(test_rsx_address_tables PRIVATE libs/video)
        target_link_libraries(test_rsx_address_tables PRIVATE dxguid)
        add_test(NAME rsx_address COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_rsx_address.py"
            "$<TARGET_FILE:test_rsx_address_gpu>" "$<TARGET_FILE:test_rsx_address_tables>")
        set_tests_properties(rsx_address PROPERTIES TIMEOUT 300)
    endif()
    if(WIN32)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/libs/input/cellPad.c")
        file(READ "${CMAKE_CURRENT_SOURCE_DIR}/libs/input/cellPad.c" _pad_source)
        string(REPLACE "if (pad_file_applied && data->len > 0) --held_n;" "if (pad_file_applied) --held_n;" _pad_legacy "${_pad_source}")
        if(_pad_source STREQUAL _pad_legacy)
            message(FATAL_ERROR "PAD_FILE control no longer mutates the report-count guard")
        endif()
        file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/pad_file_legacy.c" "${_pad_legacy}")
        foreach(_variant normal legacy)
            add_executable(test_pad_file_${_variant} tests/pad_file_reports.c)
            target_include_directories(test_pad_file_${_variant} PRIVATE include libs/input)
            target_compile_definitions(test_pad_file_${_variant} PRIVATE _CRT_SECURE_NO_WARNINGS)
            if(MSVC)
                target_compile_options(test_pad_file_${_variant} PRIVATE /Gy)
                target_link_options(test_pad_file_${_variant} PRIVATE /OPT:REF)
            endif()
            target_link_libraries(test_pad_file_${_variant} PRIVATE user32 xinput)
        endforeach()
        target_compile_definitions(test_pad_file_legacy PRIVATE PAD_IMPLEMENTATION="${CMAKE_CURRENT_BINARY_DIR}/pad_file_legacy.c")
        add_test(NAME pad_file_reports COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_pad_file_reports.py" "$<TARGET_FILE:test_pad_file_normal>" "$<TARGET_FILE:test_pad_file_legacy>")
        set_tests_properties(pad_file_reports PROPERTIES TIMEOUT 40)
    endif()
    add_test(NAME ppu_inline_read_sync COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_ppu_inline_read_sync.py"
        --compiler "${CMAKE_C_COMPILER}")
    set_tests_properties(ppu_inline_read_sync PROPERTIES TIMEOUT 90)

    foreach(_shuffle_variant IN ITEMS dispatch scalar)
        add_executable(test_spu_shufb_${_shuffle_variant} runtime/spu/tests/test_spu_shufb_dispatch.c)
        target_include_directories(test_spu_shufb_${_shuffle_variant} PRIVATE runtime/spu)
        add_test(NAME spu_shufb_${_shuffle_variant} COMMAND test_spu_shufb_${_shuffle_variant})
        set_tests_properties(spu_shufb_${_shuffle_variant} PROPERTIES TIMEOUT 40)
    endforeach()
    target_compile_definitions(test_spu_shufb_scalar PRIVATE SPU_SHUFB_FORCE_SCALAR)
    add_executable(test_spu_float_simd runtime/spu/tests/test_spu_float_simd.c runtime/spu/spu_float_simd.c)
    target_include_directories(test_spu_float_simd PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/runtime/spu)
    if(NOT WIN32)
        target_link_libraries(test_spu_float_simd PRIVATE m)
    endif()
    add_test(NAME spu_float_simd COMMAND $<TARGET_FILE:test_spu_float_simd>)
    set_tests_properties(spu_float_simd PROPERTIES TIMEOUT 40)

    add_executable(test_spu_mfc_registry runtime/spu/tests/test_spu_mfc_registry.c
        runtime/platform/win32_compat.c)
    target_include_directories(test_spu_mfc_registry PRIVATE runtime/spu runtime/platform)
    if(NOT WIN32)
        target_link_libraries(test_spu_mfc_registry PRIVATE pthread)
    endif()
    add_test(NAME spu_mfc_registry COMMAND test_spu_mfc_registry)
    set_tests_properties(spu_mfc_registry PROPERTIES TIMEOUT 30)

    add_executable(test_rsx_pipeline_cache tests/rsx_pipeline_cache.c)
    target_include_directories(test_rsx_pipeline_cache PRIVATE libs/video)
    add_test(NAME rsx_pipeline_cache COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_rsx_pipeline_cache.py" "$<TARGET_FILE:test_rsx_pipeline_cache>")
    set_tests_properties(rsx_pipeline_cache PROPERTIES TIMEOUT 40)
    add_executable(test_rsx_slot_pool tests/rsx_slot_pool.c)
    target_include_directories(test_rsx_slot_pool PRIVATE libs/video)
    add_test(NAME rsx_slot_pool COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_rsx_slot_pool.py" "$<TARGET_FILE:test_rsx_slot_pool>")
    set_tests_properties(rsx_slot_pool PROPERTIES TIMEOUT 45)
    if(WIN32)
        add_executable(test_rsx_slot_pool_gpu tests/rsx_slot_pool_gpu.cpp)
        target_include_directories(test_rsx_slot_pool_gpu PRIVATE libs/video)
        target_compile_features(test_rsx_slot_pool_gpu PRIVATE cxx_std_17)
        target_link_libraries(test_rsx_slot_pool_gpu PRIVATE d3d11 d3dcompiler)
        add_test(NAME rsx_slot_pool_gpu COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_rsx_slot_pool_gpu.py" "$<TARGET_FILE:test_rsx_slot_pool_gpu>")
        set_tests_properties(rsx_slot_pool_gpu PROPERTIES TIMEOUT 180)
    endif()
    if(WIN32)
        add_test(NAME rsx_shader_pipeline COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_shader_pipeline.py" --cc "${CMAKE_C_COMPILER}")
        set_tests_properties(rsx_shader_pipeline PROPERTIES TIMEOUT 240)
    endif()
    add_executable(test_rsx_shader_cache tests/rsx_shader_cache.c)
    target_include_directories(test_rsx_shader_cache PRIVATE libs/video)
    add_test(NAME rsx_shader_cache COMMAND test_rsx_shader_cache)
    set_tests_properties(rsx_shader_cache PROPERTIES TIMEOUT 15)
    if(WIN32)
        add_executable(test_rsx_shader_cache_gpu tests/rsx_shader_cache_gpu.cpp)
        target_include_directories(test_rsx_shader_cache_gpu PRIVATE libs/video)
        target_compile_features(test_rsx_shader_cache_gpu PRIVATE cxx_std_17)
        target_link_libraries(test_rsx_shader_cache_gpu PRIVATE d3d11 d3dcompiler)
        add_test(NAME rsx_shader_cache_gpu COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_rsx_shader_cache_gpu.py" "$<TARGET_FILE:test_rsx_shader_cache_gpu>")
        set_tests_properties(rsx_shader_cache_gpu PROPERTIES TIMEOUT 180)
    endif()
    add_executable(test_rsx_gpu_references tests/rsx_gpu_references.c)
    target_include_directories(test_rsx_gpu_references PRIVATE libs/video)
    add_test(NAME rsx_gpu_references COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_rsx_gpu_references.py" "$<TARGET_FILE:test_rsx_gpu_references>")
    set_tests_properties(rsx_gpu_references PROPERTIES TIMEOUT 45)
    if(WIN32)
        add_executable(test_rsx_gpu_pso_gpu tests/rsx_gpu_pso_gpu.cpp)
        target_include_directories(test_rsx_gpu_pso_gpu PRIVATE libs/video)
        target_compile_features(test_rsx_gpu_pso_gpu PRIVATE cxx_std_17)
        target_link_libraries(test_rsx_gpu_pso_gpu PRIVATE d3d12 dxgi d3dcompiler)
        add_test(NAME rsx_gpu_pso COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_rsx_gpu_pso.py" "$<TARGET_FILE:test_rsx_gpu_pso_gpu>")
        set_tests_properties(rsx_gpu_pso PROPERTIES TIMEOUT 100)
    endif()
    add_executable(test_rsx_gpu_submission tests/rsx_gpu_submission.c)
    target_include_directories(test_rsx_gpu_submission PRIVATE libs/video)
    add_test(NAME rsx_gpu_submission COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_rsx_gpu_submission.py" "$<TARGET_FILE:test_rsx_gpu_submission>")
    set_tests_properties(rsx_gpu_submission PROPERTIES TIMEOUT 45)

    add_executable(test_rsx_fp_decompiler libs/video/tests/test_fp_decompiler.c
        libs/video/rsx_fp_decompiler.c)
    target_include_directories(test_rsx_fp_decompiler PRIVATE include libs/video)
    add_test(NAME rsx_fp_decompiler COMMAND test_rsx_fp_decompiler)
    add_executable(test_rsx_upload_buffer tests/test_rsx_upload_buffer.c)
    add_test(NAME rsx_upload_buffer COMMAND test_rsx_upload_buffer)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/libs/video/rsx_d3d12_backend.c" RSX_UPLOAD_SOURCE)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS libs/video/rsx_d3d12_backend.c)
    string(FIND "${RSX_UPLOAD_SOURCE}" "static u32 upload_strip_vp(" RSX_UPLOAD_START)
    string(FIND "${RSX_UPLOAD_SOURCE}" "/* Fetch index k" RSX_UPLOAD_END)
    if(RSX_UPLOAD_START LESS 0 OR RSX_UPLOAD_END LESS RSX_UPLOAD_START)
        message(FATAL_ERROR "Cannot extract production VP strip upload regression seam")
    endif()
    math(EXPR RSX_UPLOAD_LENGTH "${RSX_UPLOAD_END} - ${RSX_UPLOAD_START}")
    string(SUBSTRING "${RSX_UPLOAD_SOURCE}" ${RSX_UPLOAD_START} ${RSX_UPLOAD_LENGTH} UPLOAD_STRIP_FUNCTION)
    configure_file(tests/test_rsx_upload_strip.c.in test_rsx_upload_strip.c @ONLY)
    add_executable(test_rsx_upload_strip "${CMAKE_CURRENT_BINARY_DIR}/test_rsx_upload_strip.c")
    target_include_directories(test_rsx_upload_strip PRIVATE include libs/video)
    add_test(NAME rsx_upload_strip COMMAND test_rsx_upload_strip)
    string(FIND "${RSX_UPLOAD_SOURCE}" "static u32 upload_strip_vp_indexed(" RSX_INDEXED_START)
    string(FIND "${RSX_UPLOAD_SOURCE}" "static void d3d12_draw_arrays(" RSX_INDEXED_END)
    if(RSX_INDEXED_START LESS 0 OR RSX_INDEXED_END LESS RSX_INDEXED_START)
        message(FATAL_ERROR "Cannot extract production indexed strip upload regression seam")
    endif()
    math(EXPR RSX_INDEXED_LENGTH "${RSX_INDEXED_END} - ${RSX_INDEXED_START}")
    string(SUBSTRING "${RSX_UPLOAD_SOURCE}" ${RSX_INDEXED_START} ${RSX_INDEXED_LENGTH} INDEXED_STRIP_FUNCTION)
    configure_file(tests/test_rsx_indexed_strip.c.in test_rsx_indexed_strip.c @ONLY)
    add_executable(test_rsx_indexed_strip "${CMAKE_CURRENT_BINARY_DIR}/test_rsx_indexed_strip.c" libs/video/rsx_vertex_fetch.c)
    target_include_directories(test_rsx_indexed_strip PRIVATE include libs/video)
    add_test(NAME rsx_indexed_strip COMMAND test_rsx_indexed_strip)
    add_executable(test_memory_container tests/test_memory_container.c)
    if(NOT WIN32)
        find_package(Threads REQUIRED)
        target_link_libraries(test_memory_container PRIVATE Threads::Threads)
    endif()
    add_test(NAME memory_container_allocation COMMAND test_memory_container)
    add_executable(test_cpu_surface_hle tests/test_cpu_surface_hle.c runtime/memory/guest_read_sync.c)
    target_include_directories(test_cpu_surface_hle PRIVATE include libs/video runtime/ppu)
    add_test(NAME cpu_surface_hle COMMAND test_cpu_surface_hle)
endif()

add_executable(test_rsx_capture_schedule tests/rsx_capture_schedule.c)
target_include_directories(test_rsx_capture_schedule PRIVATE libs/video)
add_test(NAME rsx_capture_schedule COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_SOURCE_DIR}/tests/check_rsx_capture_schedule.py $<TARGET_FILE:test_rsx_capture_schedule>)
set_tests_properties(rsx_capture_schedule PROPERTIES TIMEOUT 30)
