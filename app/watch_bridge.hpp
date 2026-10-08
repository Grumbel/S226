// Qt adapter for s226::Watch: re-emits the library's worker-thread
// callbacks as signals on the GUI thread, and provides a non-blocking
// request/response path for one-shot and multi-frame commands.

#pragma once

#include <QObject>
#include <QString>
#include <QTimer>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "s226/protocol.hpp"
#include "s226/watch.hpp"

class WatchBridge : public QObject {
  Q_OBJECT

public:
  using Bytes = s226::protocol::Bytes;
  using AcceptFn = std::function<bool(const Bytes&)>;

  explicit WatchBridge(QObject* parent = nullptr);
  ~WatchBridge() override;

  void start(const QString& controller, const QString& address);
  void stop();
  bool isConnected() const { return connected_; }

  void startBloodPressure();
  void stopBloodPressure();
  void send(Bytes command);

  // One-shot: first matching notification, or nullopt on timeout/disconnect.
  void request(Bytes command, AcceptFn accept,
               std::function<void(std::optional<Bytes>)> done,
               int timeoutMs = 3000);

  // Multi-frame until isTerminal, or timeout (delivers what was collected).
  void requestStream(Bytes command, AcceptFn accept,
                     std::function<bool(const Bytes&)> isTerminal,
                     std::function<void(std::vector<Bytes>)> done,
                     int timeoutMs = 10000);

  void cancelRequest();

signals:
  void stateChanged(s226::WatchState state, const QString& detail);
  void heartRate(int bpm);
  void bloodPressureProgress(int percent);
  void bloodPressure(int systolic, int diastolic);
  void steps(quint32 steps);
  void deviceInfo(int deviceNumber, const QString& firmware);
  void battery(int percent, int level);
  void logMessage(const QString& message);
  void musicControl(s226::protocol::MusicAction action);
  void connectedChanged(bool connected);

private:
  template <typename F>
  void toGui(F&& f) {
    QMetaObject::invokeMethod(this, std::forward<F>(f), Qt::QueuedConnection);
  }

  void onRawNotification(const Bytes& value);
  void finishRequest(bool timedOut);
  void armTimeout(int timeoutMs);

  std::unique_ptr<s226::Watch> watch_;
  bool connected_ = false;

  struct Pending {
    AcceptFn accept;
    std::function<bool(const Bytes&)> isTerminal;
    std::function<void(std::optional<Bytes>)> doneOne;
    std::function<void(std::vector<Bytes>)> doneStream;
    std::vector<Bytes> collected;
    bool stream = false;
  };
  std::optional<Pending> pending_;
  QTimer timeoutTimer_;
};
