file(GLOB_RECURSE cSrc CONFIGURE_DEPENDS "*.cpp")
add_library(c-utils STATIC ${cSrc})

target_include_directories(c-utils PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(c-utils PUBLIC ${thirdparty})
target_include_directories(c-utils PUBLIC ${thirdparty}/libzip/git/${platform_dir}/)
target_include_directories(c-utils PUBLIC ${thirdparty}/ktx/git/include/)
target_include_directories(c-utils PUBLIC ${thirdparty}/sdl/git/include)

target_precompile_headers(c-utils PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/Utils.h)
