#pragma once

#include <QWidget>
#include <vector>
#include "s226/protocol.hpp"

class WatchBridge;
class QTableWidget;
class QPushButton;
class QLabel;

class AlarmsTab : public QWidget {
  Q_OBJECT
public:
  explicit AlarmsTab(WatchBridge& bridge, QWidget* parent = nullptr);
  void setConnected(bool connected);
  void refresh();
  void onShown();

private:
  void buildUi();
  void addAlarm();
  void editAlarm();
  void deleteAlarm();
  void fillTable(const std::vector<s226::protocol::Alarm>& alarms);

  WatchBridge& bridge_;
  bool connected_ = false;
  bool needRefresh_ = false;
  QTableWidget* table_ = nullptr;
  QPushButton* refreshBtn_ = nullptr;
  QPushButton* addBtn_ = nullptr;
  QPushButton* editBtn_ = nullptr;
  QPushButton* deleteBtn_ = nullptr;
  QLabel* status_ = nullptr;
  std::vector<s226::protocol::Alarm> alarms_;
};
