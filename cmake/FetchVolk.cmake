include(FetchContent)

set(VOLK_PULL_IN_VULKAN OFF CACHE BOOL "VOLK_PULL_IN_VULKAN")

if (WIN32)
    set(VOLK_STATIC_DEFINES VK_USE_PLATFORM_WIN32_KHR)
endif()

FetchContent_Declare(
        volk
        GIT_REPOSITORY https://github.com/zeux/volk.git
        GIT_TAG master
)
FetchContent_MakeAvailable(volk)

target_compile_definitions(volk PUBLIC VK_NO_PROTOTYPES)
target_link_libraries(volk PUBLIC Vulkan::Headers)