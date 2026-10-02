find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBUSB REQUIRED IMPORTED_TARGET libusb-1.0)
find_package(Threads REQUIRED)

add_library(s226ble STATIC
  src/protocol.cpp
  src/step_rate.cpp
  src/usb.cpp
  src/usb_transport.cpp
  src/hci_host.cpp
  src/watch.cpp
)
target_include_directories(s226ble PUBLIC include PRIVATE src)
target_link_libraries(s226ble PRIVATE PkgConfig::LIBUSB PUBLIC Threads::Threads)
target_compile_options(s226ble PRIVATE -Wall -Wextra)

if(BUILD_TESTING)
  add_executable(test_protocol tests/test_protocol.cpp)
  target_link_libraries(test_protocol PRIVATE s226ble)
  add_test(NAME protocol COMMAND test_protocol)
endif()
