#pragma once

#include <QWidget>

class WatchBridge;
class QComboBox;
class QPushButton;
class QTableWidget;
class QLabel;

class HistoryTab : public QWidget {
  Q_OBJECT
public:
  explicit HistoryTab(WatchBridge& bridge, QWidget* parent = nullptr);
  void setConnected(bool connected);

private:
  void buildUi();
  void fetch();

  WatchBridge& bridge_;
  bool connected_ = false;
  QComboBox* dayBox_ = nullptr;
  QPushButton* fetchBtn_ = nullptr;
  QTableWidget* table_ = nullptr;
  QLabel* status_ = nullptr;
};
