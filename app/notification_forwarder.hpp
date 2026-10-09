#pragma once

#include <QObject>
#include <QString>

class QSocketNotifier;
class QTimer;

// Listens on the session bus for org.freedesktop.Notifications.Notify
// method calls. Prefers BecomeMonitor; falls back to an eavesdrop match.
// D-Bus I/O is polled on a timer; Notify events are queued to the Qt
// event loop so the GUI thread never blocks inside libdbus.
class NotificationForwarder : public QObject {
  Q_OBJECT

public:
  explicit NotificationForwarder(QObject* parent = nullptr);
  ~NotificationForwarder() override;

  void setEnabled(bool on);
  bool isEnabled() const { return enabled_; }
  bool isListening() const { return listening_; }
  QString listenMode() const { return listenMode_; }

  // Called queued from the D-Bus filter (must stay on the GUI thread).
  void emitQueued(const QString& app, const QString& summary, const QString& body);

signals:
  void enabledChanged(bool on);
  void notificationReceived(const QString& app, const QString& summary, const QString& body);
  void listenFailed(const QString& reason);
  void debugLog(const QString& line);

private:
  void startListening();
  void stopListening();
  void dispatch();
  bool tryBecomeMonitor();
  bool tryEavesdropMatch();
  struct DBusState;
  DBusState* dbus_ = nullptr;
  QSocketNotifier* notifier_ = nullptr;
  QTimer* pollTimer_ = nullptr;
  bool enabled_ = false;
  bool listening_ = false;
  QString listenMode_;
};
