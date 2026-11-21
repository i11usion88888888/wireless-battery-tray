cmake_minimum_required(VERSION 3.16)
project(wireless-battery-tray VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "wireless-battery-tray uses the Win32 API and builds on Windows only.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(wireless-battery-tray WIN32
    src/main.cpp
    src/TrayIcon.cpp
    src/TrayIcon.h
    src/HidDevice.cpp
    src/HidDevice.h
)

target_compile_definitions(wireless-battery-tray PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0A00
)

target_link_libraries(wireless-battery-tray PRIVATE
    hid
    setupapi
    cfgmgr32
    shell32
    user32
    gdi32
)

if(MSVC)
    target_compile_options(wireless-battery-tray PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(wireless-battery-tray PRIVATE -Wall -Wextra)
    if(MINGW)
        target_link_options(wireless-battery-tray PRIVATE -static)
    endif()
endif()
