#include "mpris_controller.hpp"

#include <QSettings>
#include <QTimer>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDBusVariant>
#include <QVariantMap>

namespace {

constexpr const char* kMprisPrefix = "org.mpris.MediaPlayer2.";
constexpr const char* kMprisPath = "/org/mpris/MediaPlayer2";
constexpr const char* kPlayerIface = "org.mpris.MediaPlayer2.Player";
constexpr const char* kPropsIface = "org.freedesktop.DBus.Properties";

QDBusInterface playerIface(const QString& service) {
  return QDBusInterface(service, kMprisPath, kPlayerIface, QDBusConnection::sessionBus());
}

QDBusInterface propsIface(const QString& service) {
  return QDBusInterface(service, kMprisPath, kPropsIface, QDBusConnection::sessionBus());
}

QVariant getPlayerProperty(const QString& service, const QString& name) {
  QDBusInterface props = propsIface(service);
  if (!props.isValid()) return {};
  QDBusReply<QVariant> reply =
      props.call(QStringLiteral("Get"), QString::fromLatin1(kPlayerIface), name);
  if (!reply.isValid()) return {};
  const QVariant v = reply.value();
  if (v.canConvert<QDBusVariant>())
    return qvariant_cast<QDBusVariant>(v).variant();
  return v;
}

} // namespace

MprisController::MprisController(QObject* parent) : QObject(parent) {
  QSettings s;
  forwardKeys_ = s.value(QStringLiteral("mpris/forwardKeys"), false).toBool();
  autoPush_ = s.value(QStringLiteral("mpris/autoPush"), false).toBool();

  pollTimer_ = new QTimer(this);
  pollTimer_->setInterval(2000);
  connect(pollTimer_, &QTimer::timeout, this, &MprisController::pollTrack);
  if (autoPush_) pollTimer_->start();
}

void MprisController::setForwardKeys(bool on) {
  if (forwardKeys_ == on) return;
  forwardKeys_ = on;
  QSettings s;
  s.setValue(QStringLiteral("mpris/forwardKeys"), on);
  emit forwardKeysChanged(on);
}

void MprisController::setAutoPush(bool on) {
  if (autoPush_ == on) return;
  autoPush_ = on;
  QSettings s;
  s.setValue(QStringLiteral("mpris/autoPush"), on);
  if (on) {
    lastFingerprint_.clear();
    pollTimer_->start();
    pollTrack();
  } else {
    pollTimer_->stop();
    lastFingerprint_.clear();
  }
  emit autoPushChanged(on);
}

QStringList MprisController::listPlayerServices() const {
  QStringList out;
  auto* bus = QDBusConnection::sessionBus().interface();
  if (!bus) return out;
  const QDBusReply<QStringList> names = bus->registeredServiceNames();
  if (!names.isValid()) return out;
  for (const QString& n : names.value()) {
    if (n.startsWith(QLatin1String(kMprisPrefix)) && !n.endsWith(QLatin1String(".Instance")))
      out.push_back(n);
  }
  if (out.isEmpty()) {
    for (const QString& n : names.value()) {
      if (n.startsWith(QLatin1String(kMprisPrefix))) out.push_back(n);
    }
  }
  return out;
}

bool MprisController::hasPlayers() const { return !listPlayerServices().isEmpty(); }

QString MprisController::pickPlayerService() const {
  const QStringList services = listPlayerServices();
  QString fallback;
  for (const QString& svc : services) {
    const QVariant status = getPlayerProperty(svc, QStringLiteral("PlaybackStatus"));
    if (status.toString() == QLatin1String("Playing")) return svc;
    if (fallback.isEmpty()) fallback = svc;
  }
  return fallback;
}

void MprisController::handleWatchAction(s226::protocol::MusicAction action) {
  if (!forwardKeys_) return;
  const QString svc = pickPlayerService();
  if (svc.isEmpty()) return;
  QDBusInterface player = playerIface(svc);
  if (!player.isValid()) return;
  switch (action) {
  case s226::protocol::MusicAction::Next:
    player.call(QStringLiteral("Next"));
    break;
  case s226::protocol::MusicAction::Previous:
    player.call(QStringLiteral("Previous"));
    break;
  case s226::protocol::MusicAction::PlayPause:
    player.call(QStringLiteral("PlayPause"));
    break;
  }
}

std::optional<s226::protocol::NowPlaying> MprisController::currentTrack() const {
  const QString svc = pickPlayerService();
  if (svc.isEmpty()) return std::nullopt;

  s226::protocol::NowPlaying np;
  const QVariant status = getPlayerProperty(svc, QStringLiteral("PlaybackStatus"));
  np.playing = (status.toString() == QLatin1String("Playing"));

  const QVariant vol = getPlayerProperty(svc, QStringLiteral("Volume"));
  if (vol.isValid()) {
    np.volume = qBound(vol.toDouble() * 100.0);
    if (np.volume < 0) np.volume = 0;
    if (np.volume > 100) np.volume = 100;
  }

  const QVariant metaVar = getPlayerProperty(svc, QStringLiteral("Metadata"));
  const QVariantMap meta = metaVar.toMap();
  auto strField = [&](const char* key) -> std::string {
    const QVariant v = meta.value(QLatin1String(key));
    if (v.metaType().id() == QMetaType::QStringList) {
      const QStringList list = v.toStringList();
      return list.isEmpty() ? std::string{} : list.front().toStdString();
    }
    return v.toString().toStdString();
  };
  np.title = strField("xesam:title");
  np.artist = strField("xesam:artist");
  np.album = strField("xesam:album");

  if (np.title.empty() && np.artist.empty() && np.album.empty()) return std::nullopt;
  return np;
}

std::string MprisController::fingerprint(const s226::protocol::NowPlaying& np) {
  return np.title + "\n" + np.artist + "\n" + np.album + "\n" + (np.playing ? "1" : "0") + "\n" +
         std::to_string(np.volume);
}

void MprisController::pollTrack() {
  if (!autoPush_) return;
  auto np = currentTrack();
  if (!np) return;
  const std::string fp = fingerprint(*np);
  if (fp == lastFingerprint_) return;
  lastFingerprint_ = fp;
  emit trackChanged(*np);
}
