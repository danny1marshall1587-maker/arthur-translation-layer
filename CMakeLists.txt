cmake_minimum_required(VERSION 3.20)
project(ArthurTranslationLayer VERSION 1.0.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Find Wayland dependencies
find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND REQUIRED wayland-client)

# --- VST3 SDK: Only fetch pluginterfaces (header-only, no GUI/X11 deps) ---
# We download into a directory structure that matches the expected include layout:
#   <prefix>/pluginterfaces/vst/...
include(FetchContent)

set(FETCHCONTENT_BASE_DIR "${CMAKE_BINARY_DIR}/_vst3_deps")

FetchContent_Declare(
    vst3_pluginterfaces
    GIT_REPOSITORY https://github.com/steinbergmedia/vst3_pluginterfaces.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
    SOURCE_DIR     "${CMAKE_BINARY_DIR}/_vst3_headers/pluginterfaces"
)

FetchContent_Declare(
    vst3_base
    GIT_REPOSITORY https://github.com/steinbergmedia/vst3_base.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
    SOURCE_DIR     "${CMAKE_BINARY_DIR}/_vst3_headers/base"
)

FetchContent_MakeAvailable(vst3_pluginterfaces vst3_base)

# Define the shared library
add_library(arthur_bridge SHARED
    AraPathTranslator.cpp
    WaylandPluginWindow.cpp
    main.cpp
)

# VST3 headers: point to the parent so #include <pluginterfaces/vst/...> resolves
target_include_directories(arthur_bridge PRIVATE
    "${CMAKE_BINARY_DIR}/_vst3_headers"
)

# Wayland
target_include_directories(arthur_bridge PRIVATE ${WAYLAND_INCLUDE_DIRS})
target_link_libraries(arthur_bridge PRIVATE
    ${WAYLAND_LIBRARIES}
    rt
)

# Suppress deprecation warnings from VST3 SDK headers
target_compile_options(arthur_bridge PRIVATE -Wno-deprecated-declarations)

# Wine (only when building as Winelib)
if(DEFINED ENV{WINEBUILD})
    target_link_libraries(arthur_bridge PRIVATE wine)
    target_compile_definitions(arthur_bridge PRIVATE __WINE__)
endif()
