#pragma once

#include <QObject>
#include <QString>
#include <optional>
#include <string>

#include "s226/protocol.hpp"

class QTimer;

// Optional helper: forward watch media keys to the active MPRIS player on
// the session bus, pull now-playing metadata, and optionally emit track
// changes so the GUI can push them to the watch.
class MprisController : public QObject {
  Q_OBJECT

public:
  explicit MprisController(QObject* parent = nullptr);

  // Persisted in QSettings ("mpris/forwardKeys", "mpris/autoPush").
  void setForwardKeys(bool on);
  bool forwardKeys() const { return forwardKeys_; }

  void setAutoPush(bool on);
  bool autoPush() const { return autoPush_; }

  // Session bus reachable and at least one org.mpris.MediaPlayer2.* name.
  bool hasPlayers() const;

  // Invoke Next / Previous / PlayPause on a suitable player when forwardKeys.
  void handleWatchAction(s226::protocol::MusicAction action);

  // Best-effort read of title/artist/album/playing/volume from a player.
  std::optional<s226::protocol::NowPlaying> currentTrack() const;

signals:
  void forwardKeysChanged(bool on);
  void autoPushChanged(bool on);
  // Fired when autoPush is on and the active player's track/state changes.
  void trackChanged(const s226::protocol::NowPlaying& np);

private:
  QString pickPlayerService() const;
  QStringList listPlayerServices() const;
  void pollTrack();
  static std::string fingerprint(const s226::protocol::NowPlaying& np);

  bool forwardKeys_ = false;
  bool autoPush_ = false;
  QTimer* pollTimer_ = nullptr;
  std::string lastFingerprint_;
};
