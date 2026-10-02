// Discovery of USB Bluetooth controllers usable for raw HCI access.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace s226 {

struct UsbController {
  uint16_t vendorId = 0;
  uint16_t productId = 0;
  uint8_t busNumber = 0;
  uint8_t deviceAddress = 0;
  std::string portPath;      // e.g. "1-10.1", stable across replugging
  int interfaceNumber = 0;   // the HCI interface (class e0/01/01)
  std::string manufacturer;  // from sysfs, may be empty
  std::string product;       // from sysfs, may be empty (often misleading
                             // on WiFi/BT combo chips, e.g. "802.11ac NIC")
  std::string kernelDriver;  // driver bound to the HCI interface ("btusb", "usbfs", "")
  std::string hciName;       // "hci0" if the kernel exposes it as an adapter
  bool accessible = false;   // current user may open the device node

  std::string vidPid() const;      // "0bda:b82c"
  std::string devNode() const;     // "/dev/bus/usb/001/012"
  std::string displayName() const; // one-line human readable description
};

// All USB devices that have a Bluetooth HCI interface.
std::vector<UsbController> listUsbControllers();

// Resolve a user-supplied selector against listUsbControllers().
//
// Accepted forms:
//   ""  / "auto"         first controller (accessible ones preferred)
//   "0", "1", ...        index into the list
//   "0bda:b82c"          vendor:product (Bumble-style "usb:" prefix allowed)
//   "1-10.1"             USB port path
//   "001/012"            bus/device as in lsusb
//   "hci0"               the kernel adapter currently backed by that dongle
std::optional<UsbController> findUsbController(const std::string& selector,
                                               std::string* error = nullptr);

} // namespace s226
