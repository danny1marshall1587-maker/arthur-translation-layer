cmake_minimum_required(VERSION 3.20)
project(ArthurTranslationLayer VERSION 1.0.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find Wayland dependencies
find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND REQUIRED wayland-client)

# Define the shared library that acts as our translation layer bridge
add_library(arthur_bridge SHARED
    AraPathTranslator.cpp
    WaylandPluginWindow.cpp
    main.cpp
)

# VST3 SDK Integration
# Assuming vst3sdk was cloned into this directory
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/vst3sdk/CMakeLists.txt")
    # We only need the public.sdk headers and pluginterfaces, not the full GUI builder
    add_subdirectory(vst3sdk/pluginterfaces)
    target_link_libraries(arthur_bridge PRIVATE pluginterfaces)
    target_include_directories(arthur_bridge PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/vst3sdk)
else()
    message(WARNING "VST3 SDK not found. Please clone the vst3sdk submodule.")
endif()

# Link Wayland and Wine dependencies
target_include_directories(arthur_bridge PRIVATE ${WAYLAND_INCLUDE_DIRS})
target_link_libraries(arthur_bridge PRIVATE 
    ${WAYLAND_LIBRARIES}
    rt      # Required for shared memory IPC (shm_open, mmap)
)

# If compiling with Winelib, we need to link against libwine
if(DEFINED ENV{WINEBUILD})
    target_link_libraries(arthur_bridge PRIVATE wine)
    # Define __WINE__ so our AraPathTranslator knows to use Wine's fast C-APIs
    target_compile_definitions(arthur_bridge PRIVATE __WINE__)
endif()
