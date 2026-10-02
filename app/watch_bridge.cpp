#include "watch_bridge.hpp"

WatchBridge::WatchBridge(QObject* parent) : QObject(parent) {
  s226::WatchEvents ev;
  ev.stateChanged = [this](s226::WatchState state, const std::string& detail) {
    toGui([this, state, d = QString::fromStdString(detail)] { emit stateChanged(state, d); });
  };
  ev.heartRate = [this](const s226::protocol::HeartRateSample& s) {
    if (s.sessionEnded) return; // the library restarts the measurement
    toGui([this, bpm = s.bpm] { emit heartRate(bpm); });
  };
  ev.bloodPressure = [this](const s226::protocol::BloodPressureSample& s) {
    if (s.stopped) return;
    toGui([this, s] {
      if (s.done) {
        emit bloodPressure(s.systolic, s.diastolic);
      } else {
        emit bloodPressureProgress(s.percent);
      }
    });
  };
  ev.activity = [this](const s226::protocol::ActivityTotals& a) {
    toGui([this, n = a.steps] { emit steps(n); });
  };
  ev.log = [this](const std::string& m) {
    toGui([this, m = QString::fromStdString(m)] { emit logMessage(m); });
  };
  watch_ = std::make_unique<s226::Watch>(std::move(ev));
}

WatchBridge::~WatchBridge() {
  // Join the worker before the callbacks' target goes away.
  watch_->stop();
}

void WatchBridge::start(const QString& controller) {
  s226::WatchOptions opts;
  opts.controller = controller.toStdString();
  opts.startHeartRate = true;
  opts.reconnect = true;
  watch_->start(opts);
}

void WatchBridge::stop() { watch_->stop(); }

void WatchBridge::startBloodPressure() { watch_->startBloodPressure(); }

void WatchBridge::stopBloodPressure() { watch_->stopBloodPressure(); }
