# Runtime custom-song code is native. Python tools remain development oracles.
set(TAIKO_REALM_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/realm-core-source"
    CACHE PATH "Pinned Realm Core source (scripts/setup_realm.sh)")
if(NOT EXISTS "${TAIKO_REALM_SOURCE}/src/realm/db.hpp")
    message(FATAL_ERROR "Realm Core source missing. Run scripts/setup_realm.sh first.")
endif()
file(READ "${TAIKO_REALM_SOURCE}/src/realm/db.cpp" realm_db_source)
if(NOT realm_db_source MATCHES "options.allow_file_format_upgrade && backup.must_restore_from_backup")
    message(FATAL_ERROR "Realm read-only safeguard missing. Run scripts/setup_realm.sh.")
endif()
set(REALM_BUILD_LIB_ONLY ON CACHE BOOL "" FORCE)
set(REALM_ENABLE_SYNC OFF CACHE BOOL "" FORCE)
set(REALM_ENABLE_ENCRYPTION OFF CACHE BOOL "" FORCE)
set(REALM_ENABLE_GEOSPATIAL OFF CACHE BOOL "" FORCE)
set(REALM_APP_SERVICES OFF CACHE BOOL "" FORCE)
add_subdirectory("${TAIKO_REALM_SOURCE}" "realm-core" EXCLUDE_FROM_ALL)
if(MINGW)
    get_target_property(realm_libraries Storage INTERFACE_LINK_LIBRARIES)
    list(TRANSFORM realm_libraries REPLACE "^Version.lib$" "version")
    list(TRANSFORM realm_libraries REPLACE "^psapi.lib$" "psapi")
    list(APPEND realm_libraries bcrypt)
    set_target_properties(Storage PROPERTIES INTERFACE_LINK_LIBRARIES "${realm_libraries}")
endif()

set(TAIKO_CHART_SOURCES
    src/taiko_chart.cpp src/taiko_chart_tja.cpp src/taiko_chart_osu.cpp
    src/taiko_osu_lazer.cpp)
set(TAIKO_CHART_RECIPE "green-native-1")
foreach(source IN LISTS TAIKO_CHART_SOURCES)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${source}")
    file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/${source}" digest)
    string(APPEND TAIKO_CHART_RECIPE "${digest}")
endforeach()
foreach(source src/taiko_chart_internal.h tools/vendor/tja2fumen/hp_values.csv)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${source}")
    file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/${source}" digest)
    string(APPEND TAIKO_CHART_RECIPE "${digest}")
endforeach()
string(SHA256 TAIKO_CHART_RECIPE "${TAIKO_CHART_RECIPE}")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/tools/vendor/tja2fumen/hp_values.csv" TAIKO_HP_VALUES)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/taiko_chart_data.h.in"
               "${CMAKE_CURRENT_BINARY_DIR}/taiko_chart_data.h" @ONLY)
add_library(taiko_charts STATIC ${TAIKO_CHART_SOURCES})
target_include_directories(taiko_charts PRIVATE src "${CMAKE_CURRENT_BINARY_DIR}" "${TAIKO_MBEDTLS_ROOT}/include")
target_link_libraries(taiko_charts PRIVATE Storage "${TAIKO_MBEDTLS_CRYPTO}")
if(WIN32)
    target_compile_definitions(taiko_charts PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()
set_target_properties(taiko_charts PROPERTIES JOB_POOL_COMPILE taiko_compile)
set_target_properties(Storage PROPERTIES JOB_POOL_COMPILE taiko_compile)
if(NOT WIN32)
    find_package(Iconv REQUIRED)
    target_link_libraries(taiko_charts PRIVATE Iconv::Iconv)
endif()
target_link_libraries(taiko_boot PRIVATE taiko_charts)
add_custom_command(TARGET taiko_boot POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:taiko_boot>/licenses"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${TAIKO_REALM_SOURCE}/LICENSE"
            "$<TARGET_FILE_DIR:taiko_boot>/licenses/RealmCore.txt"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${TAIKO_REALM_SOURCE}/THIRD-PARTY-NOTICES"
            "$<TARGET_FILE_DIR:taiko_boot>/licenses/RealmCore-third-party.txt"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/tools/vendor/tja2fumen/LICENSE.txt"
            "$<TARGET_FILE_DIR:taiko_boot>/licenses/tja2fumen.txt"
    VERBATIM)
