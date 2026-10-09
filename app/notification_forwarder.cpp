#include "notification_forwarder.hpp"

#include <QMetaObject>
#include <QSettings>
#include <QSocketNotifier>
#include <QThread>
#include <QTimer>
#include <QtGlobal>

#include <dbus/dbus.h>

#include <cstring>

// KDE Connect form; BecomeMonitor treats rules as eavesdrop=true.
static const char* kNotifyMatch = "interface='org.freedesktop.Notifications',member='Notify'";

static const char* kNotifyMatchEavesdrop =
    "type='method_call',interface='org.freedesktop.Notifications',member='Notify',eavesdrop='true'";

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

// Parse freedesktop hints dict: category, urgency, transient, synchronous OSD.
void parseHints(DBusMessageIter* dictIt, QString* category, int* urgency, bool* transient,
                bool* synchronous) {
  *urgency = 1; // Normal default per spec when omitted
  *transient = false;
  *synchronous = false;
  category->clear();

  if (dbus_message_iter_get_arg_type(dictIt) != DBUS_TYPE_ARRAY) return;
  DBusMessageIter entries;
  dbus_message_iter_recurse(dictIt, &entries);
  while (dbus_message_iter_get_arg_type(&entries) == DBUS_TYPE_DICT_ENTRY) {
    DBusMessageIter entry;
    dbus_message_iter_recurse(&entries, &entry);
    if (dbus_message_iter_get_arg_type(&entry) != DBUS_TYPE_STRING) {
      dbus_message_iter_next(&entries);
      continue;
    }
    const char* key = nullptr;
    dbus_message_iter_get_basic(&entry, &key);
    if (!dbus_message_iter_next(&entry)) {
      dbus_message_iter_next(&entries);
      continue;
    }
    // Variant
    if (dbus_message_iter_get_arg_type(&entry) != DBUS_TYPE_VARIANT) {
      dbus_message_iter_next(&entries);
      continue;
    }
    DBusMessageIter var;
    dbus_message_iter_recurse(&entry, &var);
    const int t = dbus_message_iter_get_arg_type(&var);

    if (key && std::strcmp(key, "urgency") == 0 && t == DBUS_TYPE_BYTE) {
      unsigned char u = 1;
      dbus_message_iter_get_basic(&var, &u);
      *urgency = static_cast<int>(u);
    } else if (key && std::strcmp(key, "category") == 0 && t == DBUS_TYPE_STRING) {
      *category = readString(&var);
    } else if (key && std::strcmp(key, "transient") == 0 && t == DBUS_TYPE_BOOLEAN) {
      dbus_bool_t b = FALSE;
      dbus_message_iter_get_basic(&var, &b);
      *transient = b;
    } else if (key && (std::strcmp(key, "x-canonical-private-synchronous") == 0 ||
                       std::strcmp(key, "synchronous") == 0)) {
      // Volume/brightness OSDs set x-canonical-private-synchronous (string value).
      *synchronous = true;
    }

    dbus_message_iter_next(&entries);
  }
}

struct ParsedNotify {
  QString app;
  QString summary;
  QString body;
  QString category;
  int urgency = 1;
  bool transient = false;
  bool synchronous = false;
};

bool parseNotify(DBusMessage* msg, ParsedNotify* out) {
  // Signature: susssasa{sv}i
  DBusMessageIter it;
  if (!dbus_message_iter_init(msg, &it)) return false;
  out->app = readString(&it);
  if (!dbus_message_iter_next(&it)) return false;
  if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_UINT32) return false;
  skip(&it); // replaces_id
  if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_STRING) return false;
  skip(&it); // app_icon
  out->summary = stripMarkup(readString(&it));
  if (!dbus_message_iter_next(&it)) return false;
  out->body = stripMarkup(readString(&it));
  if (!dbus_message_iter_next(&it)) {
    return !out->summary.isEmpty() || !out->body.isEmpty() || !out->app.isEmpty();
  }
  // actions array — skip
  if (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_ARRAY) skip(&it);
  else
    return !out->summary.isEmpty() || !out->body.isEmpty() || !out->app.isEmpty();

  if (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_ARRAY)
    parseHints(&it, &out->category, &out->urgency, &out->transient, &out->synchronous);

  return !out->summary.isEmpty() || !out->body.isEmpty() || !out->app.isEmpty();
}

} // namespace

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
          Q_ARG(QString, QStringLiteral("BecomeMonitor and eavesdrop match both failed")));
      return;
    }

    int fd = -1;
    if (dbus_connection_get_unix_fd(conn_, &fd) && fd >= 0) {
      notifier_ = new QSocketNotifier(fd, QSocketNotifier::Read, this);
      connect(notifier_, &QSocketNotifier::activated, this, &Worker::onReadable);
    }
    timer_ = new QTimer(this);
    timer_->setInterval(250);
    connect(timer_, &QTimer::timeout, this, &Worker::onReadable);
    timer_->start();

    onReadable();
    QMetaObject::invokeMethod(owner_, "onListening", Qt::QueuedConnection, Q_ARG(QString, mode));
    QMetaObject::invokeMethod(
        owner_, "onDebug", Qt::QueuedConnection,
        Q_ARG(QString, QStringLiteral("desktop-notify: ready (mode=%1)").arg(mode)));
  }

  void stop() { teardown(); }

private slots:
  void onReadable() {
    if (!conn_) return;
    for (int i = 0; i < 64; ++i) {
      if (!dbus_connection_read_write_dispatch(conn_, 0)) break;
    }
  }

private:
  static DBusHandlerResult filter(DBusConnection*, DBusMessage* msg, void* data) {
    auto* self = static_cast<Worker*>(data);
    if (self && self->conn_ &&
        dbus_message_is_method_call(msg, "org.freedesktop.Notifications", "Notify")) {
      ParsedNotify n;
      if (parseNotify(msg, &n)) {
        QMetaObject::invokeMethod(self->owner_, "onNotify", Qt::QueuedConnection,
                                  Q_ARG(QString, n.app), Q_ARG(QString, n.summary),
                                  Q_ARG(QString, n.body), Q_ARG(QString, n.category),
                                  Q_ARG(int, n.urgency), Q_ARG(bool, n.transient),
                                  Q_ARG(bool, n.synchronous));
      } else {
        QMetaObject::invokeMethod(
            self->owner_, "onDebug", Qt::QueuedConnection,
            Q_ARG(QString, QStringLiteral("desktop-notify: Notify parse failed")));
      }
    }
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
  minUrgency_ = s.value(QStringLiteral("notify/filterMinUrgency"), int(Normal)).toInt();
  skipTransient_ = s.value(QStringLiteral("notify/filterSkipTransient"), true).toBool();
  skipSynchronous_ = s.value(QStringLiteral("notify/filterSkipSynchronous"), true).toBool();
  blockedApps_ = s.value(QStringLiteral("notify/filterBlockedApps")).toStringList();
  blockedCategories_ = s.value(QStringLiteral("notify/filterBlockedCategories")).toStringList();

  if (enabled_) {
    QTimer::singleShot(0, this, [this]() {
      if (enabled_) emit startWorker();
    });
  }
}

NotificationForwarder::~NotificationForwarder() {
  emit stopWorker();
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
    if (listening_) emit enabledChanged(true);
  } else {
    listening_ = false;
    listenMode_.clear();
    emit stopWorker();
    emit enabledChanged(false);
  }
}

void NotificationForwarder::setMinUrgency(int urgency) {
  urgency = qBound(0, urgency, 2);
  if (minUrgency_ == urgency) return;
  minUrgency_ = urgency;
  QSettings s;
  s.setValue(QStringLiteral("notify/filterMinUrgency"), minUrgency_);
  emit filtersChanged();
}

void NotificationForwarder::setSkipTransient(bool on) {
  if (skipTransient_ == on) return;
  skipTransient_ = on;
  QSettings s;
  s.setValue(QStringLiteral("notify/filterSkipTransient"), on);
  emit filtersChanged();
}

void NotificationForwarder::setSkipSynchronous(bool on) {
  if (skipSynchronous_ == on) return;
  skipSynchronous_ = on;
  QSettings s;
  s.setValue(QStringLiteral("notify/filterSkipSynchronous"), on);
  emit filtersChanged();
}

void NotificationForwarder::setBlockedApps(const QStringList& apps) {
  QStringList cleaned;
  for (QString a : apps) {
    a = a.trimmed();
    if (!a.isEmpty()) cleaned.push_back(a);
  }
  if (cleaned == blockedApps_) return;
  blockedApps_ = cleaned;
  QSettings s;
  s.setValue(QStringLiteral("notify/filterBlockedApps"), blockedApps_);
  emit filtersChanged();
}

void NotificationForwarder::setBlockedCategories(const QStringList& cats) {
  QStringList cleaned;
  for (QString c : cats) {
    c = c.trimmed();
    if (!c.isEmpty()) cleaned.push_back(c);
  }
  if (cleaned == blockedCategories_) return;
  blockedCategories_ = cleaned;
  QSettings s;
  s.setValue(QStringLiteral("notify/filterBlockedCategories"), blockedCategories_);
  emit filtersChanged();
}

bool NotificationForwarder::passesFilters(const QString& app, const QString& category, int urgency,
                                          bool transient, bool synchronous, QString* why) const {
  if (urgency < minUrgency_) {
    *why = QStringLiteral("urgency %1 < min %2").arg(urgency).arg(minUrgency_);
    return false;
  }
  if (skipTransient_ && transient) {
    *why = QStringLiteral("transient");
    return false;
  }
  if (skipSynchronous_ && synchronous) {
    *why = QStringLiteral("synchronous OSD");
    return false;
  }
  for (const QString& block : blockedApps_) {
    if (app.contains(block, Qt::CaseInsensitive)) {
      *why = QStringLiteral("blocked app ~%1").arg(block);
      return false;
    }
  }
  if (!category.isEmpty()) {
    for (const QString& block : blockedCategories_) {
      if (category.compare(block, Qt::CaseInsensitive) == 0 ||
          category.startsWith(block + QLatin1Char('.'), Qt::CaseInsensitive)) {
        *why = QStringLiteral("blocked category %1").arg(category);
        return false;
      }
    }
  }
  return true;
}

void NotificationForwarder::onNotify(const QString& app, const QString& summary, const QString& body,
                                     const QString& category, int urgency, bool transient,
                                     bool synchronous) {
  if (!enabled_) return;
  QString why;
  if (!passesFilters(app, category, urgency, transient, synchronous, &why)) {
    emit debugLog(QStringLiteral("desktop-notify: skipped (%1) app=%2 cat=%3 urg=%4 — %5")
                      .arg(why, app, category.isEmpty() ? QStringLiteral("-") : category)
                      .arg(urgency)
                      .arg(summary.left(60)));
    return;
  }
  emit debugLog(QStringLiteral("desktop-notify: forward app=%1 cat=%2 urg=%3 — %4")
                    .arg(app, category.isEmpty() ? QStringLiteral("-") : category)
                    .arg(urgency)
                    .arg(summary.left(60)));
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
