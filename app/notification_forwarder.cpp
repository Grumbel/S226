#include "notification_forwarder.hpp"

#include <QMetaObject>
#include <QSettings>
#include <QSocketNotifier>
#include <QThread>
#include <QTimer>
#include <QtGlobal>

#include <dbus/dbus.h>

// Match rule used by dbus-monitor / KDE Connect for Notify method calls.
// BecomeMonitor treats rules as eavesdrop=true automatically.
static const char* kNotifyMatch =
    "type='method_call',"
    "interface='org.freedesktop.Notifications',"
    "member='Notify',"
    "path='/org/freedesktop/Notifications'";

static const char* kNotifyMatchEavesdrop =
    "type='method_call',"
    "interface='org.freedesktop.Notifications',"
    "member='Notify',"
    "path='/org/freedesktop/Notifications',"
    "eavesdrop='true'";

namespace {

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

bool parseNotify(DBusMessage* msg, QString* app, QString* summary, QString* body) {
  // Signature: susssasa{sv}i
  //   app_name, replaces_id, app_icon, summary, body, actions, hints, expire
  DBusMessageIter it;
  if (!dbus_message_iter_init(msg, &it)) return false;
  *app = readString(&it);
  if (!dbus_message_iter_next(&it)) return false;
  if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_UINT32) return false;
  skip(&it);
  if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_STRING) return false;
  skip(&it);
  *summary = stripMarkup(readString(&it));
  if (!dbus_message_iter_next(&it)) return false;
  *body = stripMarkup(readString(&it));
  return !summary->isEmpty() || !body->isEmpty() || !app->isEmpty();
}

} // namespace

// ---------------------------------------------------------------------------
// Worker: owns the private D-Bus connection and runs on a background thread.
// ---------------------------------------------------------------------------
class NotificationForwarder::Worker : public QObject {
  Q_OBJECT
public:
  explicit Worker(NotificationForwarder* owner) : owner_(owner) {}
  ~Worker() override { teardown(); }

public slots:
  void start() {
    if (conn_) return;
    DBusError err;
    dbus_error_init(&err);
    conn_ = dbus_bus_get_private(DBUS_BUS_SESSION, &err);
    if (!conn_) {
      const QString reason =
          err.message ? QString::fromUtf8(err.message) : QStringLiteral("session bus connect failed");
      dbus_error_free(&err);
      QMetaObject::invokeMethod(owner_, "onListenFailed", Qt::QueuedConnection, Q_ARG(QString, reason));
      return;
    }
    dbus_connection_set_exit_on_disconnect(conn_, FALSE);
    // Required so method_calls addressed to the notification daemon are
    // delivered to this monitor connection as well (KDE Connect does this).
    dbus_connection_set_route_peer_messages(conn_, TRUE);

    if (!dbus_connection_add_filter(conn_, &Worker::filter, this, nullptr)) {
      teardown();
      QMetaObject::invokeMethod(owner_, "onListenFailed", Qt::QueuedConnection,
                                Q_ARG(QString, QStringLiteral("Could not install D-Bus filter")));
      return;
    }

    QString mode;
    if (becomeMonitor()) {
      mode = QStringLiteral("BecomeMonitor");
    } else if (eavesdropMatch()) {
      mode = QStringLiteral("eavesdrop-match");
    } else {
      teardown();
      QMetaObject::invokeMethod(
          owner_, "onListenFailed", Qt::QueuedConnection,
          Q_ARG(QString,
                QStringLiteral("BecomeMonitor and eavesdrop match both failed "
                               "(is a session bus available?)")));
      return;
    }

    int fd = -1;
    if (dbus_connection_get_unix_fd(conn_, &fd) && fd >= 0) {
      notifier_ = new QSocketNotifier(fd, QSocketNotifier::Read, this);
      connect(notifier_, &QSocketNotifier::activated, this, &Worker::onReadable);
    }
    // Bound polling so a quiet socket does not starve us if the notifier
    // misses an edge.
    timer_ = new QTimer(this);
    timer_->setInterval(250);
    connect(timer_, &QTimer::timeout, this, &Worker::onReadable);
    timer_->start();

    onReadable();
    QMetaObject::invokeMethod(owner_, "onListening", Qt::QueuedConnection, Q_ARG(QString, mode));
    QMetaObject::invokeMethod(
        owner_, "onDebug", Qt::QueuedConnection,
        Q_ARG(QString, QStringLiteral("desktop-notify: ready (mode=%1) — try: notify-send 'S226' 'test'")
                           .arg(mode)));
  }

  void stop() { teardown(); }

private slots:
  void onReadable() {
    if (!conn_) return;
    // Cap work per tick to keep this thread responsive.
    for (int i = 0; i < 64; ++i) {
      if (!dbus_connection_read_write_dispatch(conn_, 0)) break;
    }
  }

private:
  static DBusHandlerResult filter(DBusConnection*, DBusMessage* msg, void* data) {
    auto* self = static_cast<Worker*>(data);
    if (!self || !self->conn_) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    if (!dbus_message_is_method_call(msg, "org.freedesktop.Notifications", "Notify"))
      return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    QString app, summary, body;
    if (!parseNotify(msg, &app, &summary, &body)) {
      QMetaObject::invokeMethod(self->owner_, "onDebug", Qt::QueuedConnection,
                                Q_ARG(QString, QStringLiteral("desktop-notify: Notify parse failed")));
      // Monitor connections must not leave messages unhandled in a way that
      // confuses the connection; mark as handled so libdbus does not try to
      // send a default reply (monitors must not send).
      return DBUS_HANDLER_RESULT_HANDLED;
    }

    QMetaObject::invokeMethod(self->owner_, "onNotify", Qt::QueuedConnection, Q_ARG(QString, app),
                              Q_ARG(QString, summary), Q_ARG(QString, body));
    // HANDLED: we are a monitor — never attempt to reply to the method call.
    return DBUS_HANDLER_RESULT_HANDLED;
  }

  bool becomeMonitor() {
    DBusMessage* call = dbus_message_new_method_call(
        DBUS_SERVICE_DBUS, DBUS_PATH_DBUS, DBUS_INTERFACE_MONITORING, "BecomeMonitor");
    if (!call) return false;

    const char* rules[] = {kNotifyMatch};
    const char** rulesPtr = rules;
    dbus_uint32_t flags = 0;
    if (!dbus_message_append_args(call, DBUS_TYPE_ARRAY, DBUS_TYPE_STRING, &rulesPtr, 1,
                                  DBUS_TYPE_UINT32, &flags, DBUS_TYPE_INVALID)) {
      dbus_message_unref(call);
      return false;
    }

    DBusError err;
    dbus_error_init(&err);
    // Block only this worker thread (not the GUI). Short timeout.
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(conn_, call, 1500, &err);
    dbus_message_unref(call);
    if (!reply) {
      const QString reason =
          err.message ? QString::fromUtf8(err.message) : QStringLiteral("BecomeMonitor failed");
      dbus_error_free(&err);
      QMetaObject::invokeMethod(
          owner_, "onDebug", Qt::QueuedConnection,
          Q_ARG(QString, QStringLiteral("desktop-notify: BecomeMonitor unavailable: %1").arg(reason)));
      return false;
    }
    dbus_message_unref(reply);
    return true;
  }

  bool eavesdropMatch() {
    DBusError err;
    dbus_error_init(&err);
    dbus_bus_add_match(conn_, kNotifyMatchEavesdrop, &err);
    if (dbus_error_is_set(&err)) {
      const QString reason = QString::fromUtf8(err.message);
      dbus_error_free(&err);
      QMetaObject::invokeMethod(
          owner_, "onDebug", Qt::QueuedConnection,
          Q_ARG(QString, QStringLiteral("desktop-notify: eavesdrop match failed: %1").arg(reason)));
      return false;
    }
    dbus_connection_flush(conn_);
    return true;
  }

  void teardown() {
    if (timer_) {
      timer_->stop();
      delete timer_;
      timer_ = nullptr;
    }
    if (notifier_) {
      notifier_->setEnabled(false);
      delete notifier_;
      notifier_ = nullptr;
    }
    if (conn_) {
      dbus_connection_remove_filter(conn_, &Worker::filter, this);
      dbus_connection_flush(conn_);
      dbus_connection_close(conn_);
      dbus_connection_unref(conn_);
      conn_ = nullptr;
    }
  }

  NotificationForwarder* owner_ = nullptr;
  DBusConnection* conn_ = nullptr;
  QSocketNotifier* notifier_ = nullptr;
  QTimer* timer_ = nullptr;
};

#include "notification_forwarder.moc"

// ---------------------------------------------------------------------------
// Public facade (GUI thread)
// ---------------------------------------------------------------------------

NotificationForwarder::NotificationForwarder(QObject* parent) : QObject(parent) {
  thread_ = new QThread(this);
  worker_ = new Worker(this);
  worker_->moveToThread(thread_);
  connect(thread_, &QThread::finished, worker_, &QObject::deleteLater);
  connect(this, &NotificationForwarder::startWorker, worker_, &Worker::start);
  connect(this, &NotificationForwarder::stopWorker, worker_, &Worker::stop);
  thread_->start();

  QSettings s;
  enabled_ = s.value(QStringLiteral("notify/forwardDesktop"), false).toBool();
  if (enabled_) {
    QTimer::singleShot(0, this, [this]() {
      if (enabled_) emit startWorker();
    });
  }
}

NotificationForwarder::~NotificationForwarder() {
  emit stopWorker();
  // Give the worker a moment to drop the connection before quitting the thread.
  if (thread_) {
    thread_->quit();
    thread_->wait(2000);
  }
}

void NotificationForwarder::setEnabled(bool on) {
  if (enabled_ == on) return;
  enabled_ = on;
  QSettings s;
  s.setValue(QStringLiteral("notify/forwardDesktop"), on);
  if (on) {
    emit startWorker();
    // If the worker was already up, onListening will not fire again.
    if (listening_) emit enabledChanged(true);
  } else {
    listening_ = false;
    listenMode_.clear();
    emit stopWorker();
    emit enabledChanged(false);
  }
}

void NotificationForwarder::onNotify(const QString& app, const QString& summary, const QString& body) {
  if (!enabled_) return;
  emit debugLog(QStringLiteral("desktop-notify: seen Notify app=%1 summary=%2 body=%3")
                    .arg(app, summary, body.left(80)));
  emit notificationReceived(app, summary, body);
}

void NotificationForwarder::onDebug(const QString& line) { emit debugLog(line); }

void NotificationForwarder::onListenFailed(const QString& reason) {
  listening_ = false;
  listenMode_.clear();
  if (enabled_) {
    enabled_ = false;
    QSettings s;
    s.setValue(QStringLiteral("notify/forwardDesktop"), false);
    emit enabledChanged(false);
  }
  emit listenFailed(reason);
  emit debugLog(QStringLiteral("desktop-notify: %1").arg(reason));
}

void NotificationForwarder::onListening(const QString& mode) {
  listening_ = true;
  listenMode_ = mode;
  if (enabled_) emit enabledChanged(true);
}
