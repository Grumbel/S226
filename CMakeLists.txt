cmake_minimum_required(VERSION 3.21)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" S226_VERSION_FILE)
string(STRIP "${S226_VERSION_FILE}" S226_VERSION_FILE)
set(S226_VERSION "${S226_VERSION_FILE}" CACHE STRING "Version string shown by the tools")

project(s226 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(S226_BUILD_GUI "Build the Qt heart-rate GUI" ON)

include(GNUInstallDirs)
include(CTest)

add_subdirectory(lib)
add_subdirectory(cli)
if(S226_BUILD_GUI)
  add_subdirectory(app)
endif()

install(FILES udev/70-s226-bluetooth.rules DESTINATION ${CMAKE_INSTALL_LIBDIR}/udev/rules.d)
