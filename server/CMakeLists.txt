cmake_minimum_required(VERSION 3.10)
project(InternetRadioServer)

# Set C++11 standard
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find required packages
find_package(Threads REQUIRED)

# Find PortAudio using pkg-config (works on Linux and macOS with Homebrew)
find_package(PkgConfig REQUIRED)
pkg_check_modules(PORTAUDIO REQUIRED portaudio-2.0)

# Include directories
include_directories(
    ${PORTAUDIO_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Add executable
add_executable(server
    src/main.cpp
    src/server.cpp
)

# Link libraries
target_link_libraries(server
    ${PORTAUDIO_LIBRARIES}
    Threads::Threads
)

# Link directories
link_directories(${PORTAUDIO_LIBRARY_DIRS})

# Compiler flags
target_compile_options(server PRIVATE -Wall)
