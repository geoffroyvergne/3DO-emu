cmake_minimum_required(VERSION 3.20)
project(3DOEmu VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    # Optimised with debug info: a Debug build is too slow to run the BIOS in real time.
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

find_package(SDL2 REQUIRED CONFIG)

# Shared warning flags, applied to every target we own.
add_library(emu_warnings INTERFACE)
if(MSVC)
    target_compile_options(emu_warnings INTERFACE /W4 /permissive-)
else()
    target_compile_options(emu_warnings INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion)
endif()

# Emulation core: pure C++, no SDL dependency, so it can be unit-tested headless.
add_library(emu_core STATIC
    src/core/system.cpp
    src/core/bus/memory.cpp
    src/core/bus/mmio_names.cpp
    src/core/cdrom/disc_image.cpp
    src/core/cdrom/opera_fs.cpp
    src/core/input/pbus.cpp
    src/core/cpu/arm60.cpp
    src/core/clio/clio.cpp
    src/core/sport/sport.cpp
    src/core/xbus/cd_drive.cpp
    src/core/xbus/xbus.cpp
    src/core/slowbus/slow_bus.cpp
    src/core/video/vdlp.cpp
    src/core/madam/cel.cpp
    src/core/madam/madam.cpp
)
target_include_directories(emu_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(emu_core PRIVATE emu_warnings)

# SDL2 frontend executable.
add_executable(3do-emu
    src/main.cpp
    src/frontend/sdl_input.cpp
)
target_link_libraries(3do-emu PRIVATE emu_core emu_warnings SDL2::SDL2)
if(TARGET SDL2::SDL2main)
    target_link_libraries(3do-emu PRIVATE SDL2::SDL2main)
endif()

# Disc inspection tool.
add_executable(opera-ls tools/opera_ls.cpp)
target_link_libraries(opera-ls PRIVATE emu_core emu_warnings)

# Headless unit tests.
enable_testing()
add_executable(arm60_test tests/arm60_test.cpp)
target_link_libraries(arm60_test PRIVATE emu_core emu_warnings)
add_test(NAME arm60 COMMAND arm60_test)
add_executable(memory_test tests/memory_test.cpp)
target_link_libraries(memory_test PRIVATE emu_core emu_warnings)
add_test(NAME memory COMMAND memory_test)
add_executable(opera_fs_test tests/opera_fs_test.cpp)
target_link_libraries(opera_fs_test PRIVATE emu_core emu_warnings)
add_test(NAME opera_fs COMMAND opera_fs_test)
add_executable(cel_test tests/cel_test.cpp)
target_link_libraries(cel_test PRIVATE emu_core emu_warnings)
add_test(NAME cel COMMAND cel_test)
add_executable(input_test tests/input_test.cpp)
target_link_libraries(input_test PRIVATE emu_core emu_warnings)
add_test(NAME input COMMAND input_test)
add_executable(system_test tests/system_test.cpp)
target_link_libraries(system_test PRIVATE emu_core emu_warnings)
add_test(NAME system COMMAND system_test)
add_executable(devices_test tests/devices_test.cpp)
target_link_libraries(devices_test PRIVATE emu_core emu_warnings)
add_test(NAME devices COMMAND devices_test)
