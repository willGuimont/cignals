cmake_minimum_required(VERSION 4.2)
project(cignals C)

set(CMAKE_C_STANDARD 23)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBNOTIFY REQUIRED IMPORTED_TARGET libnotify)

add_executable(cignals
        src/main.c
        src/daemon.c)

target_include_directories(cignals PRIVATE src)
target_compile_definitions(cignals PRIVATE _POSIX_C_SOURCE=200809L)
target_link_libraries(cignals PRIVATE PkgConfig::LIBNOTIFY)
