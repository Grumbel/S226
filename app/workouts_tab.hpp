#pragma once

#include <QWidget>

class WatchBridge;
class QPushButton;
class QTreeWidget;
class QLabel;

class WorkoutsTab : public QWidget {
  Q_OBJECT
public:
  explicit WorkoutsTab(WatchBridge& bridge, QWidget* parent = nullptr);
  void setConnected(bool connected);

private:
  void buildUi();
  void fetch();

  WatchBridge& bridge_;
  bool connected_ = false;
  QPushButton* fetchBtn_ = nullptr;
  QTreeWidget* tree_ = nullptr;
  QLabel* status_ = nullptr;
};
