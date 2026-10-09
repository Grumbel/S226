#pragma once

#include <QObject>
#include <QString>

class QThread;

// Forwards org.freedesktop.Notifications.Notify traffic to the watch.
//
// Correct approach (dbus-monitor / KDE Connect):
//   - private session-bus connection
//   - dbus_connection_set_route_peer_messages(true)
//   - install message filter, then BecomeMonitor (eavesdrop is deprecated)
//   - match: method_call + interface + member + path
//   - never emit Qt/BLE work from inside the libdbus filter
//   - I/O runs on a dedicated QThread so the GUI cannot freeze
class NotificationForwarder : public QObject {
  Q_OBJECT

public:
  explicit NotificationForwarder(QObject* parent = nullptr);
  ~NotificationForwarder() override;

  void setEnabled(bool on);
  bool isEnabled() const { return enabled_; }
  bool isListening() const { return listening_; }
  QString listenMode() const { return listenMode_; }

public slots:
  // Queued from the monitor thread onto the GUI thread.
  void onNotify(const QString& app, const QString& summary, const QString& body);
  void onDebug(const QString& line);
  void onListenFailed(const QString& reason);
  void onListening(const QString& mode);

signals:
  void enabledChanged(bool on);
  void notificationReceived(const QString& app, const QString& summary, const QString& body);
  void listenFailed(const QString& reason);
  void debugLog(const QString& line);

  // Internal: ask the worker to start/stop (cross-thread).
  void startWorker();
  void stopWorker();

private:
  class Worker;
  Worker* worker_ = nullptr;
  QThread* thread_ = nullptr;
  bool enabled_ = false;
  bool listening_ = false;
  QString listenMode_;
};
