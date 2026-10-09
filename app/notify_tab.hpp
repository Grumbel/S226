#pragma once

#include <QWidget>

#include "s226/protocol.hpp"

class WatchBridge;
class MprisController;
class NotificationForwarder;
class QLineEdit;
class QComboBox;
class QPushButton;
class QLabel;
class QCheckBox;
class QSpinBox;
class QTableWidget;

class NotifyTab : public QWidget {
  Q_OBJECT
public:
  explicit NotifyTab(WatchBridge& bridge, MprisController* mpris,
                     NotificationForwarder* desktopNotify, QWidget* parent = nullptr);
  void setConnected(bool connected);

private:
  void buildUi();
  void sendMessage();
  void sendCall();
  void endCall();
  void pushNowPlaying();
  void pullFromMpris();
  void onMusicControl(s226::protocol::MusicAction action);
  void addContactRow();
  void removeSelectedContacts();
  void pushContacts();
  void clearContactsOnWatch();

  WatchBridge& bridge_;
  MprisController* mpris_ = nullptr;
  NotificationForwarder* desktopNotify_ = nullptr;
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
  QPushButton* musicPullBtn_ = nullptr;
  QCheckBox* mprisForward_ = nullptr;
  QCheckBox* mprisAutoPush_ = nullptr;
  QCheckBox* desktopNotifyForward_ = nullptr;
  QTableWidget* contactsTable_ = nullptr;
  QPushButton* contactAddBtn_ = nullptr;
  QPushButton* contactRemoveBtn_ = nullptr;
  QPushButton* contactPushBtn_ = nullptr;
  QPushButton* contactClearBtn_ = nullptr;
  QLabel* musicLastAction_ = nullptr;
  QLabel* status_ = nullptr;
};
