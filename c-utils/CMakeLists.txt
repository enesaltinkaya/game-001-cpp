file(GLOB_RECURSE cSrc CONFIGURE_DEPENDS "*.cpp")
add_library(c-utils STATIC ${cSrc})

target_include_directories(c-utils PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(c-utils PUBLIC ${thirdparty})
target_include_directories(c-utils PUBLIC ${thirdparty}/libzip/git/${platform_dir}/)
target_include_directories(c-utils PUBLIC ${thirdparty}/ktx/git/include/)
target_include_directories(c-utils PUBLIC ${thirdparty}/sdl/git/include)
# jansson headers (json/Json.h is public): the generated jansson_config.h
# lives in the per-platform build dir — Linux only worked via /usr/include.
target_include_directories(c-utils PUBLIC ${thirdparty}/jansson/git/${platform_dir}/include)
target_include_directories(c-utils PUBLIC ${thirdparty}/jansson/git/src)

target_precompile_headers(c-utils PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/Utils.h)
