file(GLOB_RECURSE cSrc CONFIGURE_DEPENDS "*.cpp")
add_library(c-engine STATIC ${cSrc})

# c-utils is PUBLIC: its include dirs and PCH propagate to anything linking c-engine
target_link_libraries(c-engine PUBLIC c-utils)

target_include_directories(c-engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(c-engine PUBLIC ${CMAKE_SOURCE_DIR})
target_include_directories(c-engine PUBLIC ${thirdparty}/fsr3.1/git/sdk/include)
target_include_directories(c-engine PUBLIC ${thirdparty}/shared)
target_include_directories(c-engine PUBLIC ${thirdparty}/openexr/git/src/lib/OpenEXRCore)
target_include_directories(c-engine PUBLIC ${thirdparty}/openexr/git/${platform_dir}/cmake)
target_include_directories(c-engine PUBLIC ${thirdparty}/openexr/git/${platform_dir}/_deps/imath-build/config)

if (WIN32)
    target_include_directories(c-engine PUBLIC /home/enes/Sdks/VulkanSDK-win/1.4.313.0/Include/)
elseif (LINUX)
    target_include_directories(c-engine PUBLIC /home/enes/Sdks/VulkanSDK-linux/1.4.313.0/x86_64/include/)
    target_include_directories(c-engine PUBLIC /home/enes/Apps/renderdoc/renderdoc/api/app/)
endif ()

target_precompile_headers(c-engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/pch.h)
