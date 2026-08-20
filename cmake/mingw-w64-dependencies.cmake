include("${CMAKE_CURRENT_LIST_DIR}/../mingw-w64.cmake")

# Third-party target SDKs are installed into a repository-local prefix rather
# than the Fedora MinGW sysroot. This toolchain is only for building those
# dependencies; the game continues to use the strict root toolchain above.
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
