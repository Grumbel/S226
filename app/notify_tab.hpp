#pragma once

#include <QWidget>

class WatchBridge;
class QLineEdit;
class QComboBox;
class QPushButton;
class QLabel;

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

  WatchBridge& bridge_;
  bool connected_ = false;
  QLineEdit* messageEdit_ = nullptr;
  QComboBox* messageType_ = nullptr;
  QPushButton* sendBtn_ = nullptr;
  QLineEdit* callName_ = nullptr;
  QPushButton* callBtn_ = nullptr;
  QPushButton* endCallBtn_ = nullptr;
  QLabel* status_ = nullptr;
};
