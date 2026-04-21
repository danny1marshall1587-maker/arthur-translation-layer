cmake_minimum_required(VERSION 3.20)
project(ArthurTranslationLayer VERSION 1.0.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Find Wayland dependencies
find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND REQUIRED wayland-client)

# --- VST3 SDK Integration ---
# If not present locally, download during configure
set(VST3SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vst3sdk")
if(NOT EXISTS "${VST3SDK_DIR}/pluginterfaces/CMakeLists.txt")
    message(STATUS "VST3 SDK not found locally. Downloading via FetchContent...")
    include(FetchContent)
    FetchContent_Declare(
        vst3sdk
        GIT_REPOSITORY https://github.com/steinbergmedia/vst3sdk.git
        GIT_TAG        master
        GIT_SHALLOW    TRUE
        SOURCE_DIR     "${VST3SDK_DIR}"
    )
    FetchContent_MakeAvailable(vst3sdk)
endif()

# Only build pluginterfaces (the header-only interface definitions)
if(EXISTS "${VST3SDK_DIR}/pluginterfaces/CMakeLists.txt")
    add_subdirectory("${VST3SDK_DIR}/pluginterfaces" "${CMAKE_BINARY_DIR}/pluginterfaces")
endif()

# Define the shared library that acts as our translation layer bridge
add_library(arthur_bridge SHARED
    AraPathTranslator.cpp
    WaylandPluginWindow.cpp
    main.cpp
)

# VST3 SDK headers
target_link_libraries(arthur_bridge PRIVATE pluginterfaces)
target_include_directories(arthur_bridge PRIVATE "${VST3SDK_DIR}")

# Wayland headers and libraries
target_include_directories(arthur_bridge PRIVATE ${WAYLAND_INCLUDE_DIRS})
target_link_libraries(arthur_bridge PRIVATE 
    ${WAYLAND_LIBRARIES}
    rt      # Required for shared memory IPC (shm_open, mmap)
)

# If compiling with Winelib, link against libwine
if(DEFINED ENV{WINEBUILD})
    target_link_libraries(arthur_bridge PRIVATE wine)
    target_compile_definitions(arthur_bridge PRIVATE __WINE__)
endif()
