#include "notification_forwarder.hpp"

#include <QSettings>
#include <QSocketNotifier>
#include <QtGlobal>

#include <dbus/dbus.h>

struct NotificationForwarder::DBusState {
  DBusConnection* conn = nullptr;
};

namespace {

const char* kMatchRule =
    "type='method_call',interface='org.freedesktop.Notifications',member='Notify'";
const char* kMatchEavesdrop =
    "type='method_call',interface='org.freedesktop.Notifications',member='Notify',eavesdrop='true'";

QString readString(DBusMessageIter* it) {
  if (dbus_message_iter_get_arg_type(it) != DBUS_TYPE_STRING) return {};
  const char* s = nullptr;
  dbus_message_iter_get_basic(it, &s);
  return s ? QString::fromUtf8(s) : QString{};
}

void skip(DBusMessageIter* it) { dbus_message_iter_next(it); }

QString stripMarkup(QString s) {
  s.replace(QLatin1String("&nbsp;"), QLatin1String(" "));
  s.replace(QLatin1String("&amp;"), QLatin1String("&"));
  s.replace(QLatin1String("&lt;"), QLatin1String("<"));
  s.replace(QLatin1String("&gt;"), QLatin1String(">"));
  s.replace(QLatin1String("&quot;"), QLatin1String("\""));
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

bool parseNotify(DBusMessage* msg, QString* app, QString* summary, QString* body, QString* err) {
  DBusMessageIter it;
  if (!dbus_message_iter_init(msg, &it)) {
    *err = QStringLiteral("empty message");
    return false;
  }

  *app = readString(&it);
  if (!dbus_message_iter_next(&it)) {
    *err = QStringLiteral("truncated after app_name");
    return false;
  }

  if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_UINT32) {
    *err = QStringLiteral("replaces_id not uint32 (type %1)")
               .arg(int(dbus_message_iter_get_arg_type(&it)));
    return false;
  }
  skip(&it);

  if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_STRING) {
    *err = QStringLiteral("app_icon not string");
    return false;
  }
  skip(&it);

  *summary = stripMarkup(readString(&it));
  if (!dbus_message_iter_next(&it)) {
    *err = QStringLiteral("truncated after summary");
    return false;
  }

  *body = stripMarkup(readString(&it));
  return !summary->isEmpty() || !body->isEmpty() || !app->isEmpty();
}

DBusHandlerResult filterMessage(DBusConnection*, DBusMessage* msg, void* userData) {
  auto* self = static_cast<NotificationForwarder*>(userData);
  if (!self || !self->isEnabled()) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

  const int type = dbus_message_get_type(msg);
  const char* iface = dbus_message_get_interface(msg);
  const char* member = dbus_message_get_member(msg);

  // BecomeMonitor can deliver a wide set of messages depending on rules;
  // only act on Notifications.Notify method calls.
  if (type != DBUS_MESSAGE_TYPE_METHOD_CALL || !iface || !member) {
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
  }
  if (qstrcmp(iface, "org.freedesktop.Notifications") != 0 || qstrcmp(member, "Notify") != 0) {
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
  }

  QString app, summary, body, err;
  if (!parseNotify(msg, &app, &summary, &body, &err)) {
    emit self->debugLog(QStringLiteral("desktop-notify: Notify parse failed: %1").arg(err));
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
  }

  emit self->debugLog(QStringLiteral("desktop-notify: seen Notify app=%1 summary=%2 body=%3")
                          .arg(app, summary, body.left(80)));
  emit self->notificationReceived(app, summary, body);
  return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

bool openPrivateBus(DBusConnection** out, QString* reason) {
  DBusError err;
  dbus_error_init(&err);
  DBusConnection* conn = dbus_bus_get_private(DBUS_BUS_SESSION, &err);
  if (!conn) {
    *reason = err.message ? QString::fromUtf8(err.message)
                          : QStringLiteral("Could not connect to the session bus");
    dbus_error_free(&err);
    return false;
  }
  dbus_connection_set_exit_on_disconnect(conn, FALSE);
  *out = conn;
  return true;
}

} // namespace

NotificationForwarder::NotificationForwarder(QObject* parent) : QObject(parent) {
  QSettings s;
  enabled_ = s.value(QStringLiteral("notify/forwardDesktop"), false).toBool();
  if (enabled_) {
    startListening();
    if (!listening_) {
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
    if (!listening_) return; // listenFailed already emitted
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

bool NotificationForwarder::tryBecomeMonitor() {
  // dbus-1 Monitoring API (preferred; does not need eavesdrop ACL).
  DBusMessage* call = dbus_message_new_method_call("org.freedesktop.DBus", "/org/freedesktop/DBus",
                                                   "org.freedesktop.DBus.Monitoring", "BecomeMonitor");
  if (!call) return false;

  const char* rules[] = {kMatchRule};
  const char* rulePtr = rules[0];
  DBusMessageIter args, array;
  dbus_message_iter_init_append(call, &args);
  if (!dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "s", &array) ||
      !dbus_message_iter_append_basic(&array, DBUS_TYPE_STRING, &rulePtr) ||
      !dbus_message_iter_close_container(&args, &array)) {
    dbus_message_unref(call);
    return false;
  }
  dbus_uint32_t flags = 0;
  if (!dbus_message_iter_append_basic(&args, DBUS_TYPE_UINT32, &flags)) {
    dbus_message_unref(call);
    return false;
  }

  DBusError err;
  dbus_error_init(&err);
  DBusMessage* reply = dbus_connection_send_with_reply_and_block(dbus_->conn, call, 3000, &err);
  dbus_message_unref(call);
  if (!reply) {
    const QString reason =
        err.message ? QString::fromUtf8(err.message) : QStringLiteral("BecomeMonitor failed");
    dbus_error_free(&err);
    emit debugLog(QStringLiteral("desktop-notify: BecomeMonitor unavailable: %1").arg(reason));
    return false;
  }
  dbus_message_unref(reply);
  listenMode_ = QStringLiteral("BecomeMonitor");
  emit debugLog(QStringLiteral("desktop-notify: listening via BecomeMonitor"));
  return true;
}

bool NotificationForwarder::tryEavesdropMatch() {
  DBusError err;
  dbus_error_init(&err);
  dbus_bus_add_match(dbus_->conn, kMatchEavesdrop, &err);
  if (dbus_error_is_set(&err)) {
    const QString reason = QString::fromUtf8(err.message);
    dbus_error_free(&err);
    emit debugLog(QStringLiteral("desktop-notify: eavesdrop match failed: %1").arg(reason));
    return false;
  }
  dbus_connection_flush(dbus_->conn);
  listenMode_ = QStringLiteral("eavesdrop-match");
  emit debugLog(QStringLiteral("desktop-notify: listening via eavesdrop match rule"));
  return true;
}

void NotificationForwarder::startListening() {
  if (listening_) return;

  dbus_ = new DBusState;
  QString reason;
  if (!openPrivateBus(&dbus_->conn, &reason)) {
    delete dbus_;
    dbus_ = nullptr;
    emit listenFailed(reason);
    return;
  }

  // Prefer BecomeMonitor; fall back to classic eavesdrop match.
  if (!tryBecomeMonitor() && !tryEavesdropMatch()) {
    dbus_connection_close(dbus_->conn);
    dbus_connection_unref(dbus_->conn);
    delete dbus_;
    dbus_ = nullptr;
    listenMode_.clear();
    emit listenFailed(tr(
        "Cannot watch notifications: BecomeMonitor and eavesdrop match both failed. "
        "Check session-bus policy and that a notification daemon is running."));
    return;
  }

  if (!dbus_connection_add_filter(dbus_->conn, filterMessage, this, nullptr)) {
    dbus_connection_close(dbus_->conn);
    dbus_connection_unref(dbus_->conn);
    delete dbus_;
    dbus_ = nullptr;
    listenMode_.clear();
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
  emit debugLog(QStringLiteral("desktop-notify: ready (mode=%1) — send a test with notify-send")
                    .arg(listenMode_));
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
      if (listenMode_ == QLatin1String("eavesdrop-match"))
        dbus_bus_remove_match(dbus_->conn, kMatchEavesdrop, nullptr);
      dbus_connection_flush(dbus_->conn);
      dbus_connection_close(dbus_->conn);
      dbus_connection_unref(dbus_->conn);
      dbus_->conn = nullptr;
    }
    delete dbus_;
    dbus_ = nullptr;
  }
  if (listening_) emit debugLog(QStringLiteral("desktop-notify: stopped"));
  listening_ = false;
  listenMode_.clear();
}

void NotificationForwarder::dispatch() {
  if (!dbus_ || !dbus_->conn) return;
  while (dbus_connection_read_write_dispatch(dbus_->conn, 0))
    ;
}
