cmake_minimum_required(VERSION 3.20)
project(showfps VERSION 1.0.0 LANGUAGES C CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

if(NOT WIN32)
    message(FATAL_ERROR "ShowFPS targets Windows (Forza Horizon 2 PC).")
endif()

# Include FH2 Mod Platform (provides fh2mod_platform and fh2mod_api)
add_subdirectory(platform)

# ShowFPS mod library
add_library(showfps SHARED
    src/showfps.cpp
    src/config.cpp
    src/fps_counter.cpp
    src/gpu_monitor.cpp
    src/overlay.cpp
    src/dxgi_hook.cpp
    src/version.rc
)

target_link_libraries(showfps PRIVATE
    fh2mod_api
    user32
    gdi32
    dwmapi
    dxgi
    d3d11
    pdh
)

set_target_properties(showfps PROPERTIES
    OUTPUT_NAME ShowFPS
    PREFIX ""
)

target_compile_definitions(showfps PRIVATE
    SHOWFPS_VERSION="${PROJECT_VERSION}"
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
)

if(MSVC)
    target_compile_options(showfps PRIVATE /W4 /permissive- /utf-8 /EHsc)
else()
    target_compile_options(showfps PRIVATE -Wall -Wextra -Wno-missing-field-initializers -fms-extensions)
    target_link_options(showfps PRIVATE -static)
endif()

# Package target for easy distribution
add_custom_target(dist
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/dist/fh2-mods
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:fh2mod_platform> ${CMAKE_BINARY_DIR}/dist/d3dcompiler_47.dll
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:showfps> ${CMAKE_BINARY_DIR}/dist/fh2-mods/ShowFPS.dll
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/src/ShowFPS.ini ${CMAKE_BINARY_DIR}/dist/fh2-mods/ShowFPS.ini
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/platform/third_party/minhook/LICENSE.txt ${CMAKE_BINARY_DIR}/dist/LICENSE-MinHook.txt
    DEPENDS fh2mod_platform showfps
    COMMENT "Packaging ShowFPS distribution..."
)
