#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

class QThread;

// Forwards org.freedesktop.Notifications.Notify to the watch, with optional
// filtering by urgency, category, app name, and OSD-style hints.
//
// Freedesktop hints used for filtering:
//   urgency   (byte: 0=Low, 1=Normal, 2=Critical)
//   category  (string, e.g. "email.arrived", "device", "im.received")
//   transient (bool) — short-lived banner
//   x-canonical-private-synchronous / resident — volume/brightness OSDs
class NotificationForwarder : public QObject {
  Q_OBJECT

public:
  enum Urgency : int { Low = 0, Normal = 1, Critical = 2 };

  explicit NotificationForwarder(QObject* parent = nullptr);
  ~NotificationForwarder() override;

  void setEnabled(bool on);
  bool isEnabled() const { return enabled_; }
  bool isListening() const { return listening_; }
  QString listenMode() const { return listenMode_; }

  // Filters (persisted under notify/filter*).
  void setMinUrgency(int urgency); // 0=Low (all), 1=Normal+, 2=Critical only
  int minUrgency() const { return minUrgency_; }

  void setSkipTransient(bool on);
  bool skipTransient() const { return skipTransient_; }

  void setSkipSynchronous(bool on); // volume/brightness OSD style
  bool skipSynchronous() const { return skipSynchronous_; }

  void setBlockedApps(const QStringList& apps); // case-insensitive substring match
  QStringList blockedApps() const { return blockedApps_; }

  void setBlockedCategories(const QStringList& cats); // case-insensitive exact or prefix
  QStringList blockedCategories() const { return blockedCategories_; }

public slots:
  void onNotify(const QString& app, const QString& summary, const QString& body, const QString& category,
                int urgency, bool transient, bool synchronous);
  void onDebug(const QString& line);
  void onListenFailed(const QString& reason);
  void onListening(const QString& mode);

signals:
  void enabledChanged(bool on);
  void filtersChanged();
  void notificationReceived(const QString& app, const QString& summary, const QString& body);
  void listenFailed(const QString& reason);
  void debugLog(const QString& line);
  void startWorker();
  void stopWorker();

private:
  bool passesFilters(const QString& app, const QString& category, int urgency, bool transient,
                     bool synchronous, QString* why) const;

  class Worker;
  Worker* worker_ = nullptr;
  QThread* thread_ = nullptr;
  bool enabled_ = false;
  bool listening_ = false;
  QString listenMode_;

  int minUrgency_ = Normal; // skip Low (volume ticks often use Low)
  bool skipTransient_ = true;
  bool skipSynchronous_ = true; // skip canonical/plasma OSD volume etc.
  QStringList blockedApps_;
  QStringList blockedCategories_;
};
