SET(CMAKE_SYSTEM_NAME    Windows) # Target system name
SET(CMAKE_SYSTEM_VERSION 1)

SET(PATH /home/enes/Sdks/llvm-mingw-20250402-ucrt-ubuntu-20.04-x86_64)
SET(CMAKE_C_COMPILER        "${PATH}/bin/x86_64-w64-mingw32-clang")
SET(CMAKE_CXX_COMPILER      "${PATH}/bin/x86_64-w64-mingw32-clang++")

# SET(PATH /home/enes/Sdks/llvm-mingw-20250402-ucrt-ubuntu-20.04-x86_64)
# SET(CMAKE_C_COMPILER        "/usr/bin/x86_64-w64-mingw32-gcc")
# SET(CMAKE_CXX_COMPILER      "/usr/bin/x86_64-w64-mingw32-g++")

SET(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
SET(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
SET(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
