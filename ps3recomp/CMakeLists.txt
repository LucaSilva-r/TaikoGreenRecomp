cmake_minimum_required(VERSION 3.20)
project(ps3recomp_runtime
    VERSION 0.3.0
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
set(PS3RECOMP_RSX_BACKEND "null" CACHE STRING "RSX backend: null or sdl_gpu")
set(PS3RECOMP_INPUT_BACKEND "null" CACHE STRING "Input backend: null or sdl3")
set(PS3RECOMP_AUDIO_BACKEND "sdl3" CACHE STRING "Audio backend: null, sdl3, or wasapi")
set_property(CACHE PS3RECOMP_RSX_BACKEND PROPERTY STRINGS null sdl_gpu)
set_property(CACHE PS3RECOMP_INPUT_BACKEND PROPERTY STRINGS null sdl3)
set_property(CACHE PS3RECOMP_AUDIO_BACKEND PROPERTY STRINGS null sdl3 wasapi)
if(NOT PS3RECOMP_RSX_BACKEND STREQUAL "null" AND
   NOT PS3RECOMP_RSX_BACKEND STREQUAL "sdl_gpu")
    message(FATAL_ERROR
        "Unsupported PS3RECOMP_RSX_BACKEND='${PS3RECOMP_RSX_BACKEND}'")
endif()
if(NOT PS3RECOMP_INPUT_BACKEND STREQUAL "null" AND
   NOT PS3RECOMP_INPUT_BACKEND STREQUAL "sdl3")
    message(FATAL_ERROR
        "Unsupported PS3RECOMP_INPUT_BACKEND='${PS3RECOMP_INPUT_BACKEND}'")
endif()
if(NOT PS3RECOMP_AUDIO_BACKEND STREQUAL "null" AND
   NOT PS3RECOMP_AUDIO_BACKEND STREQUAL "sdl3" AND
   NOT PS3RECOMP_AUDIO_BACKEND STREQUAL "wasapi")
    message(FATAL_ERROR
        "Unsupported PS3RECOMP_AUDIO_BACKEND='${PS3RECOMP_AUDIO_BACKEND}'")
endif()
if(PS3RECOMP_AUDIO_BACKEND STREQUAL "wasapi" AND NOT WIN32)
    message(FATAL_ERROR "PS3RECOMP_AUDIO_BACKEND=wasapi requires Windows")
endif()

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
)
# Exclude test harnesses (e.g. libs/video/tests/*): they define their own main().
list(FILTER HLE_SOURCES EXCLUDE REGEX "/tests/")

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
list(FILTER RUNTIME_SOURCES EXCLUDE REGEX "/tests/|/runtime/ppu/|/runtime/host/")

# ---------------------------------------------------------------------------
# Static library
# ---------------------------------------------------------------------------
add_library(ps3recomp_runtime STATIC
    ${HLE_SOURCES}
    ${RUNTIME_SOURCES}
    "runtime/host/host_platform.c"
    "runtime/host/host_sdl.c"
)

target_include_directories(ps3recomp_runtime PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

string(TOUPPER "${PS3RECOMP_RSX_BACKEND}" _PS3RECOMP_RSX_BACKEND)
string(TOUPPER "${PS3RECOMP_INPUT_BACKEND}" _PS3RECOMP_INPUT_BACKEND)
string(TOUPPER "${PS3RECOMP_AUDIO_BACKEND}" _PS3RECOMP_AUDIO_BACKEND)
target_compile_definitions(ps3recomp_runtime PUBLIC
    "PS3RECOMP_RSX_BACKEND_${_PS3RECOMP_RSX_BACKEND}=1"
    "PS3RECOMP_INPUT_BACKEND_${_PS3RECOMP_INPUT_BACKEND}=1"
    "PS3RECOMP_AUDIO_BACKEND_${_PS3RECOMP_AUDIO_BACKEND}=1")

# Presentation straight to KMS needs libdrm. It is only meaningful on a Linux
# appliance that owns the display, so a missing libdrm simply leaves
# rsx_kms_present.c compiling to stubs and the windowed path in charge.
if(UNIX AND NOT APPLE)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(TAIKO_LIBDRM QUIET libdrm)
    endif()
endif()
if(TAIKO_LIBDRM_FOUND)
    target_compile_definitions(ps3recomp_runtime PRIVATE RSX_SDL_KMS_PRESENT=1)
    target_include_directories(ps3recomp_runtime PRIVATE ${TAIKO_LIBDRM_INCLUDE_DIRS})
    target_link_libraries(ps3recomp_runtime PUBLIC ${TAIKO_LIBDRM_LIBRARIES})
    message(STATUS "KMS presentation enabled (libdrm ${TAIKO_LIBDRM_VERSION})")
endif()

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

if(PS3RECOMP_RSX_BACKEND STREQUAL "sdl_gpu" OR
   PS3RECOMP_INPUT_BACKEND STREQUAL "sdl3" OR
   PS3RECOMP_AUDIO_BACKEND STREQUAL "sdl3")
    if(NOT TARGET SDL3::SDL3-static AND NOT TARGET SDL3::SDL3)
        find_package(SDL3 3.2 CONFIG REQUIRED)
    endif()
    if(TARGET SDL3::SDL3-static)
        target_link_libraries(ps3recomp_runtime PUBLIC SDL3::SDL3-static)
    else()
        target_link_libraries(ps3recomp_runtime PUBLIC SDL3::SDL3)
    endif()
endif()

# Platform-specific link libraries
if(WIN32)
    target_link_libraries(ps3recomp_runtime PRIVATE ws2_32 bcrypt)
    if(PS3RECOMP_AUDIO_BACKEND STREQUAL "wasapi")
        target_link_libraries(ps3recomp_runtime PRIVATE ole32)
    endif()
else()
    find_package(Threads REQUIRED)
    target_link_libraries(ps3recomp_runtime PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
endif()

# ---------------------------------------------------------------------------
# Compiler flags
# ---------------------------------------------------------------------------
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(ps3recomp_runtime PRIVATE
        -Wall -Wextra -Wno-unused-parameter
        -fno-strict-aliasing
    )
elseif(MSVC)
    target_compile_options(ps3recomp_runtime PRIVATE
        /W3 /wd4100 /wd4244 /wd4267 /wd4996
        /Zc:__cplusplus
        /experimental:c11atomics   # Enable C11 <stdatomic.h> support
    )
    target_compile_definitions(ps3recomp_runtime PRIVATE
        _CRT_SECURE_NO_WARNINGS
    )
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
