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

add_executable(virtualpen-host main.cpp pen.cpp usb.cpp tcp.cpp)
target_compile_options(virtualpen-host PRIVATE -Wall -Wextra)
target_link_libraries(virtualpen-host PRIVATE PkgConfig::LIBUSB)

add_executable(pen-test test.cpp pen.cpp)
target_compile_options(pen-test PRIVATE -Wall -Wextra)

enable_testing()
add_test(NAME pen-test COMMAND pen-test)

include(GNUInstallDirs)
install(TARGETS virtualpen-host RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
