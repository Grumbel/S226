#include "watch_bridge.hpp"

WatchBridge::WatchBridge(QObject* parent) : QObject(parent) {
  timeoutTimer_.setSingleShot(true);
  connect(&timeoutTimer_, &QTimer::timeout, this, [this] { finishRequest(true); });

  s226::WatchEvents ev;
  ev.stateChanged = [this](s226::WatchState state, const std::string& detail) {
    toGui([this, state, d = QString::fromStdString(detail)] {
      const bool was = connected_;
      connected_ = (state == s226::WatchState::Connected);
      if (was != connected_) emit connectedChanged(connected_);
      if (!connected_ && pending_) finishRequest(false);
      emit stateChanged(state, d);
    });
  };
  ev.heartRate = [this](const s226::protocol::HeartRateSample& s) {
    if (s.sessionEnded) return;
    toGui([this, bpm = s.bpm] { emit heartRate(bpm); });
  };
  ev.bloodPressure = [this](const s226::protocol::BloodPressureSample& s) {
    if (s.stopped) return;
    toGui([this, s] {
      if (s.done) emit bloodPressure(s.systolic, s.diastolic);
      else emit bloodPressureProgress(s.percent);
    });
  };
  ev.activity = [this](const s226::protocol::ActivityTotals& a) {
    toGui([this, n = a.steps] { emit steps(n); });
  };
  ev.deviceInfo = [this](const s226::protocol::DeviceInfo& i) {
    toGui([this, n = i.deviceNumber, fw = QString::fromStdString(i.firmware)] {
      emit deviceInfo(n, fw);
    });
  };
  ev.battery = [this](const s226::protocol::Battery& b) {
    toGui([this, b] { emit battery(b.percent, b.level); });
  };
  ev.log = [this](const std::string& m) {
    toGui([this, m = QString::fromStdString(m)] { emit logMessage(m); });
  };
  ev.rawNotification = [this](const s226::protocol::Bytes& value) {
    toGui([this, value] { onRawNotification(value); });
  };
  ev.musicControl = [this](s226::protocol::MusicAction a) {
    toGui([this, a] { emit musicControl(a); });
  };
  watch_ = std::make_unique<s226::Watch>(std::move(ev));
}

WatchBridge::~WatchBridge() {
  cancelRequest();
  watch_->stop();
}

void WatchBridge::start(const QString& controller, const QString& address) {
  cancelRequest();
  s226::WatchOptions opts;
  opts.controller = controller.toStdString();
  opts.address = address.toStdString();
  opts.startHeartRate = true;
  opts.reconnect = true;
  watch_->start(opts);
}

void WatchBridge::stop() {
  cancelRequest();
  watch_->stop();
}

void WatchBridge::startHeartRate() {
  if (watch_) watch_->startHeartRate();
}
void WatchBridge::stopHeartRate() {
  if (watch_) watch_->stopHeartRate();
}
void WatchBridge::startBloodPressure() { watch_->startBloodPressure(); }
void WatchBridge::stopBloodPressure() { watch_->stopBloodPressure(); }

void WatchBridge::send(Bytes command) {
  if (!connected_) return;
  watch_->send(std::move(command));
}

void WatchBridge::request(Bytes command, AcceptFn accept,
                          std::function<void(std::optional<Bytes>)> done, int timeoutMs) {
  cancelRequest();
  if (!connected_) {
    if (done) done(std::nullopt);
    return;
  }
  Pending p;
  p.accept = std::move(accept);
  p.doneOne = std::move(done);
  p.stream = false;
  pending_ = std::move(p);
  armTimeout(timeoutMs);
  watch_->send(std::move(command));
}

void WatchBridge::requestStream(Bytes command, AcceptFn accept,
                                std::function<bool(const Bytes&)> isTerminal,
                                std::function<void(std::vector<Bytes>)> done, int timeoutMs) {
  cancelRequest();
  if (!connected_) {
    if (done) done({});
    return;
  }
  Pending p;
  p.accept = std::move(accept);
  p.isTerminal = std::move(isTerminal);
  p.doneStream = std::move(done);
  p.stream = true;
  pending_ = std::move(p);
  armTimeout(timeoutMs);
  watch_->send(std::move(command));
}

void WatchBridge::cancelRequest() {
  if (!pending_) return;
  timeoutTimer_.stop();
  auto p = std::move(*pending_);
  pending_.reset();
  if (p.stream) {
    if (p.doneStream) p.doneStream({});
  } else {
    if (p.doneOne) p.doneOne(std::nullopt);
  }
}

void WatchBridge::armTimeout(int timeoutMs) {
  timeoutTimer_.stop();
  if (timeoutMs > 0) timeoutTimer_.start(timeoutMs);
}

void WatchBridge::onRawNotification(const Bytes& value) {
  if (!pending_) return;
  if (!pending_->accept || !pending_->accept(value)) return;

  if (!pending_->stream) {
    auto done = std::move(pending_->doneOne);
    pending_.reset();
    timeoutTimer_.stop();
    if (done) done(value);
    return;
  }

  pending_->collected.push_back(value);
  const bool terminal = pending_->isTerminal && pending_->isTerminal(value);
  if (terminal) {
    auto done = std::move(pending_->doneStream);
    auto frames = std::move(pending_->collected);
    pending_.reset();
    timeoutTimer_.stop();
    if (done) done(std::move(frames));
  }
}

void WatchBridge::finishRequest(bool) {
  if (!pending_) return;
  auto p = std::move(*pending_);
  pending_.reset();
  timeoutTimer_.stop();
  if (p.stream) {
    if (p.doneStream) p.doneStream(std::move(p.collected));
  } else {
    if (p.doneOne) p.doneOne(std::nullopt);
  }
}
