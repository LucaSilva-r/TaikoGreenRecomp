set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)

# Make pkg-config describe target libraries, never the x86-64 build container.
set(ENV{PKG_CONFIG_DIR} "")
set(ENV{PKG_CONFIG_LIBDIR}
    "/usr/lib/aarch64-linux-gnu/pkgconfig:/usr/share/pkgconfig")
set(ENV{PKG_CONFIG_SYSROOT_DIR} "/")

# Debian's cross toolchain keeps the Raspberry Pi OS-compatible target root
# here. Programs used during the build must still come from the build host.
# Cross-libc headers live below /usr/aarch64-linux-gnu, while Debian multiarch
# development packages (Wayland, ALSA, Vulkan) install below /usr/include and
# /usr/lib/aarch64-linux-gnu. The / root admits those target paths; the image
# intentionally contains no corresponding amd64 development packages.
set(CMAKE_FIND_ROOT_PATH /usr/aarch64-linux-gnu /)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
