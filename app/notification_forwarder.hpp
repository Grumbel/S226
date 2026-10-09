#pragma once

#include <QObject>
#include <QString>

class QSocketNotifier;

// Listens on the session bus for org.freedesktop.Notifications.Notify
// method calls (desktop notifications) and emits their content. Uses
// libdbus with an eavesdrop match so the real notification daemon still
// handles them.
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

signals:
  void enabledChanged(bool on);
  // app is the desktop app name; summary/body are UTF-8 notification text.
  void notificationReceived(const QString& app, const QString& summary, const QString& body);
  void listenFailed(const QString& reason);

private:
  void startListening();
  void stopListening();
  void onSocketActivated(QSocketDescriptor fd, QSocketNotifier::Type type);
  void dispatch();

  struct DBusState;
  DBusState* dbus_ = nullptr;
  QSocketNotifier* notifier_ = nullptr;
  bool enabled_ = false;
  bool listening_ = false;
};
