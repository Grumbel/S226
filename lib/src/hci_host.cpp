#include "hci_host.hpp"

#include <cstdio>

namespace s226::detail {

namespace {

constexpr uint8_t kEvDisconnectionComplete = 0x05;
constexpr uint8_t kEvCommandComplete = 0x0E;
constexpr uint8_t kEvCommandStatus = 0x0F;
constexpr uint8_t kEvHardwareError = 0x10;
constexpr uint8_t kEvNumberOfCompletedPackets = 0x13;
constexpr uint8_t kEvLeMeta = 0x3E;

constexpr uint8_t kLeConnectionComplete = 0x01;
constexpr uint8_t kLeAdvertisingReport = 0x02;
constexpr uint8_t kLeEnhancedConnectionComplete = 0x0A;

constexpr uint16_t kRealtek = 0x005D;

std::string opcodeName(uint16_t op) {
  char buf[16];
  std::snprintf(buf, sizeof buf, "0x%04x", op);
  return buf;
}

} // namespace

std::string addressToString(const Address& a) {
  char buf[24];
  std::snprintf(buf, sizeof buf, "%02X:%02X:%02X:%02X:%02X:%02X", a[5], a[4], a[3], a[2], a[1],
                a[0]);
  return buf;
}

std::optional<Address> addressFromString(const std::string& s) {
  unsigned v[6];
  if (std::sscanf(s.c_str(), "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) !=
      6) {
    return std::nullopt;
  }
  Address a;
  for (int i = 0; i < 6; ++i) a[static_cast<size_t>(5 - i)] = static_cast<uint8_t>(v[i]);
  return a;
}

std::string hex(const Bytes& data) {
  std::string s;
  char buf[4];
  for (uint8_t b : data) {
    std::snprintf(buf, sizeof buf, s.empty() ? "%02x" : " %02x", b);
    s += buf;
  }
  return s;
}

HciHost::HciHost(LogFn log) : log_(std::move(log)) {}

HciHost::~HciHost() { close(); }

void HciHost::open(const UsbController& controller) {
  transport_.open(controller, [this](HciPacketType type, Bytes data) {
    {
      std::lock_guard lock(mutex_);
      queue_.push_back(Item{type, std::move(data), {}});
    }
    cv_.notify_one();
  });
  initController();
}

void HciHost::close() {
  transport_.close();
  std::lock_guard lock(mutex_);
  queue_.clear();
  pendingOpcode_.reset();
  commandResult_.reset();
  inFlight_.clear();
  l2capRx_.clear();
}

void HciHost::initController() {
  command(opcode::Reset);

  Bytes ver = command(opcode::ReadLocalVersion);
  if (ver.size() >= 9) {
    const uint16_t manufacturer = le16(&ver[5]);
    const uint16_t subversion = le16(&ver[7]);
    char buf[96];
    std::snprintf(buf, sizeof buf, "Controller: HCI %u, manufacturer 0x%04x, subversion 0x%04x",
                  ver[1], manufacturer, subversion);
    log_(buf);
    // Realtek ROM code reports the chip ID as subversion until btusb has
    // downloaded the patch firmware. The patch survives HCI_Reset.
    if (manufacturer == kRealtek && (subversion & 0xF000) == 0x8000) {
      log_("Warning: Realtek firmware looks unpatched. Re-plug the dongle so the kernel "
           "loads it, then try again.");
    }
  }

  Bytes addr = command(opcode::ReadBdAddr);
  if (addr.size() >= 7) {
    Address a;
    std::copy(addr.begin() + 1, addr.begin() + 7, a.begin());
    log_("Controller address: " + addressToString(a));
  }

  // Default mask plus LE Meta (bit 61).
  command(opcode::SetEventMask, {0xFF, 0xFF, 0xFB, 0xFF, 0x07, 0xF8, 0xBF, 0x3D});
  // Connection Complete, Advertising Report, Connection Update Complete,
  // Read Remote Features Complete, LTK Request.
  command(opcode::LeSetEventMask, {0x1F, 0, 0, 0, 0, 0, 0, 0});
  command(opcode::WriteLeHostSupport, {0x01, 0x00}, /*allowFailure=*/true);

  Bytes buf = command(opcode::LeReadBufferSize);
  uint16_t mtu = buf.size() >= 4 ? le16(&buf[1]) : 0;
  int count = buf.size() >= 4 ? buf[3] : 0;
  if (mtu == 0 || count == 0) {
    // Shared ACL buffers.
    Bytes shared = command(opcode::ReadBufferSize);
    if (shared.size() >= 6) {
      mtu = le16(&shared[1]);
      count = le16(&shared[4]);
    }
  }
  aclMtu_ = mtu ? mtu : 27;
  aclCredits_ = count ? count : 1;
}

void HciHost::sendCommandPacket(uint16_t op, const Bytes& params) {
  Bytes pkt;
  put16(pkt, op);
  pkt.push_back(static_cast<uint8_t>(params.size()));
  pkt.insert(pkt.end(), params.begin(), params.end());
  transport_.sendCommand(pkt);
}

Bytes HciHost::command(uint16_t op, const Bytes& params, bool allowFailure) {
  pendingOpcode_ = op;
  commandResult_.reset();
  sendCommandPacket(op, params);
  const bool ok = waitUntil([&] { return commandResult_.has_value(); },
                            Clock::now() + 3s, /*cancellable=*/false);
  pendingOpcode_.reset();
  if (!ok) {
    throw std::runtime_error("Bluetooth controller not responding to command " +
                             opcodeName(op));
  }
  Bytes result = std::move(*commandResult_);
  commandResult_.reset();
  if (!allowFailure && (result.empty() || result[0] != 0)) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "HCI command %s failed with status 0x%02x", opcodeName(op).c_str(),
                  result.empty() ? 0xFF : result[0]);
    throw std::runtime_error(buf);
  }
  return result;
}

uint8_t HciHost::commandWithStatus(uint16_t op, const Bytes& params) {
  Bytes r = command(op, params, /*allowFailure=*/true);
  return r.empty() ? 0xFF : r[0];
}

void HciHost::commandNoWait(uint16_t op, const Bytes& params) { sendCommandPacket(op, params); }

void HciHost::sendL2cap(uint16_t handle, uint16_t cid, const Bytes& payload) {
  Bytes frame;
  put16(frame, static_cast<uint16_t>(payload.size()));
  put16(frame, cid);
  frame.insert(frame.end(), payload.begin(), payload.end());

  size_t offset = 0;
  bool first = true;
  while (offset < frame.size()) {
    if (aclCredits_ <= 0) {
      waitUntil([&] { return aclCredits_ > 0; }, Clock::now() + 2s, false);
      if (aclCredits_ <= 0) throw LinkLost("controller stopped accepting data");
    }
    const size_t n = std::min<size_t>(aclMtu_, frame.size() - offset);
    Bytes pkt;
    // PB=00 first non-flushable, 01 continuation.
    put16(pkt, static_cast<uint16_t>(handle | (first ? 0x0000 : 0x1000)));
    put16(pkt, static_cast<uint16_t>(n));
    pkt.insert(pkt.end(), frame.begin() + static_cast<long>(offset),
               frame.begin() + static_cast<long>(offset + n));
    transport_.sendAcl(pkt);
    --aclCredits_;
    ++inFlight_[handle];
    offset += n;
    first = false;
  }
}

bool HciHost::waitUntil(const std::function<bool()>& pred, Clock::time_point deadline,
                        bool cancellable) {
  for (;;) {
    if (pred()) return true;
    Item item;
    {
      std::unique_lock lock(mutex_);
      if (cancellable && stop_) throw Cancelled();
      bool ready = cv_.wait_until(lock, deadline, [&] {
        return !queue_.empty() || (cancellable && stop_);
      });
      if (!ready) return pred();
      if (queue_.empty()) continue; // stop requested; thrown above
      item = std::move(queue_.front());
      queue_.pop_front();
    }
    dispatch(item);
  }
}

void HciHost::post(std::function<void()> task) {
  {
    std::lock_guard lock(mutex_);
    queue_.push_back(Item{HciPacketType::Event, {}, std::move(task)});
  }
  cv_.notify_one();
}

void HciHost::requestStop() {
  {
    std::lock_guard lock(mutex_);
    stop_ = true;
  }
  cv_.notify_all();
}

bool HciHost::stopRequested() const {
  std::lock_guard lock(mutex_);
  return stop_;
}

void HciHost::dispatch(Item& item) {
  if (item.task) {
    item.task();
  } else if (item.type == HciPacketType::Event) {
    handleEvent(item.data);
  } else {
    handleAcl(item.data);
  }
}

void HciHost::handleEvent(const Bytes& ev) {
  if (ev.size() < 2) return;
  const uint8_t code = ev[0];
  const uint8_t* p = ev.data() + 2;
  const size_t n = ev.size() - 2;

  switch (code) {
  case kEvCommandComplete:
    if (n >= 3 && pendingOpcode_ && le16(p + 1) == *pendingOpcode_) {
      commandResult_ = Bytes(p + 3, p + n);
    }
    break;
  case kEvCommandStatus:
    if (n >= 4 && pendingOpcode_ && le16(p + 2) == *pendingOpcode_) {
      commandResult_ = Bytes{p[0]};
    }
    break;
  case kEvNumberOfCompletedPackets:
    if (n >= 1) {
      for (size_t i = 0; i < p[0] && 1 + i * 4 + 4 <= n; ++i) {
        const uint16_t handle = le16(p + 1 + i * 4) & 0x0FFF;
        const int done = le16(p + 3 + i * 4);
        aclCredits_ += done;
        inFlight_[handle] = std::max(0, inFlight_[handle] - done);
      }
    }
    break;
  case kEvDisconnectionComplete:
    if (n >= 4 && p[0] == 0) {
      const uint16_t handle = le16(p + 1) & 0x0FFF;
      aclCredits_ += inFlight_[handle];
      inFlight_.erase(handle);
      l2capRx_.erase(handle);
      if (onDisconnected) onDisconnected(handle, p[3]);
    }
    break;
  case kEvHardwareError:
    log_("Bluetooth controller reported a hardware error");
    break;
  case kEvLeMeta:
    handleLeMeta(p, n);
    break;
  default:
    break;
  }
}

void HciHost::handleLeMeta(const uint8_t* p, size_t n) {
  if (n < 1) return;
  const uint8_t sub = p[0];
  ++p;
  --n;
  if ((sub == kLeConnectionComplete && n >= 18) ||
      (sub == kLeEnhancedConnectionComplete && n >= 30)) {
    ConnectionComplete cc;
    cc.status = p[0];
    cc.handle = le16(p + 1) & 0x0FFF;
    cc.peerAddressType = p[4];
    std::copy(p + 5, p + 11, cc.peer.begin());
    if (cc.status == 0) inFlight_[cc.handle] = 0;
    if (onConnectionComplete) onConnectionComplete(cc);
  } else if (sub == kLeAdvertisingReport && n >= 1) {
    size_t count = p[0];
    size_t off = 1;
    for (size_t i = 0; i < count && off + 9 <= n; ++i) {
      AdvReport r;
      r.eventType = p[off];
      r.addressType = p[off + 1];
      std::copy(p + off + 2, p + off + 8, r.address.begin());
      const size_t len = p[off + 8];
      if (off + 9 + len + 1 > n) break;
      r.data.assign(p + off + 9, p + off + 9 + len);
      r.rssi = static_cast<int8_t>(p[off + 9 + len]);
      off += 10 + len;
      if (onAdvertisement) onAdvertisement(r);
    }
  }
}

void HciHost::handleAcl(const Bytes& pkt) {
  if (pkt.size() < 4) return;
  const uint16_t hdr = le16(pkt.data());
  const uint16_t handle = hdr & 0x0FFF;
  const int pb = (hdr >> 12) & 0x3;
  Bytes& rx = l2capRx_[handle];
  if (pb != 0x1) rx.clear(); // start of a new L2CAP frame
  rx.insert(rx.end(), pkt.begin() + 4, pkt.end());
  if (rx.size() < 4) return;
  const size_t len = le16(rx.data());
  if (rx.size() < 4 + len) return;
  const uint16_t cid = le16(rx.data() + 2);
  Bytes payload(rx.begin() + 4, rx.begin() + 4 + static_cast<long>(len));
  rx.clear();
  if (onL2cap) onL2cap(handle, cid, payload);
}

} // namespace s226::detail
