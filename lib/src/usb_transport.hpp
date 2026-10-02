// HCI over USB (Bluetooth Core Vol 4 Part B) using libusb.
//
// Takes the HCI interface away from the kernel's btusb driver for the
// lifetime of the object and gives it back on close().

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "s226/usb.hpp"

struct libusb_context;
struct libusb_device_handle;
struct libusb_transfer;

namespace s226::detail {

enum class HciPacketType : uint8_t { Event = 0x04, Acl = 0x02 };

class UsbTransport {
public:
  using PacketHandler = std::function<void(HciPacketType, std::vector<uint8_t>)>;

  UsbTransport() = default;
  ~UsbTransport();
  UsbTransport(const UsbTransport&) = delete;
  UsbTransport& operator=(const UsbTransport&) = delete;

  // Throws std::runtime_error with a user-presentable message.
  // The handler is called from an internal USB thread.
  void open(const UsbController& controller, PacketHandler handler);
  void close();
  bool isOpen() const { return handle_ != nullptr; }

  // Packets without the H4 type byte.
  void sendCommand(const std::vector<uint8_t>& packet);
  void sendAcl(const std::vector<uint8_t>& packet);

private:
  struct Reassembler {
    std::vector<uint8_t> buffer;
  };

  static void onTransfer(libusb_transfer* transfer);
  void handleData(HciPacketType type, const uint8_t* data, int len);
  void eventLoop();

  libusb_context* ctx_ = nullptr;
  libusb_device_handle* handle_ = nullptr;
  int interface_ = 0;
  uint8_t epEvents_ = 0;
  uint8_t epAclIn_ = 0;
  uint8_t epAclOut_ = 0;
  int eventPacketSize_ = 16;

  libusb_transfer* eventTransfer_ = nullptr;
  libusb_transfer* aclTransfer_ = nullptr;
  std::vector<uint8_t> eventBuf_;
  std::vector<uint8_t> aclBuf_;
  Reassembler eventAsm_;
  Reassembler aclAsm_;
  std::atomic<int> pendingTransfers_{0};

  PacketHandler handler_;
  std::atomic<bool> running_{false};
  std::thread thread_;
};

} // namespace s226::detail
