// Steps per minute from the watch's running step total.
//
// Feed it every total the watch reports (d8 poll, every second). The rate
// is measured between the moments the counter changed, so it follows the
// watch's own update rate: steps gained between the oldest and newest
// change within `window`, divided by the time between them. If the counter
// stops changing for clearly longer than its usual update interval, the
// rate is 0. No rate is reported until the counter has changed twice.

#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <optional>

namespace s226 {

class StepRate {
public:
  using Clock = std::chrono::steady_clock;

  explicit StepRate(Clock::duration window = std::chrono::seconds(10));

  void reset();
  // A lower total than before (midnight, watch reset) starts over.
  void add(Clock::time_point time, uint32_t totalSteps);
  std::optional<double> stepsPerMinute() const;

private:
  struct Change {
    Clock::time_point time;
    uint32_t steps;
  };

  Clock::duration window_;
  std::optional<uint32_t> lastTotal_;
  Clock::time_point lastSeen_;
  std::deque<Change> changes_;
};

} // namespace s226
