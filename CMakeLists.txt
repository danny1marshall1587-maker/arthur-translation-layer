cmake_minimum_required(VERSION 3.16)
project(ArthurBridge)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(BUILD_GUI "Build Qt6 Control Center GUI" ON)

if(BUILD_GUI)
    # Enable Qt AUTOMOC, AUTOUIC, and AUTORCC
    set(CMAKE_AUTOMOC ON)
    set(CMAKE_AUTOUIC ON)
    set(CMAKE_AUTORCC ON)
    
    # Find Qt6
    find_package(Qt6 COMPONENTS Widgets Core Gui Network REQUIRED)
endif()

# Find Packages
find_package(PkgConfig REQUIRED)

# Try to find PipeWire and ALSA
pkg_check_modules(PIPEWIRE REQUIRED libpipewire-0.3)
pkg_check_modules(ALSA REQUIRED alsa)

# --- GUI: Arthur Control Center (Native C++ Qt6) ---
if(BUILD_GUI)
    add_executable(arthur-control-center
        gui/main.cpp
        gui/mainwindow.cpp
        gui/mainwindow.h
    )
    target_link_libraries(arthur-control-center PRIVATE
        Qt6::Widgets
        Qt6::Core
        Qt6::Gui
        Qt6::Network
    )
endif()

# --- Linux Bridge (VST3) ---
add_library(arthur_bridge SHARED
    main.cpp
    vstiids.cpp
)
target_include_directories(arthur_bridge PRIVATE 
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/vst3_headers
)
target_link_libraries(arthur_bridge PRIVATE rt pthread)
set_target_properties(arthur_bridge PROPERTIES PREFIX "" NO_SONAME ON)

# --- Arthur Daemon ---
add_executable(arthur-daemon arthur-daemon.cpp)
target_link_libraries(arthur-daemon PRIVATE rt pthread)

# --- PipeWire CLLS Filter ---
add_executable(pw_module_clls pw_module_clls.cpp)
target_include_directories(pw_module_clls PRIVATE ${PIPEWIRE_INCLUDE_DIRS})
target_link_libraries(pw_module_clls PRIVATE ${PIPEWIRE_LIBRARIES} rt pthread m)

# --- ALSA MIDI Sync Daemon ---
add_executable(midi_sync midi_sync.cpp)
target_include_directories(midi_sync PRIVATE ${ALSA_INCLUDE_DIRS})
target_link_libraries(midi_sync PRIVATE ${ALSA_LIBRARIES} rt pthread)

# --- Installation rules ---
if(BUILD_GUI)
    install(TARGETS arthur-control-center RUNTIME DESTINATION bin)
endif()

install(TARGETS arthur-daemon pw_module_clls midi_sync
    RUNTIME DESTINATION bin
)

install(TARGETS arthur_bridge
    LIBRARY DESTINATION lib
)

install(PROGRAMS 
    vdc_tune.sh
    arthur-installer-bridge.sh
    DESTINATION bin
)

# Copy scripts to the build directory for local testing/running
configure_file(vdc_tune.sh ${CMAKE_CURRENT_BINARY_DIR}/vdc_tune.sh COPYONLY)
configure_file(arthur-installer-bridge.sh ${CMAKE_CURRENT_BINARY_DIR}/arthur-installer-bridge.sh COPYONLY)

# Install desktop entry and icons for Flatpak system integration
install(FILES org.arthur.TranslationLayer.desktop DESTINATION share/applications)
install(FILES arthur.png DESTINATION share/pixmaps RENAME org.arthur.TranslationLayer.png)
install(FILES arthur.png DESTINATION share/icons/hicolor/512x512/apps RENAME org.arthur.TranslationLayer.png)

# Install pre-built Windows Guest Agent binaries if they exist
if(EXISTS "${CMAKE_SOURCE_DIR}/bin/win_guest_agent.exe")
    install(PROGRAMS bin/win_guest_agent.exe DESTINATION bin)
endif()
if(EXISTS "${CMAKE_SOURCE_DIR}/bin/win_guest_agent.exe.so")
    install(PROGRAMS bin/win_guest_agent.exe.so DESTINATION bin)
endif()

# Install pre-built Wine Guest binaries if they exist
if(EXISTS "${CMAKE_SOURCE_DIR}/bin/arthur-guest.exe")
    install(PROGRAMS bin/arthur-guest.exe DESTINATION bin)
endif()
if(EXISTS "${CMAKE_SOURCE_DIR}/bin/arthur-guest.exe.so")
    install(PROGRAMS bin/arthur-guest.exe.so DESTINATION bin)
endif()
