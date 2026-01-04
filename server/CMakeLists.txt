cmake_minimum_required(VERSION 3.16)
project(InternetRadioServer VERSION 0.1 LANGUAGES CXX)

# Sensible defaults for Debian builds
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# HTTP port as an integer cache entry (not a BOOL option, avoid ON/OFF)
set(DEFAULT_HTTP_PORT 8080 CACHE STRING "Port used by the HTTP UI/server")

# If an old cache stored it as BOOL (ON/OFF), reset to numeric default
if(DEFAULT_HTTP_PORT STREQUAL "ON" OR DEFAULT_HTTP_PORT STREQUAL "OFF")
    unset(DEFAULT_HTTP_PORT CACHE)
    set(DEFAULT_HTTP_PORT 8080 CACHE STRING "Port used by the HTTP UI/server")
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(PORTAUDIO REQUIRED IMPORTED_TARGET portaudio-2.0)
find_package(Threads REQUIRED)

add_executable(server
    src/main.cpp
    src/server.cpp
)

target_compile_features(server PRIVATE cxx_std_17)
target_compile_options(server PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(server PRIVATE DEFAULT_HTTP_PORT=${DEFAULT_HTTP_PORT})

target_include_directories(server PRIVATE
    ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(server PRIVATE
    PkgConfig::PORTAUDIO
    Threads::Threads
)

# Ensure runtime data (UI + bundled WAVs) is available next to the binary when run from the build tree
add_custom_command(TARGET server POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
            $<TARGET_FILE_DIR:server>/public
            $<TARGET_FILE_DIR:server>/audio
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${PROJECT_SOURCE_DIR}/public/index.html $<TARGET_FILE_DIR:server>/public/index.html
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${PROJECT_SOURCE_DIR}/audio/berdly.wav $<TARGET_FILE_DIR:server>/audio/berdly.wav
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${PROJECT_SOURCE_DIR}/audio/sans.wav   $<TARGET_FILE_DIR:server>/audio/sans.wav
)
