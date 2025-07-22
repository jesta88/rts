cmake_minimum_required(VERSION 3.31)
project(warcry C CXX)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

option(TOMB_TRACE "Trace mode" OFF)
option(TOMB_HEADLESS "Headless mode" OFF)
option(TOMB_SANITIZE "Enable sanitizers" OFF)
option(TOMB_LTO "Link time optimization" OFF)

# Diagnostic information.
message(STATUS "Configuring ${PROJECT_NAME}")
message(STATUS "* Runtime output directory: ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
message(STATUS "* Library output directory: ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}")
message(STATUS "* Trace: ${WC_TRACE}")
message(STATUS "* Link time optimization: ${WC_LTO}")
message(STATUS "* Sanitize: ${WC_SANITIZE}")

if(WC_SANITIZE)
    if(MSVC)
        add_compile_options("$<$<CONFIG:Debug>:/fsanitize=address>")
        add_link_options("$<$<CONFIG:Debug>:/INCREMENTAL:NO>")
    elseif(APPLE)
        add_compile_options(-fsanitize=address -fsanitize-address-use-after-scope -fsanitize=undefined)
        add_link_options(-fsanitize=address -fsanitize-address-use-after-scope -fsanitize=undefined)
    endif()
endif()

if(MSVC)
    add_compile_options(/permissive- /W3 /WX /Oi /TC /std:clatest /experimental:c11atomics /zi /Zo /FS /utf-8 /GS- /fp:fast /arch:AVX2)
    add_link_options(/INCREMENTAL:NO /OPT:REF,ICF /SUBSYSTEM:WINDOWS)
    add_compile_definitions(WC_WINDOWS WC_MSVC _CRT_SECURE_NO_WARNINGS)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_definitions(WC_DEBUG=1)
endif()

include(cmake/FetchSDL3.cmake)
include(cmake/FetchMimalloc.cmake)
include(cmake/FetchVMA.cmake)
include(cmake/FetchVulkan.cmake)
include(cmake/FetchVolk.cmake)

add_library(vma STATIC
        src/render/allocator.cpp
        src/render/allocator.h)
target_link_libraries(vma PUBLIC volk VulkanMemoryAllocator)

add_executable(${PROJECT_NAME} src/main.c
        src/system/math.c
        src/system/math.h
        src/system/memory.c
        src/system/memory.h
        src/system/context.c
        src/system/context.h
        src/system/debug.h
        src/system/core.h
        src/system/config.c
        src/system/config.h
        src/system/app.c
        src/system/app.h
        src/system/input.c
        src/system/input.h
        src/game/game.c
        src/game/game.h
        src/render/render.c
        src/render/render.h
        src/render/resource.c
        src/render/resource.h
        src/render/types.h)
target_link_libraries(${PROJECT_NAME} PRIVATE Vulkan::Headers volk vma SDL3::SDL3 mimalloc-static)

# Shader compiling
include(cmake/CompileShaders.cmake)

add_shaders_directory(${PROJECT_NAME} ${CMAKE_CURRENT_SOURCE_DIR}/src/shaders)

create_shader_target()

add_dependencies(${PROJECT_NAME} compile_shaders)

if(SPIRV_BINARY_FILES)
    message(STATUS "Shaders to be compiled:")
    foreach(shader ${SPIRV_BINARY_FILES})
        message(STATUS "  ${shader}")
    endforeach()
else()
    message(STATUS "No shaders found to compile")
endif()