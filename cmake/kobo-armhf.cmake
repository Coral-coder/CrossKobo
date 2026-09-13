# CMake toolchain file: 32-bit ARM hard-float, statically linked.
#
# Every Kobo made since the Touch runs a 32-bit armv7 hard-float userland,
# including the MediaTek MT8113 devices (Clara BW/Colour, Libra Colour,
# Elipsa 2E) whose Cortex-A53 cores are 64-bit capable but run a 32-bit
# rootfs. We link statically so the binary does not depend on the vintage of
# glibc/libstdc++ that happens to ship in the device firmware.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(CROSSKOBO_TRIPLE arm-linux-gnueabihf CACHE STRING "cross toolchain prefix")

find_program(CMAKE_C_COMPILER ${CROSSKOBO_TRIPLE}-gcc REQUIRED)
find_program(CMAKE_CXX_COMPILER ${CROSSKOBO_TRIPLE}-g++ REQUIRED)

# armv7-a + NEON covers i.MX6 (Cortex-A9) through MT8113 (Cortex-A53).
set(CROSSKOBO_ARCH_FLAGS "-march=armv7-a -mfpu=neon -mfloat-abi=hard -mthumb")
set(CMAKE_C_FLAGS_INIT "${CROSSKOBO_ARCH_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${CROSSKOBO_ARCH_FLAGS}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static -static-libgcc -static-libstdc++")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

set(CROSSKOBO_TARGET_KOBO ON)
