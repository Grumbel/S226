#include "s226/watch.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <thread>

#include "hci_host.hpp"
#include "s226/usb.hpp"

namespace s226 {

using namespace detail;

const char* toString(WatchState state) {
  switch (state) {
  case WatchState::Stopped: return "Stopped";
  case WatchState::OpeningController: return "Opening controller";
  case WatchState::Scanning: return "Scanning";
  case WatchState::Connecting: return "Connecting";
  case WatchState::Discovering: return "Discovering";
  case WatchState::Connected: return "Connected";
  case WatchState::Error: return "Error";
  }
  return "?";
}

namespace {

constexpr uint16_t kCidAtt = 0x0004;
constexpr uint16_t kCidLeSignaling = 0x0005;
constexpr uint16_t kCidSmp = 0x0006;

namespace att {
constexpr uint8_t ErrorRsp = 0x01;
constexpr uint8_t MtuReq = 0x02;
constexpr uint8_t MtuRsp = 0x03;
constexpr uint8_t FindInfoReq = 0x04;
constexpr uint8_t FindInfoRsp = 0x05;
constexpr uint8_t FindByTypeReq = 0x06;
constexpr uint8_t ReadByTypeReq = 0x08;
constexpr uint8_t ReadByTypeRsp = 0x09;
constexpr uint8_t ReadByGroupReq = 0x10;
constexpr uint8_t WriteReq = 0x12;
constexpr uint8_t WriteRsp = 0x13;
constexpr uint8_t Notification = 0x1B;
constexpr uint8_t Indication = 0x1D;
constexpr uint8_t Confirmation = 0x1E;
constexpr uint8_t WriteCmd = 0x52;
constexpr uint8_t ErrAttributeNotFound = 0x0A;
constexpr uint8_t ErrRequestNotSupported = 0x06;
} // namespace att

constexpr uint16_t kUuidCharacteristic = 0x2803;
constexpr uint16_t kUuidCccd = 0x2902;

std::string disconnectReason(uint8_t reason) {
  switch (reason) {
  case 0x08: return "watch out of range (supervision timeout)";
  case 0x13: return "watch closed the connection";
  case 0x16: return "connection closed locally";
  case 0x3E: return "connection could not be established";
  default: {
    char buf[48];
    std::snprintf(buf, sizeof buf, "disconnected (reason 0x%02x)", reason);
    return buf;
  }
  }
}

struct Characteristic {
  uint16_t declHandle = 0;
  uint16_t valueHandle = 0;
  Bytes uuid; // little-endian, 2 or 16 bytes
};

} // namespace

struct Watch::Impl {
  explicit Impl(WatchEvents e) : ev(std::move(e)) {}

  WatchEvents ev;
  WatchOptions opts;
  std::thread thread;
  std::atomic<bool> running{false};
  std::atomic<bool> hrWanted{false};

  std::mutex hostMutex;
  std::unique_ptr<HciHost> host;

  // ---- worker thread state ----
  bool scanning = false;
  bool initiating = false;
  bool connected = false;
  bool established = false;
  bool wake = false;
  uint16_t conn = 0;
  uint8_t lastDisconnectReason = 0;
  std::optional<AdvReport> found;
  std::optional<ConnectionComplete> connResult;
  std::optional<Bytes> attResponse;

  std::optional<Address> handlesFor;
  uint16_t notifyHandle = 0;
  uint16_t writeHandle = 0;
  uint16_t cccdHandle = 0;

  bool hrActive = false;
  Clock::time_point lastHrSample;

  void log(const std::string& msg) {
    if (ev.log) ev.log(msg);
  }
  void setState(WatchState s, const std::string& detail = {}) {
    if (ev.stateChanged) ev.stateChanged(s, detail);
  }

  void post(std::function<void()> fn) {
    std::lock_guard lock(hostMutex);
    if (host) host->post(std::move(fn));
  }

  // ------------------------------------------------------------------
  void run() {
    bool failed = false;
    try {
      setState(WatchState::OpeningController);
      if (!opts.address.empty() && !addressFromString(opts.address)) {
        throw std::runtime_error("Invalid watch address '" + opts.address +
                                 "', expected XX:XX:XX:XX:XX:XX");
      }
      std::string err;
      auto controller = findUsbController(opts.controller, &err);
      if (!controller) throw std::runtime_error(err);
      log("Using " + controller->displayName());
      host->onAdvertisement = [this](const AdvReport& r) { onAdvertisement(r); };
      host->onConnectionComplete = [this](const ConnectionComplete& c) { connResult = c; };
      host->onDisconnected = [this](uint16_t h, uint8_t reason) { onDisconnected(h, reason); };
      host->onL2cap = [this](uint16_t h, uint16_t cid, const Bytes& p) { onL2cap(h, cid, p); };
      host->open(*controller);

      for (;;) {
        try {
          session();
        } catch (const LinkLost& e) {
          log(e.what());
          dropLink();
          if (established && !opts.reconnect) break;
          established = false;
          host->sleepFor(1s);
        }
      }
    } catch (const Cancelled&) {
    } catch (const std::exception& e) {
      failed = true;
      log(std::string("Error: ") + e.what());
      setState(WatchState::Error, e.what());
    }
    shutdown();
    if (!failed) setState(WatchState::Stopped);
  }

  void session() {
    setState(WatchState::Scanning, "Wake the watch (press its button) to make it advertise");
    const AdvReport target = scan();

    const std::string addr = addressToString(target.address);
    setState(WatchState::Connecting, addr);
    connect(target);

    setState(WatchState::Discovering, addr);
    discover(target.address);

    // Phone order: enable notifications, then 0xA1 bind within ~200 ms.
    Bytes rsp = attRequest({att::WriteReq, static_cast<uint8_t>(cccdHandle),
                            static_cast<uint8_t>(cccdHandle >> 8), 0x01, 0x00});
    if (rsp[0] != att::WriteRsp) throw LinkLost("Enabling notifications failed");
    writeCommand(protocol::bindPacketNow());

    established = true;
    hrActive = false;
    setState(WatchState::Connected, addr);
    log("Connected to S226 " + addr);

    writeCommand(protocol::keepalive()); // also fetches today's step count
    // H-Band polls every 3 s; polling every second times step-counter
    // updates closely enough for StepRate.
    auto nextKeepalive = Clock::now() + 1s;
    for (;;) {
      wake = false;
      host->waitUntil([this] { return wake || !connected; }, Clock::now() + 250ms);
      if (!connected) throw LinkLost(disconnectReason(lastDisconnectReason));
      const auto now = Clock::now();
      if (now >= nextKeepalive) {
        writeCommand(protocol::keepalive());
        nextKeepalive = now + 1s;
      }
      applyHeartRate(now);
    }
  }

  void applyHeartRate(Clock::time_point now) {
    if (hrWanted) {
      // Also restarts after the watch ends a session on its own.
      if (!hrActive || now - lastHrSample > 5s) {
        if (hrActive) log("Restarting heart-rate measurement");
        writeCommand(protocol::heartRateStart());
        hrActive = true;
        lastHrSample = now;
      }
    } else if (hrActive) {
      writeCommand(protocol::heartRateStop());
      hrActive = false;
    }
  }

  // ------------------------------------------------------------------
  bool isS226(const AdvReport& r) const {
    if (!opts.address.empty()) {
      auto want = addressFromString(opts.address);
      return want && *want == r.address;
    }
    for (size_t i = 0; i + 1 < r.data.size();) {
      const size_t len = r.data[i];
      if (len == 0 || i + 1 + len > r.data.size()) break;
      const uint8_t type = r.data[i + 1];
      const uint8_t* d = r.data.data() + i + 2;
      const size_t n = len - 1;
      if ((type == 0x08 || type == 0x09) &&
          std::string(reinterpret_cast<const char*>(d), n).find("S226") != std::string::npos) {
        return true;
      }
      if (type == 0xFF && n >= 2 && le16(d) == protocol::kManufacturerId) return true;
      i += 1 + len;
    }
    return false;
  }

  void onAdvertisement(const AdvReport& r) {
    if (!scanning || found) return;
    if (isS226(r)) found = r;
  }

  AdvReport scan() {
    found.reset();
    // Active scan, 60 ms interval = window (continuous): the watch only
    // advertises for short bursts.
    host->command(opcode::LeSetScanParameters, {0x01, 0x60, 0x00, 0x60, 0x00, 0x00, 0x00});
    host->command(opcode::LeSetScanEnable, {0x01, 0x00});
    scanning = true;
    while (!host->waitUntil([this] { return found.has_value(); }, Clock::now() + 1h)) {
    }
    host->command(opcode::LeSetScanEnable, {0x00, 0x00});
    scanning = false;
    log("Found S226 " + addressToString(found->address) + " RSSI " +
        std::to_string(found->rssi));
    return *found;
  }

  void connect(const AdvReport& target) {
    connResult.reset();
    Bytes p;
    put16(p, 0x0060); // scan interval
    put16(p, 0x0060); // scan window
    p.push_back(0x00);                // no filter accept list
    p.push_back(target.addressType);
    p.insert(p.end(), target.address.begin(), target.address.end());
    p.push_back(0x00);                // own address: public
    put16(p, 0x0018);                 // interval min 30 ms
    put16(p, 0x0028);                 // interval max 50 ms
    put16(p, 0x0000);                 // latency
    put16(p, 0x0190);                 // supervision timeout 4 s
    put16(p, 0x0000);
    put16(p, 0x0000);
    const uint8_t status = host->commandWithStatus(opcode::LeCreateConnection, p);
    if (status != 0) {
      throw LinkLost("Connect request rejected by controller (status " +
                     std::to_string(status) + ")");
    }
    initiating = true;
    if (!host->waitUntil([this] { return connResult.has_value(); }, Clock::now() + 8s)) {
      host->command(opcode::LeCreateConnectionCancel, {}, /*allowFailure=*/true);
      host->waitUntil([this] { return connResult.has_value(); }, Clock::now() + 2s, false);
    }
    initiating = false;
    if (!connResult || connResult->status != 0) {
      throw LinkLost("Watch did not accept the connection, retrying");
    }
    conn = connResult->handle;
    connected = true;
  }

  void onDisconnected(uint16_t handle, uint8_t reason) {
    if (connected && handle == conn) {
      connected = false;
      lastDisconnectReason = reason;
    }
  }

  void dropLink() {
    scanning = false;
    if (!connected) return;
    try {
      host->commandWithStatus(opcode::Disconnect,
                              {static_cast<uint8_t>(conn), static_cast<uint8_t>(conn >> 8), 0x13});
      host->waitUntil([this] { return !connected; }, Clock::now() + 2s, false);
    } catch (const std::exception&) {
    }
    connected = false;
  }

  void shutdown() {
    try {
      if (connected) {
        if (hrActive) writeCommand(protocol::heartRateStop());
        dropLink();
      }
      if (initiating) host->command(opcode::LeCreateConnectionCancel, {}, true);
      if (scanning) host->command(opcode::LeSetScanEnable, {0x00, 0x00}, true);
    } catch (const std::exception&) {
    }
    host->close();
  }

  // ------------------------------------------------------------------
  // ATT client (and the bare minimum of an ATT server so the watch's own
  // requests get an answer instead of timing out).

  Bytes attRequest(const Bytes& pdu) {
    if (!connected) throw LinkLost(disconnectReason(lastDisconnectReason));
    attResponse.reset();
    host->sendL2cap(conn, kCidAtt, pdu);
    const bool ok =
        host->waitUntil([this] { return attResponse.has_value() || !connected; },
                        Clock::now() + 10s);
    if (!connected) throw LinkLost(disconnectReason(lastDisconnectReason));
    if (!ok) throw LinkLost("Watch stopped answering");
    return *attResponse;
  }

  void writeCommand(const Bytes& value) {
    if (!connected) return;
    Bytes pdu{att::WriteCmd, static_cast<uint8_t>(writeHandle),
              static_cast<uint8_t>(writeHandle >> 8)};
    pdu.insert(pdu.end(), value.begin(), value.end());
    host->sendL2cap(conn, kCidAtt, pdu);
  }

  void discover(const Address& peer) {
    if (handlesFor && *handlesFor == peer) return; // same watch, same firmware

    const auto notifyUuid = protocol::uuidToAtt(protocol::kNotifyUuid);
    const auto writeUuid = protocol::uuidToAtt(protocol::kWriteUuid);
    auto is = [](const Bytes& u, const std::array<uint8_t, 16>& want) {
      return u.size() == 16 && std::equal(u.begin(), u.end(), want.begin());
    };

    std::vector<Characteristic> chars;
    uint16_t start = 1;
    for (;;) {
      Bytes rsp = attRequest({att::ReadByTypeReq, static_cast<uint8_t>(start),
                              static_cast<uint8_t>(start >> 8), 0xFF, 0xFF,
                              kUuidCharacteristic & 0xFF, kUuidCharacteristic >> 8});
      if (rsp[0] != att::ReadByTypeRsp || rsp.size() < 2) break;
      const size_t len = rsp[1];
      if (len < 7) break;
      uint16_t last = start;
      for (size_t i = 2; i + len <= rsp.size(); i += len) {
        Characteristic c;
        c.declHandle = le16(&rsp[i]);
        c.valueHandle = le16(&rsp[i + 3]);
        c.uuid.assign(rsp.begin() + static_cast<long>(i + 5),
                      rsp.begin() + static_cast<long>(i + len));
        last = c.declHandle;
        chars.push_back(std::move(c));
      }
      if (last == 0xFFFF) break;
      start = static_cast<uint16_t>(last + 1);
    }

    notifyHandle = writeHandle = cccdHandle = 0;
    uint16_t notifyEnd = 0xFFFF;
    for (size_t i = 0; i < chars.size(); ++i) {
      if (is(chars[i].uuid, notifyUuid)) {
        notifyHandle = chars[i].valueHandle;
        if (i + 1 < chars.size()) notifyEnd = static_cast<uint16_t>(chars[i + 1].declHandle - 1);
      }
      if (is(chars[i].uuid, writeUuid)) writeHandle = chars[i].valueHandle;
    }
    if (!notifyHandle || !writeHandle) {
      throw std::runtime_error("Device has no S226 vendor service (f008)");
    }

    // Find the CCCD among the notify characteristic's descriptors.
    start = static_cast<uint16_t>(notifyHandle + 1);
    while (!cccdHandle && start <= notifyEnd) {
      Bytes rsp = attRequest({att::FindInfoReq, static_cast<uint8_t>(start),
                              static_cast<uint8_t>(start >> 8),
                              static_cast<uint8_t>(notifyEnd),
                              static_cast<uint8_t>(notifyEnd >> 8)});
      if (rsp[0] != att::FindInfoRsp || rsp.size() < 2) break;
      const size_t entry = rsp[1] == 1 ? 4 : 18;
      uint16_t last = start;
      for (size_t i = 2; i + entry <= rsp.size(); i += entry) {
        last = le16(&rsp[i]);
        if (entry == 4 && le16(&rsp[i + 2]) == kUuidCccd) {
          cccdHandle = last;
          break;
        }
      }
      if (last == 0xFFFF) break;
      start = static_cast<uint16_t>(last + 1);
    }
    if (!cccdHandle) cccdHandle = static_cast<uint16_t>(notifyHandle + 1);

    char buf[96];
    std::snprintf(buf, sizeof buf, "GATT: notify 0x%04x (CCCD 0x%04x), write 0x%04x",
                  notifyHandle, cccdHandle, writeHandle);
    log(buf);
    handlesFor = peer;
  }

  void onL2cap(uint16_t handle, uint16_t cid, const Bytes& p) {
    if (handle != conn || p.empty()) return;
    if (cid == kCidAtt) {
      onAtt(p);
    } else if (cid == kCidLeSignaling && p.size() >= 4) {
      onSignaling(p);
    } else if (cid == kCidSmp) {
      // No pairing: the watch does not use it, H-Band never pairs.
      host->sendL2cap(conn, kCidSmp, {0x05, 0x05}); // Pairing Failed: not supported
    }
  }

  void onAtt(const Bytes& p) {
    const uint8_t op = p[0];
    if ((op == att::Notification || op == att::Indication) && p.size() >= 3) {
      if (op == att::Indication) host->sendL2cap(conn, kCidAtt, {att::Confirmation});
      if (le16(&p[1]) == notifyHandle) onNotification(Bytes(p.begin() + 3, p.end()));
      return;
    }
    if (op == att::MtuReq) {
      host->sendL2cap(conn, kCidAtt, {att::MtuRsp, 23, 0});
      return;
    }
    if (op & 0x40) return; // commands need no answer
    if (op == att::ErrorRsp || (op & 1)) {
      attResponse = p;
      return;
    }
    // A request to our (empty) GATT server.
    const bool discovery = op == att::FindInfoReq || op == att::FindByTypeReq ||
                           op == att::ReadByTypeReq || op == att::ReadByGroupReq;
    const uint8_t lo = discovery && p.size() >= 3 ? p[1] : 0;
    const uint8_t hi = discovery && p.size() >= 3 ? p[2] : 0;
    host->sendL2cap(conn, kCidAtt,
                    {att::ErrorRsp, op, lo, hi,
                     discovery ? att::ErrAttributeNotFound : att::ErrRequestNotSupported});
  }

  void onSignaling(const Bytes& p) {
    const uint8_t code = p[0];
    const uint8_t id = p[1];
    if (code == 0x12 && p.size() >= 12) {
      // Connection Parameter Update Request: accept and apply.
      host->sendL2cap(conn, kCidLeSignaling, {0x13, id, 0x02, 0x00, 0x00, 0x00});
      Bytes cmd{static_cast<uint8_t>(conn), static_cast<uint8_t>(conn >> 8)};
      cmd.insert(cmd.end(), p.begin() + 4, p.begin() + 12);
      put16(cmd, 0);
      put16(cmd, 0);
      host->commandNoWait(opcode::LeConnectionUpdate, cmd);
    } else if (code != 0x01 && code != 0x13) {
      host->sendL2cap(conn, kCidLeSignaling, {0x01, id, 0x02, 0x00, 0x00, 0x00});
    }
  }

  void onNotification(const Bytes& value) {
    if (ev.rawNotification) ev.rawNotification(value);
    if (auto hr = protocol::decodeHeartRate(value)) {
      if (!hrWanted) return; // watch keeps streaming a while after d0 00
      if (hr->sessionEnded) {
        // Restart about a second after the watch ends a session.
        lastHrSample = Clock::now() - 4s;
      } else {
        lastHrSample = Clock::now();
      }
      if (ev.heartRate) ev.heartRate(*hr);
    } else if (auto act = protocol::decodeActivity(value)) {
      if (ev.activity) ev.activity(*act);
    } else if (auto bp = protocol::decodeBloodPressure(value)) {
      if (ev.bloodPressure) ev.bloodPressure(*bp);
    }
  }
};

Watch::Watch(WatchEvents events) : d_(std::make_unique<Impl>(std::move(events))) {}

Watch::~Watch() { stop(); }

void Watch::start(const WatchOptions& options) {
  stop();
  d_->opts = options;
  d_->hrWanted = options.startHeartRate;
  d_->handlesFor.reset();
  d_->established = d_->connected = d_->scanning = d_->initiating = false;
  {
    std::lock_guard lock(d_->hostMutex);
    d_->host = std::make_unique<HciHost>([this](const std::string& m) { d_->log(m); });
  }
  d_->running = true;
  d_->thread = std::thread([this] {
    d_->run();
    d_->running = false;
  });
}

void Watch::stop() {
  {
    std::lock_guard lock(d_->hostMutex);
    if (d_->host) d_->host->requestStop();
  }
  if (d_->thread.joinable()) d_->thread.join();
  std::lock_guard lock(d_->hostMutex);
  d_->host.reset();
}

bool Watch::isRunning() const { return d_->running; }

void Watch::startHeartRate() {
  d_->hrWanted = true;
  d_->post([this] { d_->wake = true; });
}

void Watch::stopHeartRate() {
  d_->hrWanted = false;
  d_->post([this] { d_->wake = true; });
}

void Watch::startBloodPressure() {
  d_->post([this] { d_->writeCommand(protocol::bloodPressureStart()); });
}

void Watch::stopBloodPressure() {
  d_->post([this] { d_->writeCommand(protocol::bloodPressureStop()); });
}

} // namespace s226
