#include "s226/step_rate.hpp"

#include <algorithm>

namespace s226 {

using namespace std::chrono_literals;

StepRate::StepRate(Clock::duration window) : window_(window) {}

void StepRate::reset() {
  lastTotal_.reset();
  changes_.clear();
}

void StepRate::add(Clock::time_point time, uint32_t totalSteps) {
  if (lastTotal_ && totalSteps < *lastTotal_) reset();
  lastSeen_ = time;
  if (!lastTotal_) {
    // First reading: not a change instant, only a baseline.
    lastTotal_ = totalSteps;
    return;
  }
  if (totalSteps == *lastTotal_) return;
  lastTotal_ = totalSteps;
  changes_.push_back({time, totalSteps});
  // Keep changes inside the window, but always the previous one so a rate
  // exists even when the watch updates less often than the window.
  while (changes_.size() > 2 && changes_[1].time < time - window_) changes_.pop_front();
}

std::optional<double> StepRate::stepsPerMinute() const {
  if (changes_.size() < 2) return std::nullopt;
  const Change& first = changes_.front();
  const Change& last = changes_.back();
  // Stopped: no update for well over the usual interval between updates.
  const auto interval = last.time - changes_[changes_.size() - 2].time;
  if (lastSeen_ - last.time > std::max<Clock::duration>(interval * 5 / 2, 3s)) return 0.0;
  const double minutes =
      std::chrono::duration<double, std::ratio<60>>(last.time - first.time).count();
  if (minutes <= 0) return std::nullopt;
  return (last.steps - first.steps) / minutes;
}

} // namespace s226
