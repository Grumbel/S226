// Minimal LE-only HCI host: commands, scanning, connections and L2CAP
// framing on top of UsbTransport.
//
// Threading: everything except post() and requestStop() must be called
// from a single worker thread. Incoming HCI packets are queued by the USB
// thread and dispatched from waitUntil(), so callbacks also run on the
// worker thread.

#pragma once

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "usb_transport.hpp"

namespace s226::detail {

using Bytes = std::vector<uint8_t>;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

struct Cancelled : std::runtime_error {
  Cancelled() : std::runtime_error("cancelled") {}
};

// Connection attempt failed or an established link went away.
struct LinkLost : std::runtime_error {
  using std::runtime_error::runtime_error;
};

using Address = std::array<uint8_t, 6>; // little-endian, as on the wire

std::string addressToString(const Address& a);
std::optional<Address> addressFromString(const std::string& s);
std::string hex(const Bytes& data);

inline uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }
inline void put16(Bytes& b, uint16_t v) {
  b.push_back(static_cast<uint8_t>(v & 0xFF));
  b.push_back(static_cast<uint8_t>(v >> 8));
}

struct AdvReport {
  uint8_t eventType = 0;
  uint8_t addressType = 0;
  Address address{};
  int rssi = 0;
  Bytes data;
};

struct ConnectionComplete {
  uint8_t status = 0;
  uint16_t handle = 0;
  uint8_t peerAddressType = 0;
  Address peer{};
};

namespace opcode {
inline constexpr uint16_t SetEventMask = 0x0C01;
inline constexpr uint16_t Reset = 0x0C03;
inline constexpr uint16_t WriteLeHostSupport = 0x0C6D;
inline constexpr uint16_t ReadLocalVersion = 0x1001;
inline constexpr uint16_t ReadBufferSize = 0x1005;
inline constexpr uint16_t ReadBdAddr = 0x1009;
inline constexpr uint16_t Disconnect = 0x0406;
inline constexpr uint16_t LeSetEventMask = 0x2001;
inline constexpr uint16_t LeReadBufferSize = 0x2002;
inline constexpr uint16_t LeSetScanParameters = 0x200B;
inline constexpr uint16_t LeSetScanEnable = 0x200C;
inline constexpr uint16_t LeCreateConnection = 0x200D;
inline constexpr uint16_t LeCreateConnectionCancel = 0x200E;
inline constexpr uint16_t LeConnectionUpdate = 0x2013;
} // namespace opcode

class HciHost {
public:
  using LogFn = std::function<void(const std::string&)>;

  explicit HciHost(LogFn log);
  ~HciHost();

  // Claims the USB controller and initialises it for LE central use.
  void open(const UsbController& controller);
  void close();

  // Send a command and wait for Command Complete; returns the return
  // parameters (status first). Throws if status != 0 unless allowFailure.
  Bytes command(uint16_t opcode, const Bytes& params = {}, bool allowFailure = false);
  // Send a command answered by Command Status; returns the status.
  uint8_t commandWithStatus(uint16_t opcode, const Bytes& params);
  // Fire and forget (used from inside packet dispatch).
  void commandNoWait(uint16_t opcode, const Bytes& params);

  void sendL2cap(uint16_t handle, uint16_t cid, const Bytes& payload);

  // Dispatch incoming packets / posted tasks until pred() is true (returns
  // true) or the deadline passes (returns false). Throws Cancelled after
  // requestStop() when cancellable.
  bool waitUntil(const std::function<bool()>& pred, Clock::time_point deadline,
                 bool cancellable = true);
  void sleepFor(Clock::duration d) {
    waitUntil([] { return false; }, Clock::now() + d);
  }

  // Thread-safe.
  void post(std::function<void()> task);
  void requestStop();
  bool stopRequested() const;

  // Callbacks (worker thread).
  std::function<void(const AdvReport&)> onAdvertisement;
  std::function<void(const ConnectionComplete&)> onConnectionComplete;
  std::function<void(uint16_t handle, uint8_t reason)> onDisconnected;
  std::function<void(uint16_t handle, uint16_t cid, const Bytes& payload)> onL2cap;

private:
  struct Item {
    HciPacketType type = HciPacketType::Event;
    Bytes data;
    std::function<void()> task;
  };

  void initController();
  void sendCommandPacket(uint16_t opcode, const Bytes& params);
  void dispatch(Item& item);
  void handleEvent(const Bytes& ev);
  void handleLeMeta(const uint8_t* p, size_t n);
  void handleAcl(const Bytes& pkt);

  LogFn log_;
  UsbTransport transport_;

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<Item> queue_;
  bool stop_ = false;

  // Command matching.
  std::optional<uint16_t> pendingOpcode_;
  std::optional<Bytes> commandResult_;
  bool commandWasStatus_ = false;

  // ACL flow control.
  uint16_t aclMtu_ = 27;
  int aclCredits_ = 1;
  std::map<uint16_t, int> inFlight_;
  std::map<uint16_t, Bytes> l2capRx_; // reassembly per connection
};

} // namespace s226::detail
