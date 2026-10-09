#pragma once

#include <QWidget>

class WatchBridge;
class QComboBox;
class QLabel;
class QPushButton;
class QTableWidget;

class SleepTab : public QWidget {
  Q_OBJECT

public:
  explicit SleepTab(WatchBridge& bridge, QWidget* parent = nullptr);

  void setConnected(bool connected);

private:
  void buildUi();
  void fetch();

  WatchBridge& bridge_;
  bool connected_ = false;
  QComboBox* dayBox_ = nullptr;
  QPushButton* fetchBtn_ = nullptr;
  QLabel* status_ = nullptr;
  QTableWidget* table_ = nullptr;
};
