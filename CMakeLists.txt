cmake_minimum_required(VERSION 4.0)

set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS 1) # for clangd (single compile_commands.json for all subprojects)

project(game-001 CXX)

# ── Shared compile flags ────────────────────────────────────────────────────
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fdiagnostics-color -Wall -Wextra -Wpedantic -pedantic-errors")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-nullability-completeness -Wno-nullability-extension") # libzip / clang nullability
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-initializer-overrides")                               # struct params
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-c99-designator")                                    # nested/array/mixed designators (P0963)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DCGLM_FORCE_DEPTH_ZERO_TO_ONE=1")                        # cglm Vulkan convention
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DKHRONOS_STATIC=1")   # prevent __declspec(dllimport) on Windows static KTX
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DVK_NO_PROTOTYPES=1") # use Volk; avoids symbol collisions with vulkan.h

if (CMAKE_BUILD_TYPE STREQUAL "Release")
    set(CMAKE_CXX_FLAGS_RELEASE "-O2 -march=x86-64 -mtune=generic -flto -DNDEBUG" CACHE STRING "")
    set(CMAKE_EXE_LINKER_FLAGS "-Xlinker --strip-all" CACHE STRING "")
else ()
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address -fno-omit-frame-pointer -fno-optimize-sibling-calls")
    # set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-omit-frame-pointer -fno-optimize-sibling-calls")
endif ()

# ── Third-party root (external prebuilt libs) ───────────────────────────────
set(thirdparty "/home/enes/Projects/c/cpp-thirdparty")

# platform-specific build-output subdirectory name used by third-party libs
if (WIN32)
    set(platform_dir "build-win")
elseif (LINUX)
    set(platform_dir "build-linux")
endif ()

# ── Sub-projects ────────────────────────────────────────────────────────────
add_subdirectory(c-utils)
add_subdirectory(c-engine)
add_subdirectory(c-game)
