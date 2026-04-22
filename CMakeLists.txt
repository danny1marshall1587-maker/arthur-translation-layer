cmake_minimum_required(VERSION 3.10)
project(ArthurBridge)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Force static linking for portability
set(CMAKE_EXE_LINKER_FLAGS "-static-libgcc -static-libstdc++")
set(CMAKE_SHARED_LINKER_FLAGS "-static-libgcc -static-libstdc++")

# Linux Bridge (VST3)
add_library(arthur_bridge SHARED
    main.cpp
    vstiids.cpp
)

target_include_directories(arthur_bridge PRIVATE 
    ${CMAKE_SOURCE_DIR}
    build/_vst3_headers
)

target_link_libraries(arthur_bridge PRIVATE rt pthread)
set_target_properties(arthur_bridge PROPERTIES PREFIX "" NO_SONAME ON)

# Arthur Daemon
add_executable(arthur-daemon arthur-daemon.cpp)
target_link_libraries(arthur-daemon PRIVATE rt pthread)
