cmake_minimum_required(VERSION 3.10)
project(ArthurBridge)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Force static linking for portability
set(CMAKE_EXE_LINKER_FLAGS "-static-libgcc -static-libstdc++")
set(CMAKE_SHARED_LINKER_FLAGS "-static-libgcc -static-libstdc++")

# Find PkgConfig
find_package(PkgConfig REQUIRED)

# Try to find PipeWire and ALSA
pkg_check_modules(PIPEWIRE REQUIRED libpipewire-0.3)
pkg_check_modules(ALSA REQUIRED alsa)

# Linux Bridge (VST3)
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

# Arthur Daemon
add_executable(arthur-daemon arthur-daemon.cpp)
target_link_libraries(arthur-daemon PRIVATE rt pthread)

# PipeWire CLLS Filter
add_executable(pw_module_clls pw_module_clls.cpp)
target_include_directories(pw_module_clls PRIVATE ${PIPEWIRE_INCLUDE_DIRS})
target_link_libraries(pw_module_clls PRIVATE ${PIPEWIRE_LIBRARIES} rt pthread m)

# ALSA MIDI Sync Daemon
add_executable(midi_sync midi_sync.cpp)
target_include_directories(midi_sync PRIVATE ${ALSA_INCLUDE_DIRS})
target_link_libraries(midi_sync PRIVATE ${ALSA_LIBRARIES} rt pthread)

# Installation rules
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

# Install desktop entry and icons for Flatpak system integration
install(FILES org.arthur.TranslationLayer.desktop DESTINATION share/applications)
install(FILES arthur.png DESTINATION share/pixmaps RENAME org.arthur.TranslationLayer.png)
install(FILES arthur.png DESTINATION share/icons/hicolor/512x512/apps RENAME org.arthur.TranslationLayer.png)

# Install pre-built Tauri frontend binary if it exists
if(EXISTS "${CMAKE_SOURCE_DIR}/bin/arthur-control-center")
    install(PROGRAMS bin/arthur-control-center DESTINATION bin)
endif()

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
