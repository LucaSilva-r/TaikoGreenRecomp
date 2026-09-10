# Build the native decoder without upstream's platform-specific binary downloads.
set(TAIKO_VGMSTREAM_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/vgmstream-source" CACHE PATH "Pinned vgmstream source")
set(TAIKO_G719_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libg719-source" CACHE PATH "Pinned G.719 decoder source")
if(NOT EXISTS "${TAIKO_VGMSTREAM_SOURCE}/src/libvgmstream.h" OR NOT EXISTS "${TAIKO_G719_SOURCE}/g719.c")
    message(FATAL_ERROR "Nijiiro decoder sources missing. Run scripts/setup_nijiiro.sh.")
endif()
file(GLOB_RECURSE g719_sources CONFIGURE_DEPENDS "${TAIKO_G719_SOURCE}/reference_code/*.c")
add_library(taiko_g719 STATIC "${TAIKO_G719_SOURCE}/g719.c" ${g719_sources})
target_include_directories(taiko_g719 PRIVATE "${TAIKO_G719_SOURCE}/reference_code/include")
target_compile_definitions(taiko_g719 PRIVATE VAR_ARRAYS)
file(GLOB vgm_sources CONFIGURE_DEPENDS
    "${TAIKO_VGMSTREAM_SOURCE}/src/*.c"
    "${TAIKO_VGMSTREAM_SOURCE}/src/base/*.c"
    "${TAIKO_VGMSTREAM_SOURCE}/src/coding/*.c"
    "${TAIKO_VGMSTREAM_SOURCE}/src/coding/libs/*.c"
    "${TAIKO_VGMSTREAM_SOURCE}/src/layout/*.c"
    "${TAIKO_VGMSTREAM_SOURCE}/src/meta/*.c"
    "${TAIKO_VGMSTREAM_SOURCE}/src/util/*.c")
add_library(taiko_vgmstream STATIC ${vgm_sources})
target_include_directories(taiko_vgmstream PUBLIC "${TAIKO_VGMSTREAM_SOURCE}/src"
    PRIVATE "${TAIKO_VGMSTREAM_SOURCE}/ext_includes" "${TAIKO_G719_SOURCE}")
target_compile_definitions(taiko_vgmstream PRIVATE VGM_USE_G719)
target_link_libraries(taiko_vgmstream PRIVATE taiko_g719)
if(NOT WIN32)
    target_link_libraries(taiko_vgmstream PRIVATE m)
endif()
set_target_properties(taiko_g719 taiko_vgmstream PROPERTIES JOB_POOL_COMPILE taiko_compile)
# Every executable which compiles the common audio decoder needs this library.
get_property(nijiiro_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(target IN LISTS nijiiro_targets)
    get_target_property(sources ${target} SOURCES)
    if("${sources}" MATCHES "taiko_audio_decoder.cpp")
        target_link_libraries(${target} PRIVATE taiko_vgmstream)
    endif()
endforeach()
add_custom_command(TARGET taiko_boot POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:taiko_boot>/licenses"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${TAIKO_VGMSTREAM_SOURCE}/COPYING"
        "$<TARGET_FILE_DIR:taiko_boot>/licenses/vgmstream.txt"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${TAIKO_G719_SOURCE}/reference_code"
        "$<TARGET_FILE_DIR:taiko_boot>/licenses/libg719-reference"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${TAIKO_G719_SOURCE}/g719.c"
        "$<TARGET_FILE_DIR:taiko_boot>/licenses/libg719-wrapper.c"
    VERBATIM)
