#pragma once

#include <QObject>
#include <QString>
#include <optional>

#include "s226/protocol.hpp"

// Optional helper: forward watch media keys to the active MPRIS player on
// the session bus, and pull now-playing metadata from it.
class MprisController : public QObject {
  Q_OBJECT

public:
  explicit MprisController(QObject* parent = nullptr);

  // Persisted in QSettings ("mpris/forwardKeys").
  void setForwardKeys(bool on);
  bool forwardKeys() const { return forwardKeys_; }

  // Session bus reachable and at least one org.mpris.MediaPlayer2.* name.
  bool hasPlayers() const;

  // Invoke Next / Previous / PlayPause on a suitable player when forwardKeys.
  void handleWatchAction(s226::protocol::MusicAction action);

  // Best-effort read of title/artist/album/playing/volume from a player.
  std::optional<s226::protocol::NowPlaying> currentTrack() const;

signals:
  void forwardKeysChanged(bool on);

private:
  // Prefer a Playing player; otherwise the first MPRIS name found.
  QString pickPlayerService() const;
  QStringList listPlayerServices() const;

  bool forwardKeys_ = false;
};
