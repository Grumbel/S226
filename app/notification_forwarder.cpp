#include "notification_forwarder.hpp"

#include <QSettings>
#include <QSocketNotifier>
#include <QtGlobal>

#include <dbus/dbus.h>

#include <cstring>

struct NotificationForwarder::DBusState {
  DBusConnection* conn = nullptr;
};

namespace {

const char* kMatch =
    "type='method_call',interface='org.freedesktop.Notifications',member='Notify',eavesdrop='true'";

// Extract STRING at the current iterator position.
QString readString(DBusMessageIter* it) {
  if (dbus_message_iter_get_arg_type(it) != DBUS_TYPE_STRING) return {};
  const char* s = nullptr;
  dbus_message_iter_get_basic(it, &s);
  return s ? QString::fromUtf8(s) : QString{};
}

void skip(DBusMessageIter* it) {
  // Advance one element.
  dbus_message_iter_next(it);
}

bool parseNotify(DBusMessage* msg, QString* app, QString* summary, QString* body) {
  DBusMessageIter it;
  if (!dbus_message_iter_init(msg, &it)) return false;

  // app_name
  *app = readString(&it);
  if (!dbus_message_iter_next(&it)) return false;

  // replaces_id (uint32)
  if (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_UINT32) skip(&it);
  else return false;

  // app_icon
  if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_STRING) return false;
  skip(&it);

  // summary
  *summary = readString(&it);
  if (!dbus_message_iter_next(&it)) return false;

  // body
  *body = readString(&it);

  return !summary->isEmpty() || !body->isEmpty() || !app->isEmpty();
}

DBusHandlerResult filterMessage(DBusConnection*, DBusMessage* msg, void* userData) {
  auto* self = static_cast<NotificationForwarder*>(userData);
  if (!self || !dbus_message_is_method_call(msg, "org.freedesktop.Notifications", "Notify"))
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

  QString app, summary, body;
  if (parseNotify(msg, &app, &summary, &body))
    emit self->notificationReceived(app, summary, body);

  // We only eavesdrop; never handle the call.
  return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

} // namespace

NotificationForwarder::NotificationForwarder(QObject* parent) : QObject(parent) {
  QSettings s;
  enabled_ = s.value(QStringLiteral("notify/forwardDesktop"), false).toBool();
  if (enabled_) startListening();
}

NotificationForwarder::~NotificationForwarder() { stopListening(); }

void NotificationForwarder::setEnabled(bool on) {
  if (enabled_ == on) return;
  enabled_ = on;
  QSettings s;
  s.setValue(QStringLiteral("notify/forwardDesktop"), on);
  if (on)
    startListening();
  else
    stopListening();
  emit enabledChanged(on);
}

void NotificationForwarder::startListening() {
  if (listening_) return;

  dbus_ = new DBusState;
  DBusError err;
  dbus_error_init(&err);
  dbus_->conn = dbus_bus_get_private(DBUS_BUS_SESSION, &err);
  if (!dbus_->conn) {
    const QString reason = err.message ? QString::fromUtf8(err.message)
                                       : tr("Could not connect to the session bus");
    dbus_error_free(&err);
    delete dbus_;
    dbus_ = nullptr;
    emit listenFailed(reason);
    return;
  }
  dbus_connection_set_exit_on_disconnect(dbus_->conn, FALSE);

  dbus_error_init(&err);
  dbus_bus_add_match(dbus_->conn, kMatch, &err);
  if (dbus_error_is_set(&err)) {
    const QString reason = QString::fromUtf8(err.message);
    dbus_error_free(&err);
    dbus_connection_close(dbus_->conn);
    dbus_connection_unref(dbus_->conn);
    delete dbus_;
    dbus_ = nullptr;
    emit listenFailed(tr("Cannot watch notifications (need eavesdrop on session bus): %1").arg(reason));
    return;
  }

  if (!dbus_connection_add_filter(dbus_->conn, filterMessage, this, nullptr)) {
    dbus_connection_close(dbus_->conn);
    dbus_connection_unref(dbus_->conn);
    delete dbus_;
    dbus_ = nullptr;
    emit listenFailed(tr("Could not install D-Bus message filter"));
    return;
  }

  int fd = -1;
  if (!dbus_connection_get_unix_fd(dbus_->conn, &fd) || fd < 0) {
    stopListening();
    emit listenFailed(tr("D-Bus connection has no usable file descriptor"));
    return;
  }

  notifier_ = new QSocketNotifier(fd, QSocketNotifier::Read, this);
  connect(notifier_, &QSocketNotifier::activated, this, [this](QSocketDescriptor) { dispatch(); });

  // Dispatch anything already queued.
  dispatch();
  listening_ = true;
}

void NotificationForwarder::stopListening() {
  if (notifier_) {
    notifier_->setEnabled(false);
    delete notifier_;
    notifier_ = nullptr;
  }
  if (dbus_) {
    if (dbus_->conn) {
      dbus_connection_remove_filter(dbus_->conn, filterMessage, this);
      dbus_connection_close(dbus_->conn);
      dbus_connection_unref(dbus_->conn);
      dbus_->conn = nullptr;
    }
    delete dbus_;
    dbus_ = nullptr;
  }
  listening_ = false;
}

void NotificationForwarder::dispatch() {
  if (!dbus_ || !dbus_->conn) return;
  dbus_connection_read_write(dbus_->conn, 0);
  while (dbus_connection_get_dispatch_status(dbus_->conn) == DBUS_DISPATCH_DATA_REMAINS)
    dbus_connection_dispatch(dbus_->conn);
}
