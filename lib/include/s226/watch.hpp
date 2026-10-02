// High-level S226 session: finds the watch, connects over a raw USB HCI
// controller, binds and streams heart-rate / blood-pressure readings.
//
//   s226::Watch watch({
//     .stateChanged = [](s226::WatchState s, const std::string& d) { ... },
//     .heartRate = [](const s226::protocol::HeartRateSample& s) { ... },
//   });
//   watch.start({.controller = "auto"});
//   ...
//   watch.stop();
//
// All callbacks are invoked on the Watch's internal worker thread.

#pragma once

#include <functional>
#include <memory>
#include <string>

#include "s226/protocol.hpp"

namespace s226 {

enum class WatchState {
  Stopped,
  OpeningController,
  Scanning,
  Connecting,
  Discovering,
  Connected,
  Error,
};

const char* toString(WatchState state);

struct WatchOptions {
  std::string controller;    // USB controller selector, see findUsbController()
  std::string address;       // optional watch address "FD:32:EF:97:4A:CD"
  bool startHeartRate = true; // start measuring as soon as connected
  bool reconnect = true;      // scan again after the link drops
};

struct WatchEvents {
  std::function<void(WatchState, const std::string& detail)> stateChanged;
  std::function<void(const protocol::HeartRateSample&)> heartRate;
  std::function<void(const protocol::BloodPressureSample&)> bloodPressure;
  // Today's steps / distance / calories, refreshed every few seconds.
  std::function<void(const protocol::ActivityTotals&)> activity;
  std::function<void(const std::string&)> log;
  // Every notification from the watch, for debugging.
  std::function<void(const protocol::Bytes&)> rawNotification;
};

class Watch {
public:
  explicit Watch(WatchEvents events);
  ~Watch();
  Watch(const Watch&) = delete;
  Watch& operator=(const Watch&) = delete;

  // Starts the worker thread. Errors are reported via stateChanged(Error).
  void start(const WatchOptions& options);
  // Stops any measurement, disconnects and gives the controller back to
  // the kernel. Blocks until done (a few hundred ms at most).
  void stop();
  bool isRunning() const;

  // Measurement control; may be called from any thread at any time. The
  // heart-rate request is remembered across reconnects. The watch ends a
  // measurement on its own after ~30-50 s; it is restarted automatically
  // while heart rate is wanted.
  void startHeartRate();
  void stopHeartRate();
  void startBloodPressure();
  void stopBloodPressure();

private:
  struct Impl;
  std::unique_ptr<Impl> d_;
};

} // namespace s226
