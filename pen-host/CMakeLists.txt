cmake_minimum_required(VERSION 3.16)
project(virtualpen-host VERSION 2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -march=native")

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBUSB REQUIRED IMPORTED_TARGET libusb-1.0)
pkg_check_modules(GLIB REQUIRED IMPORTED_TARGET glib-2.0)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavutil libswscale)
pkg_check_modules(X11 REQUIRED IMPORTED_TARGET x11 xext xrandr)

find_package(Threads REQUIRED)

add_executable(virtualpen-host main.cpp pen.cpp usb.cpp tcp.cpp monitors.cpp patch.cpp controls.cpp preview.cpp)
target_compile_options(virtualpen-host PRIVATE -Wall -Wextra)
target_link_libraries(virtualpen-host PRIVATE PkgConfig::LIBUSB PkgConfig::GLIB PkgConfig::FFMPEG PkgConfig::X11
                      Threads::Threads)

add_executable(pen-test test.cpp pen.cpp monitors.cpp patch.cpp preview.cpp)
target_compile_options(pen-test PRIVATE -Wall -Wextra)
target_link_libraries(pen-test PRIVATE PkgConfig::GLIB PkgConfig::FFMPEG PkgConfig::X11 Threads::Threads)

enable_testing()
add_test(NAME pen-test COMMAND pen-test)

include(GNUInstallDirs)
install(TARGETS virtualpen-host RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
