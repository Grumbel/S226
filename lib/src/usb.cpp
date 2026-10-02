#include "s226/usb.hpp"

#include <libusb.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>

namespace s226 {

namespace fs = std::filesystem;

namespace {

constexpr uint8_t kClassWireless = 0xE0;
constexpr uint8_t kSubclassRf = 0x01;
constexpr uint8_t kProtocolBluetooth = 0x01;

std::string readSysfs(const fs::path& p) {
  std::ifstream in(p);
  std::string s;
  std::getline(in, s);
  return s;
}

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

struct ContextDeleter {
  void operator()(libusb_context* c) const { libusb_exit(c); }
};

} // namespace

std::string UsbController::vidPid() const {
  char buf[16];
  std::snprintf(buf, sizeof buf, "%04x:%04x", vendorId, productId);
  return buf;
}

std::string UsbController::devNode() const {
  char buf[32];
  std::snprintf(buf, sizeof buf, "/dev/bus/usb/%03u/%03u", busNumber, deviceAddress);
  return buf;
}

std::string UsbController::displayName() const {
  std::string name = manufacturer;
  if (!product.empty()) name += (name.empty() ? "" : " ") + product;
  if (name.empty()) name = "USB Bluetooth controller";
  name += " [" + vidPid() + "] port " + portPath;
  if (!hciName.empty()) name += " (" + hciName + ")";
  if (!accessible) name += " - no permission";
  return name;
}

std::vector<UsbController> listUsbControllers() {
  std::vector<UsbController> result;

  libusb_context* raw = nullptr;
  if (libusb_init(&raw) != 0) return result;
  std::unique_ptr<libusb_context, ContextDeleter> ctx(raw);

  libusb_device** list = nullptr;
  ssize_t count = libusb_get_device_list(ctx.get(), &list);
  for (ssize_t i = 0; i < count; ++i) {
    libusb_device* dev = list[i];
    libusb_device_descriptor dd{};
    if (libusb_get_device_descriptor(dev, &dd) != 0) continue;

    libusb_config_descriptor* cfg = nullptr;
    if (libusb_get_active_config_descriptor(dev, &cfg) != 0) continue;

    int hciInterface = -1;
    for (int n = 0; n < cfg->bNumInterfaces && hciInterface < 0; ++n) {
      const libusb_interface& itf = cfg->interface[n];
      for (int a = 0; a < itf.num_altsetting; ++a) {
        const libusb_interface_descriptor& alt = itf.altsetting[a];
        if (alt.bInterfaceClass == kClassWireless && alt.bInterfaceSubClass == kSubclassRf &&
            alt.bInterfaceProtocol == kProtocolBluetooth && alt.bNumEndpoints >= 3) {
          hciInterface = alt.bInterfaceNumber;
          break;
        }
      }
    }
    const int configValue = cfg->bConfigurationValue;
    libusb_free_config_descriptor(cfg);
    if (hciInterface < 0) continue;

    UsbController c;
    c.vendorId = dd.idVendor;
    c.productId = dd.idProduct;
    c.busNumber = libusb_get_bus_number(dev);
    c.deviceAddress = libusb_get_device_address(dev);
    c.interfaceNumber = hciInterface;

    uint8_t ports[8];
    int nports = libusb_get_port_numbers(dev, ports, sizeof ports);
    c.portPath = std::to_string(c.busNumber);
    for (int p = 0; p < nports; ++p) {
      c.portPath += (p == 0 ? "-" : ".") + std::to_string(ports[p]);
    }

    const fs::path sysDev = fs::path("/sys/bus/usb/devices") / c.portPath;
    c.manufacturer = readSysfs(sysDev / "manufacturer");
    c.product = readSysfs(sysDev / "product");

    const fs::path sysItf = fs::path("/sys/bus/usb/devices") /
                            (c.portPath + ":" + std::to_string(configValue) + "." +
                             std::to_string(hciInterface));
    std::error_code ec;
    if (fs::is_symlink(sysItf / "driver", ec)) {
      c.kernelDriver = fs::read_symlink(sysItf / "driver", ec).filename().string();
    }
    for (const auto& e : fs::directory_iterator(sysItf / "bluetooth", ec)) {
      c.hciName = e.path().filename().string();
      break;
    }

    c.accessible = ::access(c.devNode().c_str(), R_OK | W_OK) == 0;
    result.push_back(std::move(c));
  }
  if (list) libusb_free_device_list(list, 1);

  std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
    return a.portPath < b.portPath;
  });
  return result;
}

std::optional<UsbController> findUsbController(const std::string& selectorIn,
                                               std::string* error) {
  auto fail = [&](const std::string& msg) -> std::optional<UsbController> {
    if (error) *error = msg;
    return std::nullopt;
  };

  const auto all = listUsbControllers();
  if (all.empty()) {
    return fail("no USB Bluetooth controller found");
  }

  std::string sel = lower(selectorIn);
  if (sel.rfind("usb:", 0) == 0) sel = sel.substr(4);

  if (sel.empty() || sel == "auto") {
    for (const auto& c : all) {
      if (c.accessible) return c;
    }
    return all.front();
  }

  if (std::all_of(sel.begin(), sel.end(), ::isdigit)) {
    size_t idx = std::stoul(sel);
    if (idx < all.size()) return all[idx];
    return fail("controller index " + sel + " out of range (" +
                std::to_string(all.size()) + " found)");
  }

  for (const auto& c : all) {
    char busdev[16];
    std::snprintf(busdev, sizeof busdev, "%03u/%03u", c.busNumber, c.deviceAddress);
    if (sel == c.vidPid() || sel == c.portPath || sel == busdev ||
        (!c.hciName.empty() && sel == c.hciName)) {
      return c;
    }
  }
  return fail("no USB Bluetooth controller matches '" + selectorIn + "'");
}

} // namespace s226
