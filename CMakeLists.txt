# Taiko no Tatsujin (System 357) -- PPU boot harness.
#
# Runs the lifted game code against ps3recomp's HLE runtime until it hits an
# unlifted function, an unimplemented import, or an lv2 syscall -- each logged,
# which is the whole point: the log is the to-do list.
#
# SDL3 provides the graphical host on Linux and Windows (SDL_GPU plus SDL3
# input). The null backends remain available for timing-accurate headless tests.
#
#   cmake -S . -B build -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=mingw-w64.cmake -DCMAKE_BUILD_TYPE=Release
#   cmake --build build
#   wine build/taiko_boot.exe game/EBOOT.elf
cmake_minimum_required(VERSION 3.20)
project(taiko_boot CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_C_STANDARD 17)

set(PS3RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/ps3recomp" CACHE PATH "ps3recomp repo root")

if(ANDROID)
    set(TAIKO_RSX_BACKEND_DEFAULT null)
    set(TAIKO_INPUT_BACKEND_DEFAULT null)
    set(TAIKO_AUDIO_BACKEND_DEFAULT null)
    set(TAIKO_EMBED_DEFAULT ON)
    set(TAIKO_ATRAC_DEFAULT OFF)
elseif(WIN32)
    set(TAIKO_RSX_BACKEND_DEFAULT sdl_gpu)
    set(TAIKO_INPUT_BACKEND_DEFAULT sdl3)
    set(TAIKO_AUDIO_BACKEND_DEFAULT sdl3)
    set(TAIKO_EMBED_DEFAULT ON)
    set(TAIKO_ATRAC_DEFAULT ON)
else()
    set(TAIKO_RSX_BACKEND_DEFAULT sdl_gpu)
    set(TAIKO_INPUT_BACKEND_DEFAULT sdl3)
    set(TAIKO_AUDIO_BACKEND_DEFAULT sdl3)
    set(TAIKO_EMBED_DEFAULT OFF)
    set(TAIKO_ATRAC_DEFAULT ON)
endif()

option(TAIKO_HEADLESS
    "Compatibility shortcut: force graphics, input, and audio backends to null"
    OFF)

if(DEFINED CACHE{TAIKO_RSX_BACKEND})
    set(_TAIKO_RSX_BACKEND_EXPLICIT TRUE)
else()
    set(_TAIKO_RSX_BACKEND_EXPLICIT FALSE)
endif()
if(DEFINED CACHE{TAIKO_INPUT_BACKEND})
    set(_TAIKO_INPUT_BACKEND_EXPLICIT TRUE)
else()
    set(_TAIKO_INPUT_BACKEND_EXPLICIT FALSE)
endif()
if(DEFINED CACHE{TAIKO_AUDIO_BACKEND})
    set(_TAIKO_AUDIO_BACKEND_EXPLICIT TRUE)
else()
    set(_TAIKO_AUDIO_BACKEND_EXPLICIT FALSE)
endif()

set(TAIKO_RSX_BACKEND "${TAIKO_RSX_BACKEND_DEFAULT}" CACHE STRING
    "RSX renderer: null or sdl_gpu")
set_property(CACHE TAIKO_RSX_BACKEND PROPERTY STRINGS null sdl_gpu)
set(TAIKO_INPUT_BACKEND "${TAIKO_INPUT_BACKEND_DEFAULT}" CACHE STRING
    "Host input: null or sdl3")
set_property(CACHE TAIKO_INPUT_BACKEND PROPERTY STRINGS null sdl3)
set(TAIKO_AUDIO_BACKEND "${TAIKO_AUDIO_BACKEND_DEFAULT}" CACHE STRING
    "Host audio: null, sdl3, or wasapi")
set_property(CACHE TAIKO_AUDIO_BACKEND PROPERTY STRINGS null sdl3 wasapi)

# Android applications enter native code through SDLActivity and therefore
# always need SDL's shared library/main glue, even for the null-backend smoke
# build.  Build the exact pinned SDL source in-tree with the NDK; host/Linux
# SDL archives are ABI- and libc-incompatible with Android.
if(ANDROID)
    if(DEFINED TAIKO_SDL_GPU_ROOT)
        set(_TAIKO_ANDROID_SDL_PREFIX "${TAIKO_SDL_GPU_ROOT}")
    else()
        set(_TAIKO_ANDROID_SDL_PREFIX
            "${CMAKE_SOURCE_DIR}/third_party/sdl-gpu-android-arm64")
    endif()
    set(TAIKO_ANDROID_SDL_SOURCE
        "${CMAKE_SOURCE_DIR}/third_party/sdl-gpu-rpi-arm64/.src/SDL3-3.4.10"
        CACHE PATH "Pinned SDL source used by the Android APK")
    if(EXISTS "${_TAIKO_ANDROID_SDL_PREFIX}/lib/cmake/SDL3/SDL3Config.cmake")
        # The graphical Android dependency bundle installs SDL alongside
        # ShaderCross.  Use that exact shared build so both libraries agree on
        # their SDL headers and ABI.
        set(_TAIKO_ANDROID_SDL_FROM_PREFIX TRUE)
    else()
        if(NOT EXISTS "${TAIKO_ANDROID_SDL_SOURCE}/CMakeLists.txt")
            message(FATAL_ERROR "Android SDL source is missing at ${TAIKO_ANDROID_SDL_SOURCE}")
        endif()
        set(_TAIKO_ANDROID_SDL_FROM_PREFIX FALSE)
        set(SDL_SHARED ON CACHE BOOL "Build Android SDL shared library" FORCE)
        set(SDL_STATIC OFF CACHE BOOL "Do not build a duplicate Android SDL archive" FORCE)
        set(SDL_TEST_LIBRARY OFF CACHE BOOL "Do not build SDL tests" FORCE)
        add_subdirectory("${TAIKO_ANDROID_SDL_SOURCE}"
            "${CMAKE_BINARY_DIR}/android-sdl" EXCLUDE_FROM_ALL)
    endif()
endif()

string(TOLOWER "${TAIKO_RSX_BACKEND}" TAIKO_RSX_BACKEND)
string(TOLOWER "${TAIKO_INPUT_BACKEND}" TAIKO_INPUT_BACKEND)
string(TOLOWER "${TAIKO_AUDIO_BACKEND}" TAIKO_AUDIO_BACKEND)
if(TAIKO_RSX_BACKEND STREQUAL "d3d12")
    message(STATUS "Migrating cached legacy RSX backend d3d12 -> sdl_gpu")
    set(TAIKO_RSX_BACKEND sdl_gpu CACHE STRING "RSX renderer" FORCE)
endif()
if(TAIKO_INPUT_BACKEND STREQUAL "win32")
    message(STATUS "Migrating cached legacy input backend win32 -> sdl3")
    set(TAIKO_INPUT_BACKEND sdl3 CACHE STRING "Host input" FORCE)
endif()
set(_TAIKO_RSX_VALUES null sdl_gpu)
set(_TAIKO_INPUT_VALUES null sdl3)
set(_TAIKO_AUDIO_VALUES null sdl3 wasapi)
if(NOT TAIKO_RSX_BACKEND IN_LIST _TAIKO_RSX_VALUES)
    message(FATAL_ERROR "Invalid TAIKO_RSX_BACKEND='${TAIKO_RSX_BACKEND}'")
endif()
if(NOT TAIKO_INPUT_BACKEND IN_LIST _TAIKO_INPUT_VALUES)
    message(FATAL_ERROR "Invalid TAIKO_INPUT_BACKEND='${TAIKO_INPUT_BACKEND}'")
endif()
if(NOT TAIKO_AUDIO_BACKEND IN_LIST _TAIKO_AUDIO_VALUES)
    message(FATAL_ERROR "Invalid TAIKO_AUDIO_BACKEND='${TAIKO_AUDIO_BACKEND}'")
endif()

if(TAIKO_HEADLESS)
    if((_TAIKO_RSX_BACKEND_EXPLICIT AND NOT TAIKO_RSX_BACKEND STREQUAL "null") OR
       (_TAIKO_INPUT_BACKEND_EXPLICIT AND NOT TAIKO_INPUT_BACKEND STREQUAL "null") OR
       (_TAIKO_AUDIO_BACKEND_EXPLICIT AND NOT TAIKO_AUDIO_BACKEND STREQUAL "null"))
        message(FATAL_ERROR
            "TAIKO_HEADLESS=ON contradicts an explicit non-null backend. "
            "Select null backends or configure with -DTAIKO_HEADLESS=OFF.")
    endif()
    set(TAIKO_RSX_BACKEND null CACHE STRING "RSX renderer" FORCE)
    set(TAIKO_INPUT_BACKEND null CACHE STRING "Host input" FORCE)
    set(TAIKO_AUDIO_BACKEND null CACHE STRING "Host audio" FORCE)
endif()

if(TAIKO_AUDIO_BACKEND STREQUAL "wasapi" AND NOT WIN32)
    message(FATAL_ERROR "TAIKO_AUDIO_BACKEND=wasapi requires a Windows target")
endif()

option(TAIKO_EMBED_PPU_IMAGE
    "Embed the EBOOT process image for a standalone USRDIR release"
    ${TAIKO_EMBED_DEFAULT})

option(TAIKO_INPROCESS_ATRAC
    "Decode ATRAC3plus inside the executable with a minimal static FFmpeg build"
    ${TAIKO_ATRAC_DEFAULT})

set(PS3RECOMP_RSX_BACKEND "${TAIKO_RSX_BACKEND}" CACHE STRING
    "ps3recomp RSX backend" FORCE)
set(PS3RECOMP_INPUT_BACKEND "${TAIKO_INPUT_BACKEND}" CACHE STRING
    "ps3recomp input backend" FORCE)
set(PS3RECOMP_AUDIO_BACKEND "${TAIKO_AUDIO_BACKEND}" CACHE STRING
    "ps3recomp audio backend" FORCE)
if(ANDROID)
    set(_TAIKO_FFMPEG_ROOT_DEFAULT
        "${CMAKE_SOURCE_DIR}/third_party/ffmpeg-android-arm64")
elseif(WIN32)
    set(_TAIKO_FFMPEG_ROOT_DEFAULT
        "${CMAKE_SOURCE_DIR}/third_party/ffmpeg-mingw")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
    set(_TAIKO_FFMPEG_ROOT_DEFAULT
        "${CMAKE_SOURCE_DIR}/third_party/ffmpeg-rpi-arm64")
else()
    set(_TAIKO_FFMPEG_ROOT_DEFAULT "")
endif()
set(TAIKO_FFMPEG_ROOT "${_TAIKO_FFMPEG_ROOT_DEFAULT}"
    CACHE PATH "Optional target prefix containing the minimal static FFmpeg build")

if(ANDROID)
    set(_TAIKO_SDL_GPU_ROOT_DEFAULT
        "${CMAKE_SOURCE_DIR}/third_party/sdl-gpu-android-arm64")
elseif(WIN32)
    set(_TAIKO_SDL_GPU_ROOT_DEFAULT "${CMAKE_SOURCE_DIR}/third_party/sdl-gpu-mingw")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
    set(_TAIKO_SDL_GPU_ROOT_DEFAULT "${CMAKE_SOURCE_DIR}/third_party/sdl-gpu-rpi-arm64")
else()
    set(_TAIKO_SDL_GPU_ROOT_DEFAULT "${CMAKE_SOURCE_DIR}/third_party/sdl-gpu-linux")
endif()
set(TAIKO_SDL_GPU_ROOT "${_TAIKO_SDL_GPU_ROOT_DEFAULT}"
    CACHE PATH "Platform SDL3/SDL_shadercross developer prefix")
if(_TAIKO_ANDROID_SDL_FROM_PREFIX OR
   TAIKO_RSX_BACKEND STREQUAL "sdl_gpu" OR
   TAIKO_INPUT_BACKEND STREQUAL "sdl3" OR
   TAIKO_AUDIO_BACKEND STREQUAL "sdl3")
    if(NOT EXISTS "${TAIKO_SDL_GPU_ROOT}/include/SDL3/SDL.h")
        message(FATAL_ERROR
            "SDL3 developer prefix is missing at ${TAIKO_SDL_GPU_ROOT}. Run the "
            "platform setup script or set TAIKO_SDL_GPU_ROOT.")
    endif()
    list(PREPEND CMAKE_PREFIX_PATH "${TAIKO_SDL_GPU_ROOT}")
    if(EXISTS "${TAIKO_SDL_GPU_ROOT}/lib/cmake/SDL3/SDL3Config.cmake")
        set(SDL3_DIR "${TAIKO_SDL_GPU_ROOT}/lib/cmake/SDL3"
            CACHE PATH "SDL3 package directory" FORCE)
    else()
        set(SDL3_DIR "${TAIKO_SDL_GPU_ROOT}/lib64/cmake/SDL3"
            CACHE PATH "SDL3 package directory" FORCE)
    endif()
    if(ANDROID)
        find_package(SDL3 3.4.10 CONFIG REQUIRED COMPONENTS SDL3-shared
            PATHS
                "${TAIKO_SDL_GPU_ROOT}/lib/cmake/SDL3"
                "${TAIKO_SDL_GPU_ROOT}/lib64/cmake/SDL3"
            NO_DEFAULT_PATH)
    else()
        find_package(SDL3 3.4.10 CONFIG REQUIRED COMPONENTS SDL3-static
            PATHS
                "${TAIKO_SDL_GPU_ROOT}/lib/cmake/SDL3"
                "${TAIKO_SDL_GPU_ROOT}/lib64/cmake/SDL3"
            NO_DEFAULT_PATH)
    endif()
endif()
if(TAIKO_RSX_BACKEND STREQUAL "sdl_gpu")
    if(NOT EXISTS "${TAIKO_SDL_GPU_ROOT}/include/SDL3_shadercross/SDL_shadercross.h")
        message(FATAL_ERROR
            "SDL_shadercross developer prefix is missing at ${TAIKO_SDL_GPU_ROOT}. "
            "Run the platform setup script or set TAIKO_SDL_GPU_ROOT.")
    endif()
    if(EXISTS "${TAIKO_SDL_GPU_ROOT}/lib/cmake/SDL3_shadercross/SDL3_shadercrossConfig.cmake")
        set(SDL3_shadercross_DIR
            "${TAIKO_SDL_GPU_ROOT}/lib/cmake/SDL3_shadercross"
            CACHE PATH "SDL_shadercross package directory" FORCE)
    else()
        set(SDL3_shadercross_DIR
            "${TAIKO_SDL_GPU_ROOT}/lib64/cmake/SDL3_shadercross"
            CACHE PATH "SDL_shadercross package directory" FORCE)
    endif()
    set(DirectXShaderCompiler_ROOT
        "${TAIKO_SDL_GPU_ROOT}/dxc-v1.8.2502" CACHE PATH
        "Pinned Microsoft DXC prefix used by SDL_shadercross" FORCE)
    if(WIN32)
        set(DirectXShaderCompiler_INCLUDE_PATH
            "${DirectXShaderCompiler_ROOT}/inc" CACHE PATH "" FORCE)
        set(DirectXShaderCompiler_dxcompiler_BINARY
            "${DirectXShaderCompiler_ROOT}/bin/x64/dxcompiler.dll"
            CACHE FILEPATH "" FORCE)
        set(DirectXShaderCompiler_dxcompiler_LIBRARY
            "${DirectXShaderCompiler_ROOT}/lib/x64/dxcompiler.lib"
            CACHE FILEPATH "" FORCE)
        set(DirectXShaderCompiler_dxil_BINARY
            "${DirectXShaderCompiler_ROOT}/bin/x64/dxil.dll"
            CACHE FILEPATH "" FORCE)
        # The DXC SDK is a target binary bundle outside the MinGW sysroot.
        # Allow shadercross's package module to resolve its headers/import lib.
        set(_TAIKO_FIND_ROOT_LIBRARY "${CMAKE_FIND_ROOT_PATH_MODE_LIBRARY}")
        set(_TAIKO_FIND_ROOT_INCLUDE "${CMAKE_FIND_ROOT_PATH_MODE_INCLUDE}")
        set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
        set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
    elseif(CMAKE_CROSSCOMPILING)
        # The pinned AArch64 DXC is staged inside the source tree, outside the
        # Debian target sysroot.  Give shadercross's package module exact target
        # paths so CMake does not prepend CMAKE_FIND_ROOT_PATH to them.
        set(DirectXShaderCompiler_INCLUDE_PATH
            "${DirectXShaderCompiler_ROOT}/include/dxc" CACHE PATH "" FORCE)
        set(DirectXShaderCompiler_dxcompiler_LIBRARY
            "${DirectXShaderCompiler_ROOT}/lib/libdxcompiler.so"
            CACHE FILEPATH "" FORCE)
        set(DirectXShaderCompiler_dxil_LIBRARY
            "${DirectXShaderCompiler_ROOT}/lib/libdxil.so"
            CACHE FILEPATH "" FORCE)
    endif()
    # SDL_shadercross's config resolves SPIRV-Cross through ordinary cached
    # package variables.  A build directory reused between native and MinGW
    # otherwise keeps the first platform's absolute library paths and can put
    # ELF archives into a PE link.  Pin every dependency to this target bundle.
    foreach(_TAIKO_SPIRV_COMPONENT c core cpp glsl hlsl msl reflect)
        set("spirv_cross_${_TAIKO_SPIRV_COMPONENT}_DIR"
            "${TAIKO_SDL_GPU_ROOT}/share/spirv_cross_${_TAIKO_SPIRV_COMPONENT}/cmake"
            CACHE PATH "SPIRV-Cross package directory" FORCE)
    endforeach()
    find_package(SDL3_shadercross CONFIG REQUIRED COMPONENTS SDL3_shadercross-static
        PATHS
            "${TAIKO_SDL_GPU_ROOT}/lib/cmake/SDL3_shadercross"
            "${TAIKO_SDL_GPU_ROOT}/lib64/cmake/SDL3_shadercross"
        NO_DEFAULT_PATH)
    if(WIN32)
        set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY "${_TAIKO_FIND_ROOT_LIBRARY}")
        set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE "${_TAIKO_FIND_ROOT_INCLUDE}")
    endif()
endif()

set(TAIKO_EMBEDDED_IMAGE_SOURCES)
if(TAIKO_EMBED_PPU_IMAGE)
    set(TAIKO_EBOOT_IMAGE "${CMAKE_CURRENT_SOURCE_DIR}/game/EBOOT.elf"
        CACHE FILEPATH "Patched PPU ELF used to generate the embedded process image")
    if(NOT EXISTS "${TAIKO_EBOOT_IMAGE}")
        message(FATAL_ERROR
            "TAIKO_EMBED_PPU_IMAGE=ON requires ${TAIKO_EBOOT_IMAGE}. "
            "Generate it from your own dump or configure with "
            "-DTAIKO_EMBED_PPU_IMAGE=OFF for the external-ELF developer mode.")
    endif()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(TAIKO_IMAGE_DIR "${CMAKE_CURRENT_BINARY_DIR}/embedded")
    set(TAIKO_IMAGE_BIN "${TAIKO_IMAGE_DIR}/taiko_ppu_image.bin")
    if(WIN32)
        set(TAIKO_IMAGE_GLUE "${TAIKO_IMAGE_DIR}/taiko_ppu_image.rc")
        set(TAIKO_IMAGE_GLUE_ARG --rc "${TAIKO_IMAGE_GLUE}")
    else()
        enable_language(ASM)
        set(TAIKO_IMAGE_GLUE "${TAIKO_IMAGE_DIR}/taiko_ppu_image.S")
        set(TAIKO_IMAGE_GLUE_ARG --asm "${TAIKO_IMAGE_GLUE}")
    endif()
    add_custom_command(
        OUTPUT "${TAIKO_IMAGE_BIN}" "${TAIKO_IMAGE_GLUE}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${TAIKO_IMAGE_DIR}"
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_ppu_image.py"
                "${TAIKO_EBOOT_IMAGE}"
                --output "${TAIKO_IMAGE_BIN}" ${TAIKO_IMAGE_GLUE_ARG}
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_ppu_image.py"
                "${TAIKO_EBOOT_IMAGE}"
        COMMENT "Generating embedded PPU process image"
        VERBATIM)
    set_source_files_properties("${TAIKO_IMAGE_GLUE}" PROPERTIES GENERATED TRUE)
    list(APPEND TAIKO_EMBEDDED_IMAGE_SOURCES "${TAIKO_IMAGE_GLUE}")
endif()

# The title's SPURS enqueue is direct-called generated code, so apply its
# publication-time native-job callback after every re-lift.  The patcher is
# idempotent and fails configuration if the expected instruction shape moves.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/fix_vertex_submit_snapshot.py"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    RESULT_VARIABLE TAIKO_VERTEX_SUBMIT_PATCH_RESULT
    OUTPUT_VARIABLE TAIKO_VERTEX_SUBMIT_PATCH_OUTPUT
    ERROR_VARIABLE TAIKO_VERTEX_SUBMIT_PATCH_ERROR
)
if(NOT TAIKO_VERTEX_SUBMIT_PATCH_RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to patch native SPURS publication hook: ${TAIKO_VERTEX_SUBMIT_PATCH_ERROR}")
endif()
message(STATUS "${TAIKO_VERTEX_SUBMIT_PATCH_OUTPUT}")

# Lifted game code (8 chunks, ~600k lines each) + generated HLE stubs/NID table.
file(GLOB RECOMP_SOURCES "src/recomp/ppu_recomp_*.cpp")
file(GLOB GEN_SOURCES    "src/gen/*.c" "src/gen/*.cpp")

# The 5 embedded SPU images, statically lifted to C by ps3recomp's spu_lifter
# (see tools/gen_spu_workloads.py for the registration glue). SPU code is not
# interpreted at runtime: each image is lifted ahead of time and registered by
# FNV-1a-64 fingerprint, so cellSpurs can dispatch a guest image to its lifted
# entry. Re-lift with:
#   for f in game/spu/*.elf; do b=$(basename "$f" .elf); \
#     python3 ps3recomp/tools/spu_lifter.py --auto-functions "$f" \
#       -o "src/spu_gen/$b" --symbol-prefix "${b}_"; done
#   python3 tools/gen_spu_workloads.py
#
# spu_0004 (bnusCore audio mixer, image_id 5) needs:
#   --extra-funcs 0x178,0x2680,0x10488,0x6320,0x6D68,0x7298,0x74A0,0x75A0,
#                 0x7600,0x7680,0x76F8,0x7900,0x7AD0
# The 0x6xxx/0x7xxx entries are the mixer's DSP module handlers, called through
# function pointers held in a 12-slot registry at LS 0x15310 (stride 28,
# {name, handler, ...}; lookup by FourCC at 0xCD50). Dumping that registry at
# runtime gives the authoritative list rather than rediscovering them one
# BRANCH-TO-0 at a time:
#   MIX =0x75A0  MUX =0x7680  PTCH=0x76F8  VOLP=0x7900  IIRF=0x6D68
#   REVB=0x7AD0  LHPF=0x74A0  DPL2=0x7298  MONO=0x7600  COMP=0x6320
# Auto-detection misses computed-call targets, and `spu_lookup` resolves exact
# function starts only, so an unresolved target lands in the branch-to-0
# handler, which sets STOPPED_BY_HALT and *returns* -- execution continues with
# corrupt state (observed: the mixer's 8192-byte output PUT retargeted to guest
# address 0, and the mixer silently skipping its whole voice list). 0x7AD0 is
# worse than unresolved: the detector classified it as data, so its block was
# never lifted at all, leaving the render gate at 0x7BD8 (which it falls into)
# unreachable. src/spu_gen/ is gitignored, so this list lives here or it is
# lost on the next re-lift. Find new ones by grepping a boot log for
# `BRANCH-TO-0`; each one resolved can expose the next.
#
# Separately fixed in spu_lifter.py itself: the continuation after a resumable
# stop-and-signal (stop 0x110 = sys_spu_thread_receive_event) was dropped,
# which cost 4 of the 5 images their syscall continuations. For receive_event
# that is the `rdch SPU_RdInMbox` sequence draining the kernel's 4-word reply;
# leaving it unread kept the mailbox non-empty, and the guest gates its reply
# on `rchcnt(SPU_RdInMbox) == 0`, so the SPU never answered the PPU.
file(GLOB SPU_GEN_SOURCES "src/spu_gen/*/spu_recomp.c" "src/spu_gen/*.c")

set(TAIKO_BOOT_SOURCES
    "${PS3RECOMP_DIR}/runtime/ppu/tests/boot_main.cpp"   # generic boot harness
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_loader.cpp"        # vm + ELF loader + run
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_hle.cpp"           # NID -> handler dispatch
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_sysprx.cpp"        # boot-critical CRT
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_fs.cpp"            # cellFs VFS
    "${PS3RECOMP_DIR}/runtime/host/frame_driver.cpp"     # shared vblank/FIFO clock
    "${CMAKE_CURRENT_SOURCE_DIR}/src/taiko_init.c"       # title id + unbuffered log
    "${CMAKE_CURRENT_SOURCE_DIR}/src/taiko_net.c"        # cellHttp NID aliases
    "${CMAKE_CURRENT_SOURCE_DIR}/src/taiko_tls.c"        # host TLS + online redirect
    "${CMAKE_CURRENT_SOURCE_DIR}/src/taiko_atrac.cpp"    # in-process ATRAC3plus -> game/SPU PCM ring
    "${CMAKE_CURRENT_SOURCE_DIR}/src/taiko_audio_offset.cpp" # persistent gameplay A/V latency calibration
    "${CMAKE_CURRENT_SOURCE_DIR}/src/taiko_audio_spu.c"  # raw lifted bnusCore mixer
    "${CMAKE_CURRENT_SOURCE_DIR}/src/taiko_sail.cpp"     # cellSail async callback shim
    "${CMAKE_CURRENT_SOURCE_DIR}/src/taiko_usio.cpp"     # virtual System 357 I/O board
    "${CMAKE_CURRENT_SOURCE_DIR}/src/taiko_card.c"       # virtual BanaPassport card
    "${CMAKE_CURRENT_SOURCE_DIR}/src/taiko_pairing.c"    # six-digit card pairing
    "${CMAKE_CURRENT_SOURCE_DIR}/src/taiko_overlay.c"    # on-screen pairing code
    "${CMAKE_CURRENT_SOURCE_DIR}/src/taiko_host_input.cpp" # portable title input latch
    "${CMAKE_CURRENT_SOURCE_DIR}/src/taiko_catalog.cpp"  # host view of installed stock songs
    "${CMAKE_CURRENT_SOURCE_DIR}/src/taiko_frontend.cpp" # host-owned Player Entry/login shell
    "${CMAKE_CURRENT_SOURCE_DIR}/src/taiko_lumen.cpp"    # title-local Lumen state fixes
    "${CMAKE_CURRENT_SOURCE_DIR}/src/taiko_entry_trace.cpp" # Player Entry/Song Select reversing trace
    "${CMAKE_CURRENT_SOURCE_DIR}/src/taiko_vertex_job.cpp" # native SPURS vertex skinning
    ${SPU_GEN_SOURCES}
    ${GEN_SOURCES}
    ${RECOMP_SOURCES}
    ${TAIKO_EMBEDDED_IMAGE_SOURCES}
)
if(ANDROID)
    add_library(taiko_boot SHARED ${TAIKO_BOOT_SOURCES})
else()
    add_executable(taiko_boot ${TAIKO_BOOT_SOURCES})
endif()
if(ANDROID)
    set_target_properties(taiko_boot PROPERTIES OUTPUT_NAME main)
endif()

if(TAIKO_EMBED_PPU_IMAGE)
    # Only the boot harness needs to know that resource 101 exists. Keeping the
    # definition source-local avoids recompiling ~300 MB of lifted game code
    # whenever release embedding is toggled.
    set_source_files_properties(
        "${PS3RECOMP_DIR}/runtime/ppu/tests/boot_main.cpp"
        PROPERTIES COMPILE_DEFINITIONS TAIKO_EMBED_PPU_IMAGE=1)
endif()

file(GLOB LIB_INC_DIRS LIST_DIRECTORIES true "${PS3RECOMP_DIR}/libs/*")
target_include_directories(taiko_boot PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/recomp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gen"
    "${PS3RECOMP_DIR}/runtime/ppu"
    "${PS3RECOMP_DIR}/runtime/spu"
    "${PS3RECOMP_DIR}/runtime/prx"
    "${PS3RECOMP_DIR}/include"
    ${LIB_INC_DIRS}
)

if(TAIKO_INPROCESS_ATRAC)
    if(TAIKO_FFMPEG_ROOT)
        set(TAIKO_FFMPEG_LIBS
            "${TAIKO_FFMPEG_ROOT}/lib/libavformat.a"
            "${TAIKO_FFMPEG_ROOT}/lib/libavcodec.a"
            "${TAIKO_FFMPEG_ROOT}/lib/libswresample.a"
            "${TAIKO_FFMPEG_ROOT}/lib/libavutil.a")
        if(NOT EXISTS "${TAIKO_FFMPEG_ROOT}/include/libavcodec/avcodec.h" OR
           NOT EXISTS "${TAIKO_FFMPEG_ROOT}/lib/libavcodec.a")
            message(FATAL_ERROR
                "TAIKO_INPROCESS_ATRAC=ON requires the minimal target FFmpeg "
                "build at ${TAIKO_FFMPEG_ROOT}.")
        endif()
        set(TAIKO_FFMPEG_INCLUDE_DIRS "${TAIKO_FFMPEG_ROOT}/include")
    else()
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(TAIKO_FFMPEG REQUIRED IMPORTED_TARGET
            libavformat libavcodec libswresample libavutil)
    endif()
    # Keep these source-local. Adding them to the target changes the command
    # line for ~300 MB of generated lifted code and needlessly recompiles every
    # guest chunk whenever the decoder prefix changes.
    set_source_files_properties(
        "${CMAKE_CURRENT_SOURCE_DIR}/src/taiko_atrac.cpp"
        PROPERTIES
            COMPILE_DEFINITIONS TAIKO_HAVE_FFMPEG=1
            INCLUDE_DIRECTORIES "${TAIKO_FFMPEG_INCLUDE_DIRS}")
endif()

# Static mbedTLS (scripts/build_mbedtls.sh) backs the online path: the arcade
# services are HTTPS and the title's own cellSsl is a lifecycle shell. Vendored
# rather than taken from the host so Linux and MinGW run one code path.
if(ANDROID)
    set(_TAIKO_MBEDTLS_ROOT_DEFAULT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/mbedtls-android-arm64")
elseif(WIN32)
    set(_TAIKO_MBEDTLS_ROOT_DEFAULT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/mbedtls-mingw")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
    set(_TAIKO_MBEDTLS_ROOT_DEFAULT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/mbedtls-rpi-arm64")
else()
    set(_TAIKO_MBEDTLS_ROOT_DEFAULT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/mbedtls-linux")
endif()
set(TAIKO_MBEDTLS_ROOT "${_TAIKO_MBEDTLS_ROOT_DEFAULT}" CACHE PATH
    "Target mbedTLS installation prefix")
# Library discoveries are absolute paths cached by CMake. A build directory
# reused for a different target must not retain a host/glibc archive.
unset(TAIKO_MBEDTLS_SSL CACHE)
unset(TAIKO_MBEDTLS_X509 CACHE)
unset(TAIKO_MBEDTLS_CRYPTO CACHE)
find_library(TAIKO_MBEDTLS_SSL    mbedtls    PATHS "${TAIKO_MBEDTLS_ROOT}/lib" "${TAIKO_MBEDTLS_ROOT}/lib64" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
find_library(TAIKO_MBEDTLS_X509   mbedx509   PATHS "${TAIKO_MBEDTLS_ROOT}/lib" "${TAIKO_MBEDTLS_ROOT}/lib64" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
find_library(TAIKO_MBEDTLS_CRYPTO mbedcrypto PATHS "${TAIKO_MBEDTLS_ROOT}/lib" "${TAIKO_MBEDTLS_ROOT}/lib64" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
if(NOT TAIKO_MBEDTLS_SSL OR NOT TAIKO_MBEDTLS_X509 OR NOT TAIKO_MBEDTLS_CRYPTO)
    message(FATAL_ERROR
        "mbedTLS not found under ${TAIKO_MBEDTLS_ROOT}. Run scripts/build_mbedtls.sh, "
        "then configure again.")
endif()
set(TAIKO_MBEDTLS_LIBS "${TAIKO_MBEDTLS_SSL}" "${TAIKO_MBEDTLS_X509}" "${TAIKO_MBEDTLS_CRYPTO}")
# Source-local include dir: adding it to the target would rebuild every lifted
# chunk. taiko_tls.c is the only file that talks to mbedTLS directly.
set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/taiko_tls.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/taiko_net.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/taiko_pairing.c"
    PROPERTIES INCLUDE_DIRECTORIES "${TAIKO_MBEDTLS_ROOT}/include")
target_link_libraries(taiko_boot PRIVATE ${TAIKO_MBEDTLS_LIBS})
if(WIN32)
    target_link_libraries(taiko_boot PRIVATE bcrypt)
endif()

# Static FreeType (scripts/build_freetype.sh) rasterizes the overlay text in the
# game's own font. Vendored for the same reason as mbedTLS: one code path for
# both targets, and no extra system package for the MinGW build.
if(ANDROID)
    set(_TAIKO_FREETYPE_ROOT_DEFAULT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/freetype-android-arm64")
elseif(WIN32)
    set(_TAIKO_FREETYPE_ROOT_DEFAULT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/freetype-mingw")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
    set(_TAIKO_FREETYPE_ROOT_DEFAULT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/freetype-rpi-arm64")
else()
    set(_TAIKO_FREETYPE_ROOT_DEFAULT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/freetype-linux")
endif()
set(TAIKO_FREETYPE_ROOT "${_TAIKO_FREETYPE_ROOT_DEFAULT}" CACHE PATH
    "Target FreeType installation prefix")
unset(TAIKO_FREETYPE_LIB CACHE)
find_library(TAIKO_FREETYPE_LIB freetype
    PATHS "${TAIKO_FREETYPE_ROOT}/lib" "${TAIKO_FREETYPE_ROOT}/lib64"
    NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
if(NOT TAIKO_FREETYPE_LIB)
    message(FATAL_ERROR
        "FreeType not found under ${TAIKO_FREETYPE_ROOT}. Run "
        "scripts/build_freetype.sh, then configure again.")
endif()
set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/taiko_overlay.c"
    PROPERTIES INCLUDE_DIRECTORIES "${TAIKO_FREETYPE_ROOT}/include/freetype2")
target_link_libraries(taiko_boot PRIVATE "${TAIKO_FREETYPE_LIB}")

# Embed the complete tracked UI font so future UI such as custom-song metadata
# retains every glyph and the executable is a drop-in with no font file beside
# it. A build configured with an absent/empty path falls back
# to loading TAIKO_OVERLAY_FONT (or fonts/font.ttf) at runtime.
set(TAIKO_OVERLAY_FONT_FILE "${CMAKE_CURRENT_SOURCE_DIR}/fonts/font.ttf"
    CACHE FILEPATH "Font embedded into the executable for the pairing overlay")
if(EXISTS "${TAIKO_OVERLAY_FONT_FILE}")
    set(TAIKO_OVERLAY_FONT_C "${CMAKE_BINARY_DIR}/taiko_overlay_font.c")
    execute_process(
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_font.py"
                --font "${TAIKO_OVERLAY_FONT_FILE}"
                --out "${TAIKO_OVERLAY_FONT_C}"
        RESULT_VARIABLE taiko_embed_font_result
        OUTPUT_VARIABLE taiko_embed_font_output
        ERROR_VARIABLE taiko_embed_font_output)
    if(NOT taiko_embed_font_result EQUAL 0)
        message(FATAL_ERROR "embed_font.py failed:\n${taiko_embed_font_output}")
    endif()
    message(STATUS "Overlay font: ${taiko_embed_font_output}")
    target_sources(taiko_boot PRIVATE "${TAIKO_OVERLAY_FONT_C}")
    target_compile_definitions(taiko_boot PRIVATE TAIKO_OVERLAY_FONT_EMBEDDED)
else()
    message(STATUS "Overlay font: ${TAIKO_OVERLAY_FONT_FILE} absent, "
                   "the overlay will load a font at runtime")
endif()

# Build the runtime from source rather than expecting a prebuilt .lib -- we are
# cross-compiling, so there is nothing prebuilt to find.
add_subdirectory("${PS3RECOMP_DIR}" "${CMAKE_BINARY_DIR}/ps3recomp_sdk" EXCLUDE_FROM_ALL)
if(TAIKO_RSX_BACKEND STREQUAL "sdl_gpu")
    target_link_libraries(ps3recomp_runtime PUBLIC
        SDL3_shadercross::SDL3_shadercross-static)
endif()
target_link_libraries(taiko_boot PRIVATE ps3recomp_runtime)
if(ANDROID)
    target_link_libraries(taiko_boot PRIVATE SDL3::SDL3-shared)
endif()
if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(taiko_boot PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
endif()

if(WIN32 AND TAIKO_RSX_BACKEND STREQUAL "sdl_gpu")
    set(TAIKO_DXC_RUNTIME_DIR
        "${TAIKO_SDL_GPU_ROOT}/dxc-v1.8.2502/bin/x64")
    function(taiko_copy_dxc_runtime target)
        foreach(dll dxcompiler.dll dxil.dll)
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${TAIKO_DXC_RUNTIME_DIR}/${dll}"
                    "$<TARGET_FILE_DIR:${target}>/${dll}"
                VERBATIM)
        endforeach()
    endfunction()
    taiko_copy_dxc_runtime(taiko_boot)
endif()
if(TAIKO_INPROCESS_ATRAC)
    if(TAIKO_FFMPEG_ROOT)
        target_link_libraries(taiko_boot PRIVATE ${TAIKO_FFMPEG_LIBS})
        if(WIN32)
            target_link_libraries(taiko_boot PRIVATE ole32 user32 atomic)
        else()
            target_link_libraries(taiko_boot PRIVATE Threads::Threads m
                ${CMAKE_DL_LIBS})
        endif()
    else()
        target_link_libraries(taiko_boot PRIVATE PkgConfig::TAIKO_FFMPEG)
    endif()
endif()
if(WIN32)
    # Static archives must precede the system libraries that satisfy them.
    # In particular FFmpeg's avutil uses BCryptGenRandom.
    target_link_libraries(taiko_boot PRIVATE
        ws2_32 bcrypt winmm) # winmm: timeBeginPeriod, for sub-15ms event waits
endif()

# Each guest call is a host call, so guest recursion nests on the host stack.
# 256 MB reserve for the main thread; spawned threads get theirs in
# sys_ppu_thread_create.
if(WIN32)
    target_link_options(taiko_boot PRIVATE -Wl,--stack,268435456
        # Static libgcc/libstdc++/winpthread: the mingw DLLs live in the Fedora
        # sysroot, not next to the .exe, so a dynamic link dies at loader_init
        # under Wine with status c0000135.
        -static -static-libgcc -static-libstdc++)
endif()

# Keep the development target name used by the existing launcher and debugging
# tools, while providing a clearly named artifact for copying into USRDIR.
if(TAIKO_EMBED_PPU_IMAGE)
    add_custom_target(taiko_release
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "$<TARGET_FILE:taiko_boot>"
                "${CMAKE_CURRENT_BINARY_DIR}/TaikoRecomp.exe"
        DEPENDS taiko_boot
        COMMENT "Creating standalone USRDIR release executable"
        VERBATIM)
endif()

# ~300 MB of generated C++ in 7 TUs. -O0 was the original choice to keep build
# time survivable, but it is not free: the guest's own memset/memcpy loops run
# one byte per lifted iteration through vm_read8/vm_write8, and unoptimized that
# turns a 912 KB clear during system_InitParam into a visible stall (it reads as
# a hang -- HOTREAD8 even reports the constant source byte as a "spin"). -O1
# keeps the build tractable while letting the compiler collapse those loops.
#
# -O1 was still not enough. The attract/LOADING path fills far more than
# system_InitParam does -- measured ~60 M byte-iterations in 20 s, i.e. ~3 MB/s,
# so multi-megabyte fills stall for tens of seconds each and read as the
# intermittent "stuck on LOADING" hang. Raising to -O2 buys throughput at the
# cost of build time; drop back to -O1 if compiling becomes intolerable.
# Lifted VMX code intentionally views each vector-register union through byte,
# halfword, word, float, and double pointers.  GCC's strict-aliasing optimiser
# can otherwise treat those accesses as independent: on AArch64 this caused
# func_0051F8EC's matrix inverse to return its input unchanged while the same
# generated source worked on x86-64.  ps3recomp's runtime and project template
# already require this flag for their endian/register unions; the lifted TUs
# need it for the same reason.
set_source_files_properties(${RECOMP_SOURCES}
    PROPERTIES COMPILE_OPTIONS "-O2;-w;-fno-strict-aliasing")
if(MINGW)
    # MinGW's scalar fmaf implementation is a sizeable software routine. The
    # lifted audio mixer executes four fmaf calls for every SPU FMA instruction,
    # which leaves the Windows producer unable to sustain its required 48 kHz
    # block rate. The Windows build therefore requires an FMA-capable x86-64
    # host for the lifted SPU units, letting GCC fold those calls into native
    # instructions. Keep the flag source-local so the PPU lift and portable
    # runtime remain baseline.
    set_source_files_properties(${SPU_GEN_SOURCES}
        PROPERTIES COMPILE_OPTIONS "-O3;-w;-mfma")
else()
    set_source_files_properties(${SPU_GEN_SOURCES}
        PROPERTIES COMPILE_OPTIONS "-O2;-w")
endif()

# Cap build parallelism. Each lifted chunk is a 600k-800k line TU, and GCC at -O2
# needs SEVERAL GIGABYTES of RAM for one of them. Ninja defaults to one job per
# core, so on a 16-core box that is 16 concurrent multi-GB compiles -- which
# exhausts system RAM and wedges the machine rather than merely being slow.
# Budget roughly 3 GB per job when raising this.
set(TAIKO_COMPILE_JOBS 4 CACHE STRING "Concurrent compiles of the lifted chunks (~3 GB RAM each)")
set_property(GLOBAL PROPERTY JOB_POOLS taiko_compile=${TAIKO_COMPILE_JOBS} taiko_link=1)
set_target_properties(taiko_boot PROPERTIES
    JOB_POOL_COMPILE taiko_compile
    JOB_POOL_LINK    taiko_link)
if(NOT WIN32 AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$" AND
   TAIKO_RSX_BACKEND STREQUAL "sdl_gpu")
    # The Pi artifact is deployed as bundle/{taiko_boot,lib/libdxcompiler.so}.
    # Do not bake the cross-build container's /src path into the executable.
    set_target_properties(taiko_boot PROPERTIES
        BUILD_WITH_INSTALL_RPATH TRUE
        INSTALL_RPATH "$ORIGIN/lib")
endif()

include(CTest)
if(BUILD_TESTING AND TAIKO_RSX_BACKEND STREQUAL "null" AND
   TAIKO_INPUT_BACKEND STREQUAL "null" AND
   TAIKO_AUDIO_BACKEND STREQUAL "null" AND NOT WIN32)
    add_executable(taiko_linux_headless_smoke
        "tests/linux_headless_smoke.c")
    target_include_directories(taiko_linux_headless_smoke PRIVATE
        "${PS3RECOMP_DIR}/libs/audio"
        "${PS3RECOMP_DIR}/libs/input"
        "${PS3RECOMP_DIR}/libs/video")
    target_link_libraries(taiko_linux_headless_smoke PRIVATE ps3recomp_runtime)
    add_test(NAME taiko_linux_headless_smoke COMMAND taiko_linux_headless_smoke)
endif()

if(BUILD_TESTING)
    add_executable(ppu_thread_lifecycle_tests
        "tests/ppu_thread_lifecycle_tests.c")
    target_include_directories(ppu_thread_lifecycle_tests PRIVATE
        "${PS3RECOMP_DIR}/runtime/syscalls")
    add_test(NAME ppu_thread_lifecycle_tests
        COMMAND ppu_thread_lifecycle_tests)

    # Semantics of the host mutex behind sys_lwmutex: recursive ownership,
    # FIFO fairness, cross-thread release, and the timed-out-acquire fallback.
    add_executable(fair_mutex_tests "tests/fair_mutex_tests.cpp")
    target_include_directories(fair_mutex_tests PRIVATE
        "${PS3RECOMP_DIR}/runtime/ppu")
    find_package(Threads REQUIRED)
    target_link_libraries(fair_mutex_tests PRIVATE Threads::Threads)
    add_test(NAME fair_mutex_tests COMMAND fair_mutex_tests)
endif()

if(BUILD_TESTING AND NOT WIN32)
    add_executable(taiko_audio_header_tests
        "tests/taiko_audio_header_tests.c")
    target_include_directories(taiko_audio_header_tests PRIVATE
        "${PS3RECOMP_DIR}/runtime/spu"
        "${PS3RECOMP_DIR}/include")
    add_test(NAME taiko_audio_header_tests COMMAND taiko_audio_header_tests)

    add_executable(rsx_render_batch_tests
        "tests/rsx_render_batch_tests.c"
        "${PS3RECOMP_DIR}/libs/video/rsx_render_batch.c"
        "${PS3RECOMP_DIR}/libs/video/rsx_batch_io.c"
        "${PS3RECOMP_DIR}/libs/video/rsx_vp_decompiler.c")
    target_include_directories(rsx_render_batch_tests PRIVATE
        "${PS3RECOMP_DIR}/include"
        "${PS3RECOMP_DIR}/libs/video")
    add_test(NAME rsx_render_batch_tests COMMAND rsx_render_batch_tests)

    # Guest big-endian pollfd/fd_set/timeval marshalling for sys_net.
    add_executable(net_select_tests "tests/net_select_tests.c" "src/taiko_tls.c"
        "${PS3RECOMP_DIR}/libs/network/cellHttpUtil.c")
    target_include_directories(net_select_tests PRIVATE
        "${PS3RECOMP_DIR}/libs/network"
        "${PS3RECOMP_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${TAIKO_MBEDTLS_ROOT}/include")
    target_link_libraries(net_select_tests PRIVATE ${TAIKO_MBEDTLS_LIBS})
    if(NOT WIN32)
        find_package(Threads REQUIRED)
        target_link_libraries(net_select_tests PRIVATE Threads::Threads)
    endif()
    add_test(NAME net_select_tests COMMAND net_select_tests)

    add_executable(taiko_host_input_tests
        "tests/taiko_host_input_tests.cpp"
        "src/taiko_host_input.cpp")
    target_include_directories(taiko_host_input_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src")
    add_test(NAME taiko_host_input_tests COMMAND taiko_host_input_tests)

    add_executable(taiko_audio_offset_tests
        "tests/taiko_audio_offset_tests.cpp"
        "src/taiko_audio_offset.cpp")
    target_include_directories(taiko_audio_offset_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src")
    add_test(NAME taiko_audio_offset_tests COMMAND taiko_audio_offset_tests)
endif()

if(BUILD_TESTING AND TAIKO_AUDIO_BACKEND STREQUAL "sdl3" AND
   NOT WIN32 AND NOT CMAKE_CROSSCOMPILING)
    add_executable(taiko_sdl3_audio_sink_smoke
        "tests/sdl3_audio_sink_smoke.c"
        "${PS3RECOMP_DIR}/libs/audio/audio_sink.c"
        "${PS3RECOMP_DIR}/runtime/host/host_platform.c"
        "${PS3RECOMP_DIR}/runtime/host/host_sdl.c")
    target_compile_definitions(taiko_sdl3_audio_sink_smoke PRIVATE
        PS3RECOMP_AUDIO_BACKEND_SDL3=1)
    target_include_directories(taiko_sdl3_audio_sink_smoke PRIVATE
        "${PS3RECOMP_DIR}/include"
        "${PS3RECOMP_DIR}/libs/audio")
    target_link_libraries(taiko_sdl3_audio_sink_smoke PRIVATE
        SDL3::SDL3-static Threads::Threads ${CMAKE_DL_LIBS})
    add_test(NAME taiko_sdl3_audio_sink_smoke
        COMMAND taiko_sdl3_audio_sink_smoke)
    set_tests_properties(taiko_sdl3_audio_sink_smoke PROPERTIES
        ENVIRONMENT "SDL_AUDIODRIVER=dummy"
        SKIP_RETURN_CODE 77 TIMEOUT 5)
endif()

if(TAIKO_RSX_BACKEND STREQUAL "sdl_gpu" AND NOT ANDROID)
    add_executable(rsx_replay
        "tools/rsx_replay.c"
        "src/taiko_host_input.cpp"
        "${PS3RECOMP_DIR}/runtime/host/host_sdl.c"
        "${PS3RECOMP_DIR}/libs/video/rsx_sdl_gpu_backend.c"
        "${PS3RECOMP_DIR}/libs/video/rsx_kms_present.c"
        "${PS3RECOMP_DIR}/libs/video/rsx_render_batch.c"
        "${PS3RECOMP_DIR}/libs/video/rsx_batch_io.c"
        "${PS3RECOMP_DIR}/libs/video/rsx_vp_decompiler.c"
        "${PS3RECOMP_DIR}/libs/video/rsx_fp_decompiler.c")
    target_compile_definitions(rsx_replay PRIVATE
        PS3RECOMP_RSX_BACKEND_SDL_GPU=1
        RSX_SDL_REPLAY_STANDALONE=1)
    target_include_directories(rsx_replay PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${PS3RECOMP_DIR}/include"
        "${PS3RECOMP_DIR}/libs/video")
    target_link_libraries(rsx_replay PRIVATE
        SDL3::SDL3-static
        SDL3_shadercross::SDL3_shadercross-static)
    # Same KMS presentation path as the game; see ps3recomp/CMakeLists.txt.
    if(UNIX AND NOT APPLE)
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(TAIKO_LIBDRM QUIET libdrm)
        endif()
    endif()
    if(TAIKO_LIBDRM_FOUND)
        target_compile_definitions(rsx_replay PRIVATE RSX_SDL_KMS_PRESENT=1)
        target_include_directories(rsx_replay PRIVATE ${TAIKO_LIBDRM_INCLUDE_DIRS})
        target_link_libraries(rsx_replay PRIVATE ${TAIKO_LIBDRM_LIBRARIES})
    endif()
    if(WIN32)
        target_link_options(rsx_replay PRIVATE
            -static -static-libgcc -static-libstdc++)
        taiko_copy_dxc_runtime(rsx_replay)
    endif()
endif()

message(STATUS "Taiko host backends: RSX=${TAIKO_RSX_BACKEND}, input=${TAIKO_INPUT_BACKEND}, audio=${TAIKO_AUDIO_BACKEND}")
