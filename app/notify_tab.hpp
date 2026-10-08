#pragma once

#include <QWidget>

#include "s226/protocol.hpp"

class WatchBridge;
class QLineEdit;
class QComboBox;
class QPushButton;
class QLabel;
class QCheckBox;
class QSpinBox;

class NotifyTab : public QWidget {
  Q_OBJECT
public:
  explicit NotifyTab(WatchBridge& bridge, QWidget* parent = nullptr);
  void setConnected(bool connected);

private:
  void buildUi();
  void sendMessage();
  void sendCall();
  void endCall();
  void pushNowPlaying();
  void onMusicControl(s226::protocol::MusicAction action);

  WatchBridge& bridge_;
  bool connected_ = false;
  QLineEdit* messageEdit_ = nullptr;
  QComboBox* messageType_ = nullptr;
  QPushButton* sendBtn_ = nullptr;
  QLineEdit* callName_ = nullptr;
  QPushButton* callBtn_ = nullptr;
  QPushButton* endCallBtn_ = nullptr;
  QLineEdit* musicTitle_ = nullptr;
  QLineEdit* musicArtist_ = nullptr;
  QLineEdit* musicAlbum_ = nullptr;
  QCheckBox* musicPlaying_ = nullptr;
  QSpinBox* musicVolume_ = nullptr;
  QPushButton* musicPushBtn_ = nullptr;
  QLabel* musicLastAction_ = nullptr;
  QLabel* status_ = nullptr;
};
