#include "notification_forwarder.hpp"

#include <QSettings>
#include <QSocketNotifier>
#include <QtGlobal>

#include <dbus/dbus.h>

struct NotificationForwarder::DBusState {
  DBusConnection* conn = nullptr;
};

namespace {

const char* kMatch =
    "type='method_call',interface='org.freedesktop.Notifications',member='Notify',eavesdrop='true'";

QString readString(DBusMessageIter* it) {
  if (dbus_message_iter_get_arg_type(it) != DBUS_TYPE_STRING) return {};
  const char* s = nullptr;
  dbus_message_iter_get_basic(it, &s);
  return s ? QString::fromUtf8(s) : QString{};
}

void skip(DBusMessageIter* it) { dbus_message_iter_next(it); }

// Desktop bodies often contain simple markup (<b>, <i>, entities).
QString stripMarkup(QString s) {
  s.replace(QLatin1String("&nbsp;"), QLatin1String(" "));
  s.replace(QLatin1String("&amp;"), QLatin1String("&"));
  s.replace(QLatin1String("&lt;"), QLatin1String("<"));
  s.replace(QLatin1String("&gt;"), QLatin1String(">"));
  s.replace(QLatin1String("&quot;"), QLatin1String("\""));
  // Drop tags: <...>
  QString out;
  out.reserve(s.size());
  bool inTag = false;
  for (QChar c : s) {
    if (c == QLatin1Char('<')) {
      inTag = true;
      continue;
    }
    if (c == QLatin1Char('>')) {
      inTag = false;
      continue;
    }
    if (!inTag) out.append(c);
  }
  return out.simplified();
}

bool parseNotify(DBusMessage* msg, QString* app, QString* summary, QString* body) {
  DBusMessageIter it;
  if (!dbus_message_iter_init(msg, &it)) return false;

  *app = readString(&it);
  if (!dbus_message_iter_next(&it)) return false;

  if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_UINT32) return false;
  skip(&it);

  if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_STRING) return false;
  skip(&it); // app_icon

  *summary = stripMarkup(readString(&it));
  if (!dbus_message_iter_next(&it)) return false;

  *body = stripMarkup(readString(&it));

  return !summary->isEmpty() || !body->isEmpty() || !app->isEmpty();
}

DBusHandlerResult filterMessage(DBusConnection*, DBusMessage* msg, void* userData) {
  auto* self = static_cast<NotificationForwarder*>(userData);
  if (!self || !self->isEnabled()) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
  if (!dbus_message_is_method_call(msg, "org.freedesktop.Notifications", "Notify"))
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

  QString app, summary, body;
  if (parseNotify(msg, &app, &summary, &body))
    emit self->notificationReceived(app, summary, body);

  return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

} // namespace

NotificationForwarder::NotificationForwarder(QObject* parent) : QObject(parent) {
  QSettings s;
  enabled_ = s.value(QStringLiteral("notify/forwardDesktop"), false).toBool();
  if (enabled_) {
    startListening();
    if (!listening_) {
      // Preference was on but bus match failed; do not pretend we are active.
      enabled_ = false;
      s.setValue(QStringLiteral("notify/forwardDesktop"), false);
    }
  }
}

NotificationForwarder::~NotificationForwarder() { stopListening(); }

void NotificationForwarder::setEnabled(bool on) {
  if (enabled_ == on) return;
  if (on) {
    startListening();
    if (!listening_) {
      // listenFailed already emitted from startListening.
      return;
    }
    enabled_ = true;
    QSettings s;
    s.setValue(QStringLiteral("notify/forwardDesktop"), true);
    emit enabledChanged(true);
  } else {
    enabled_ = false;
    QSettings s;
    s.setValue(QStringLiteral("notify/forwardDesktop"), false);
    stopListening();
    emit enabledChanged(false);
  }
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
    emit listenFailed(
        tr("Cannot watch notifications (need eavesdrop on the session bus): %1").arg(reason));
    return;
  }

  // Ensure AddMatch is sent before we wait for Notify traffic.
  dbus_connection_flush(dbus_->conn);

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
      dbus_bus_remove_match(dbus_->conn, kMatch, nullptr);
      dbus_connection_flush(dbus_->conn);
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
  // Drain the socket and dispatch until idle so Notify copies are not left queued.
  while (dbus_connection_read_write_dispatch(dbus_->conn, 0))
    ;
}
