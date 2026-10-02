// Qt adapter for s226::Watch: re-emits the library's worker-thread
// callbacks as signals on the GUI thread.

#pragma once

#include <QObject>
#include <QString>
#include <memory>

#include "s226/watch.hpp"

class WatchBridge : public QObject {
  Q_OBJECT

public:
  explicit WatchBridge(QObject* parent = nullptr);
  ~WatchBridge() override;

  void start(const QString& controller);
  void stop();

  void startBloodPressure();
  void stopBloodPressure();

signals:
  void stateChanged(s226::WatchState state, const QString& detail);
  void heartRate(int bpm); // 0 while the watch is still measuring
  void bloodPressureProgress(int percent);
  void bloodPressure(int systolic, int diastolic);
  void steps(quint32 steps); // today's step count from the watch
  void logMessage(const QString& message);

private:
  template <typename F>
  void toGui(F&& f) {
    QMetaObject::invokeMethod(this, std::forward<F>(f), Qt::QueuedConnection);
  }

  std::unique_ptr<s226::Watch> watch_;
};
