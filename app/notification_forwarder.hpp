#pragma once

#include <QObject>
#include <QString>

class QSocketNotifier;

// Listens on the session bus for org.freedesktop.Notifications.Notify
// method calls (desktop notifications) and emits their content. Prefers
// org.freedesktop.DBus.Monitoring.BecomeMonitor; falls back to an
// eavesdrop match rule.
class NotificationForwarder : public QObject {
  Q_OBJECT

public:
  explicit NotificationForwarder(QObject* parent = nullptr);
  ~NotificationForwarder() override;

  // Persisted in QSettings ("notify/forwardDesktop").
  void setEnabled(bool on);
  bool isEnabled() const { return enabled_; }

  // True after a successful bus match was installed.
  bool isListening() const { return listening_; }

  // How we are listening: "BecomeMonitor", "eavesdrop-match", or empty.
  QString listenMode() const { return listenMode_; }

signals:
  void enabledChanged(bool on);
  // app is the desktop app name; summary/body are UTF-8 notification text.
  void notificationReceived(const QString& app, const QString& summary, const QString& body);
  void listenFailed(const QString& reason);
  // Diagnostic lines for the GUI log (start, each Notify, parse errors).
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
  bool enabled_ = false;
  bool listening_ = false;
  QString listenMode_;
};
