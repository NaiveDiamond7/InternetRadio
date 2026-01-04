cmake_minimum_required(VERSION 3.10)
project(InternetRadioServer)

# Set C++17 standard (wymagane m.in. dla nowocześniejszych narzędzi)
set(CMAKE_CXX_STANDARD 17)
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
target_compile_options(server PRIVATE -Wall -Wextra -Wpedantic)

# Opcjonalnie: domyślny port HTTP jako definicja kompilacji (można nadpisać przez -DDEFAULT_HTTP_PORT=...)
if(NOT DEFINED DEFAULT_HTTP_PORT)
    set(DEFAULT_HTTP_PORT 8080)
endif()

target_compile_definitions(server PRIVATE DEFAULT_HTTP_PORT=${DEFAULT_HTTP_PORT})
