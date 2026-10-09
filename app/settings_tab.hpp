#pragma once

#include <QWidget>
#include <vector>

class WatchBridge;
class QLabel;
class QPushButton;
class QCheckBox;
class QSpinBox;
class QComboBox;
class QTimeEdit;
class QWidget;

class SettingsTab : public QWidget {
  Q_OBJECT
public:
  explicit SettingsTab(WatchBridge& bridge, QWidget* parent = nullptr);
  void setConnected(bool connected);
  void refresh();
  // Called when the tab becomes visible; loads once after each connect.
  void onShown();

private:
  void buildUi();
  void apply();
  void setBusy(bool busy);
  void updateBrightnessUi();

  WatchBridge& bridge_;
  bool connected_ = false;
  bool busy_ = false;
  bool needRefresh_ = false;

  QPushButton* refreshBtn_ = nullptr;
  QPushButton* applyBtn_ = nullptr;
  QLabel* status_ = nullptr;
  QWidget* formHost_ = nullptr;

  QCheckBox* sedentaryOn_ = nullptr;
  QTimeEdit* sedentaryStart_ = nullptr;
  QTimeEdit* sedentaryEnd_ = nullptr;
  QSpinBox* sedentaryInterval_ = nullptr;

  QCheckBox* hrAlarmOn_ = nullptr;
  QSpinBox* hrAlarmLow_ = nullptr;
  QSpinBox* hrAlarmHigh_ = nullptr;

  QSpinBox* screenOn_ = nullptr;
  QLabel* screenOnRange_ = nullptr;

  QComboBox* brightnessMode_ = nullptr;
  QSpinBox* brightnessLevel_ = nullptr;
  QSpinBox* brightnessOther_ = nullptr;
  QTimeEdit* brightnessStart_ = nullptr;
  QTimeEdit* brightnessEnd_ = nullptr;
  QLabel* brightnessMax_ = nullptr;

  QSpinBox* countdownSec_ = nullptr;
  QCheckBox* countdownShow_ = nullptr;

  QSpinBox* watchFace_ = nullptr;

  QSpinBox* personHeight_ = nullptr;
  QSpinBox* personWeight_ = nullptr;
  QSpinBox* personAge_ = nullptr;
  QComboBox* personSex_ = nullptr;
  QSpinBox* personStepGoal_ = nullptr;
  QSpinBox* personSleepGoal_ = nullptr;

  QCheckBox* weatherOn_ = nullptr;

  std::vector<QCheckBox*> featureChecks_;
  std::vector<QCheckBox*> messageChecks_;
};
