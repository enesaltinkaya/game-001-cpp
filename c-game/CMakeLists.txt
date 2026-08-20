file(GLOB_RECURSE cSrc CONFIGURE_DEPENDS "*.cpp")
# exclude scripts/ and any stray build/ dirs inside the source tree
list(FILTER cSrc EXCLUDE REGEX ".*/scripts/.*")
list(FILTER cSrc EXCLUDE REGEX ".*/build/.*")
add_executable(c-game ${cSrc})

target_include_directories(c-game PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/game)

# Linking c-engine transitively gives us:
#   - c-engine include dir (c-engine/)
#   - c-utils include dir and thirdparty dirs (via c-utils PUBLIC)
#   - both precompiled headers (Utils.h, pch.h)
target_link_libraries(c-game PRIVATE
    -Wl,--start-group
    c-engine
    c-utils

    # zip
    ${thirdparty}/libzip/git/${platform_dir}/lib/libzip.a
    ${thirdparty}/bzip2/git/${platform_dir}/libbz2_static.a
    ${thirdparty}/zlib/git/${platform_dir}/libzs.a
    ${thirdparty}/zstd/git/${platform_dir}/lib/libzstd.a

    # render
    ${thirdparty}/sdl/git/${platform_dir}/libSDL3.a
    ${thirdparty}/stb/git/${platform_dir}/libstb.a
    ${thirdparty}/volk/git/${platform_dir}/libvolk.a
    ${thirdparty}/vma/git/${platform_dir}/libvma.a
    ${thirdparty}/cgltf/git/${platform_dir}/libcgltf.a
    ${thirdparty}/ktx/git/${platform_dir}/libktx_read.a
    ${thirdparty}/rmlui/wrapper/${platform_dir}/libcrmlui.a
    ${thirdparty}/rmlui/git/${platform_dir}/librmlui_lua.a
    ${thirdparty}/rmlui/git/${platform_dir}/librmlui_debugger.a
    ${thirdparty}/rmlui/git/${platform_dir}/librmlui.a
    ${thirdparty}/meshoptimizer/git/${platform_dir}/libmeshoptimizer.a
    ${thirdparty}/fsr3.1/git/sdk/${platform_dir}/libffx_fsr3upscaler_vk.a

    # audio
    ${thirdparty}/soloud/git/${platform_dir}/libsoloud.a

    # physics
    ${thirdparty}/jolt/wrapper/${platform_dir}/libcjolt.a
    ${thirdparty}/jolt/git/${platform_dir}/libJolt.a

    # navigation
    ${thirdparty}/recast/wrapper/${platform_dir}/libcrecast.a
    ${thirdparty}/recast/git/${platform_dir}/Recast/libRecast.a
    ${thirdparty}/recast/git/${platform_dir}/Detour/libDetour.a

    # tools
    ${thirdparty}/lua/git/${platform_dir}/liblua.a
    ${thirdparty}/jansson/git/${platform_dir}/lib/libjansson.a
    ${thirdparty}/thpool/${platform_dir}/libthpool.a
    ${thirdparty}/sqlite/git/${platform_dir}/libsqlite3.a
    ${thirdparty}/freetype/git/${platform_dir}/libfreetype.a
    ${thirdparty}/harfbuzz/git/${platform_dir}/libharfbuzz.a
    ${thirdparty}/png/git/${platform_dir}/libpng.a
    ${thirdparty}/brotli/git/${platform_dir}/libbrotlidec.a
    ${thirdparty}/brotli/git/${platform_dir}/libbrotlicommon.a
    ${thirdparty}/openexr/git/${platform_dir}/src/lib/OpenEXRCore/libOpenEXRCore-4_0.a
    ${thirdparty}/openexr/git/${platform_dir}/external/OpenJPH/src/core/libopenjph.a
    -Wl,--end-group
)

target_include_directories(c-game PUBLIC ${thirdparty}/rmlui/git/Include)
target_include_directories(c-game PUBLIC ${thirdparty}/fsr3.1/git/sdk/include)
target_include_directories(c-game PUBLIC ${thirdparty}/recast/wrapper/src)

if (WIN32)
    target_link_libraries(c-game PRIVATE bcrypt hid)
    target_link_libraries(c-game PRIVATE imm32 setupapi version winmm) # sdl
    target_link_libraries(c-game PRIVATE stdc++)
    target_link_libraries(c-game PRIVATE -static pthread)
    target_include_directories(c-game PUBLIC /home/enes/Sdks/VulkanSDK-win/1.4.313.0/Include/)
elseif (LINUX)
    target_link_libraries(c-game PRIVATE stdc++ pthread dl m bsd)
    target_include_directories(c-game PUBLIC /home/enes/Sdks/VulkanSDK-linux/1.4.313.0/x86_64/include/)
endif ()
