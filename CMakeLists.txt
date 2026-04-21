cmake_minimum_required(VERSION 3.20)
project(ArthurTranslationLayer VERSION 1.0.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Find Wayland dependencies
find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND REQUIRED wayland-client)

# --- VST3 SDK: Only fetch pluginterfaces (header-only, no GUI/X11 deps) ---
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

# ============================================================
# TARGET 1: Linux-side Bridge (loaded by REAPER)
# ============================================================
add_library(arthur_bridge SHARED
    main.cpp
    AraPathTranslator.cpp
    WaylandPluginWindow.cpp
    ipc/control-protocol.cpp
)

# VST3 headers
target_include_directories(arthur_bridge PRIVATE
    "${CMAKE_BINARY_DIR}/_vst3_headers"
    "${CMAKE_SOURCE_DIR}"
)

# Wayland
target_include_directories(arthur_bridge PRIVATE ${WAYLAND_INCLUDE_DIRS})
target_link_libraries(arthur_bridge PRIVATE
    ${WAYLAND_LIBRARIES}
    rt
)

# Suppress deprecation warnings from VST3 SDK headers
target_compile_options(arthur_bridge PRIVATE -Wno-deprecated-declarations)

# ============================================================
# TARGET 2: Wine-side Guest (runs inside Wine)
# ============================================================
# The Guest executable must be built with wineg++ to produce a Winelib binary.
# We build it via a custom command since CMake doesn't natively support wineg++.

# Find wineg++
find_program(WINEGPP wineg++)

if(WINEGPP)
    message(STATUS "Found wineg++: ${WINEGPP}")

    set(GUEST_SOURCES
        "${CMAKE_SOURCE_DIR}/guest.cpp"
        "${CMAKE_SOURCE_DIR}/ipc/control-protocol.cpp"
    )

    set(GUEST_OUTPUT "${CMAKE_BINARY_DIR}/arthur-guest.exe")

    # VST3 header include path
    set(VST3_INCLUDE "${CMAKE_BINARY_DIR}/_vst3_headers")

    add_custom_command(
        OUTPUT ${GUEST_OUTPUT} ${GUEST_OUTPUT}.so
        COMMAND ${WINEGPP}
            -std=c++20
            -I${VST3_INCLUDE}
            -I${CMAKE_SOURCE_DIR}
            -Wno-deprecated-declarations
            -o ${GUEST_OUTPUT}
            ${GUEST_SOURCES}
            -lole32
        DEPENDS ${GUEST_SOURCES}
        COMMENT "[Arthur] Building Wine Guest (arthur-guest.exe)..."
    )

    add_custom_target(arthur_guest ALL DEPENDS ${GUEST_OUTPUT})

    # Install the Guest alongside the bridge
    install(FILES ${GUEST_OUTPUT} ${GUEST_OUTPUT}.so
            DESTINATION lib/arthur)
else()
    message(WARNING "wineg++ not found — cannot build arthur-guest.exe. Install wine-devel.")
endif()

# Install the bridge
install(TARGETS arthur_bridge LIBRARY DESTINATION lib/arthur)
