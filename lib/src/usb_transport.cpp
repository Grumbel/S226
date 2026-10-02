#include "usb_transport.hpp"

#include <libusb.h>

#include <stdexcept>
#include <string>

namespace s226::detail {

namespace {

constexpr unsigned kTimeoutMs = 1000;

std::runtime_error usbError(const std::string& what, int rc) {
  return std::runtime_error(what + ": " + libusb_strerror(static_cast<libusb_error>(rc)));
}

} // namespace

UsbTransport::~UsbTransport() { close(); }

void UsbTransport::open(const UsbController& controller, PacketHandler handler) {
  close();
  handler_ = std::move(handler);

  int rc = libusb_init(&ctx_);
  if (rc != 0) throw usbError("libusb_init failed", rc);

  libusb_device** list = nullptr;
  ssize_t count = libusb_get_device_list(ctx_, &list);
  libusb_device* dev = nullptr;
  for (ssize_t i = 0; i < count; ++i) {
    if (libusb_get_bus_number(list[i]) == controller.busNumber &&
        libusb_get_device_address(list[i]) == controller.deviceAddress) {
      dev = list[i];
      break;
    }
  }
  if (!dev) {
    libusb_free_device_list(list, 1);
    close();
    throw std::runtime_error("USB device " + controller.vidPid() + " at port " +
                             controller.portPath + " disappeared");
  }

  rc = libusb_open(dev, &handle_);
  if (rc != 0) {
    libusb_free_device_list(list, 1);
    handle_ = nullptr;
    close();
    if (rc == LIBUSB_ERROR_ACCESS) {
      throw std::runtime_error("No permission to open " + controller.devNode() + " (" +
                               controller.vidPid() +
                               "). Install udev/70-s226-bluetooth.rules or run as root.");
    }
    throw usbError("Cannot open " + controller.devNode(), rc);
  }

  // Find the HCI endpoints on alt setting 0 of the HCI interface.
  libusb_config_descriptor* cfg = nullptr;
  rc = libusb_get_active_config_descriptor(dev, &cfg);
  libusb_free_device_list(list, 1);
  if (rc != 0) {
    close();
    throw usbError("Cannot read config descriptor", rc);
  }
  interface_ = controller.interfaceNumber;
  for (int n = 0; n < cfg->bNumInterfaces; ++n) {
    const libusb_interface_descriptor& alt = cfg->interface[n].altsetting[0];
    if (alt.bInterfaceNumber != interface_) continue;
    for (int e = 0; e < alt.bNumEndpoints; ++e) {
      const libusb_endpoint_descriptor& ep = alt.endpoint[e];
      const int type = ep.bmAttributes & LIBUSB_TRANSFER_TYPE_MASK;
      const bool in = (ep.bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_IN;
      if (type == LIBUSB_TRANSFER_TYPE_INTERRUPT && in) {
        epEvents_ = ep.bEndpointAddress;
        eventPacketSize_ = ep.wMaxPacketSize ? ep.wMaxPacketSize : 16;
      } else if (type == LIBUSB_TRANSFER_TYPE_BULK && in) {
        epAclIn_ = ep.bEndpointAddress;
      } else if (type == LIBUSB_TRANSFER_TYPE_BULK && !in) {
        epAclOut_ = ep.bEndpointAddress;
      }
    }
  }
  libusb_free_config_descriptor(cfg);
  if (!epEvents_ || !epAclIn_ || !epAclOut_) {
    close();
    throw std::runtime_error("HCI interface has unexpected endpoints");
  }

  // Detach btusb from the HCI interface only. On WiFi/BT combo chips the
  // WiFi interface stays with its kernel driver. libusb re-attaches btusb
  // when the interface is released.
  libusb_set_auto_detach_kernel_driver(handle_, 1);
  rc = libusb_claim_interface(handle_, interface_);
  if (rc != 0) {
    libusb_close(handle_);
    handle_ = nullptr;
    close();
    if (rc == LIBUSB_ERROR_BUSY) {
      throw std::runtime_error("Bluetooth controller " + controller.vidPid() +
                               " is in use by another program");
    }
    throw usbError("Cannot claim HCI interface", rc);
  }

  // btusb submits interrupt URBs of wMaxPacketSize and reassembles; doing
  // the same avoids events getting stuck when their length is a multiple
  // of the packet size (no short packet terminates the transfer).
  eventBuf_.assign(static_cast<size_t>(eventPacketSize_), 0);
  aclBuf_.assign(1024, 0);
  eventTransfer_ = libusb_alloc_transfer(0);
  aclTransfer_ = libusb_alloc_transfer(0);
  libusb_fill_interrupt_transfer(eventTransfer_, handle_, epEvents_, eventBuf_.data(),
                                 static_cast<int>(eventBuf_.size()), &UsbTransport::onTransfer,
                                 this, 0);
  libusb_fill_bulk_transfer(aclTransfer_, handle_, epAclIn_, aclBuf_.data(),
                            static_cast<int>(aclBuf_.size()), &UsbTransport::onTransfer, this,
                            0);

  running_ = true;
  for (libusb_transfer* t : {eventTransfer_, aclTransfer_}) {
    rc = libusb_submit_transfer(t);
    if (rc != 0) {
      close();
      throw usbError("Cannot start USB transfer", rc);
    }
    ++pendingTransfers_;
  }
  thread_ = std::thread([this] { eventLoop(); });
}

void UsbTransport::close() {
  running_ = false;
  if (handle_) {
    for (libusb_transfer* t : {eventTransfer_, aclTransfer_}) {
      if (t) libusb_cancel_transfer(t);
    }
  }
  if (thread_.joinable()) thread_.join();
  // Drain cancellations if the thread never ran.
  while (ctx_ && pendingTransfers_ > 0) {
    timeval tv{0, 100000};
    libusb_handle_events_timeout(ctx_, &tv);
  }
  if (eventTransfer_) libusb_free_transfer(eventTransfer_);
  if (aclTransfer_) libusb_free_transfer(aclTransfer_);
  eventTransfer_ = aclTransfer_ = nullptr;
  if (handle_) {
    libusb_release_interface(handle_, interface_);
    libusb_close(handle_);
    handle_ = nullptr;
  }
  if (ctx_) {
    libusb_exit(ctx_);
    ctx_ = nullptr;
  }
  eventAsm_.buffer.clear();
  aclAsm_.buffer.clear();
  epEvents_ = epAclIn_ = epAclOut_ = 0;
}

void UsbTransport::eventLoop() {
  while (running_ || pendingTransfers_ > 0) {
    if (!running_) {
      // A callback may have resubmitted just before close() cancelled.
      libusb_cancel_transfer(eventTransfer_);
      libusb_cancel_transfer(aclTransfer_);
    }
    timeval tv{0, 100000};
    libusb_handle_events_timeout_completed(ctx_, &tv, nullptr);
  }
}

void UsbTransport::onTransfer(libusb_transfer* t) {
  auto* self = static_cast<UsbTransport*>(t->user_data);
  if (t->status == LIBUSB_TRANSFER_COMPLETED) {
    self->handleData(t == self->eventTransfer_ ? HciPacketType::Event : HciPacketType::Acl,
                     t->buffer, t->actual_length);
  }
  if (self->running_ && (t->status == LIBUSB_TRANSFER_COMPLETED ||
                         t->status == LIBUSB_TRANSFER_TIMED_OUT)) {
    if (libusb_submit_transfer(t) == 0) return;
  }
  --self->pendingTransfers_;
}

void UsbTransport::handleData(HciPacketType type, const uint8_t* data, int len) {
  auto& buf = (type == HciPacketType::Event ? eventAsm_ : aclAsm_).buffer;
  buf.insert(buf.end(), data, data + len);
  for (;;) {
    size_t header = type == HciPacketType::Event ? 2 : 4;
    if (buf.size() < header) return;
    size_t payload = type == HciPacketType::Event ? buf[1] : (buf[2] | buf[3] << 8);
    if (buf.size() < header + payload) return;
    std::vector<uint8_t> packet(buf.begin(), buf.begin() + static_cast<long>(header + payload));
    buf.erase(buf.begin(), buf.begin() + static_cast<long>(header + payload));
    if (handler_) handler_(type, std::move(packet));
  }
}

void UsbTransport::sendCommand(const std::vector<uint8_t>& packet) {
  if (!handle_) throw std::runtime_error("transport closed");
  int rc = libusb_control_transfer(
      handle_,
      static_cast<uint8_t>(LIBUSB_ENDPOINT_OUT) | static_cast<uint8_t>(LIBUSB_REQUEST_TYPE_CLASS) |
          static_cast<uint8_t>(LIBUSB_RECIPIENT_DEVICE),
      0, 0,
      0, const_cast<uint8_t*>(packet.data()), static_cast<uint16_t>(packet.size()),
      kTimeoutMs);
  if (rc < 0) throw usbError("HCI command transfer failed", rc);
}

void UsbTransport::sendAcl(const std::vector<uint8_t>& packet) {
  if (!handle_) throw std::runtime_error("transport closed");
  int transferred = 0;
  int rc = libusb_bulk_transfer(handle_, epAclOut_, const_cast<uint8_t*>(packet.data()),
                                static_cast<int>(packet.size()), &transferred, kTimeoutMs);
  if (rc != 0) throw usbError("HCI ACL transfer failed", rc);
}

} // namespace s226::detail
