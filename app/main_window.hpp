#pragma once

#include <QElapsedTimer>
#include <QMainWindow>
#include <QTimer>

#include "measurement_log.hpp"
#include "metronome.hpp"
#include "s226/step_rate.hpp"
#include "watch_bridge.hpp"

class BpmView;
class TrendGraph;
class QCheckBox;
class QComboBox;
class QDockWidget;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QSlider;
class QTabWidget;
class SettingsTab;
class AlarmsTab;
class NotifyTab;
class HistoryTab;
class WorkoutsTab;

class MainWindow : public QMainWindow {
  Q_OBJECT

public:
  MainWindow(const QString& controllerOverride, const QString& addressOverride,
             QWidget* parent = nullptr);
  ~MainWindow() override;

  void connectWatch();
  void disconnectWatch();

protected:
  void closeEvent(QCloseEvent* event) override;

private:
  void buildUi();
  void refreshControllers();
  QString selectedController() const;
  QString selectedWatch() const;
  void rememberWatch(const QString& address);
  void onStateChanged(s226::WatchState state, const QString& detail);
  void onHeartRate(int bpm);
  void checkStale();
  void updateActivity();
  void loadHistory();
  void appendLog(const QString& line);
  void toggleFullScreen();
  void setTabsEnabled(bool connected);

  WatchBridge bridge_;
  Metronome metronome_;

  QTabWidget* tabs_ = nullptr;
  BpmView* view_ = nullptr;
  TrendGraph* graph_ = nullptr;
  QComboBox* graphWindowBox_ = nullptr;
  QComboBox* controllerBox_ = nullptr;
  QComboBox* watchBox_ = nullptr;
  QPushButton* connectButton_ = nullptr;
  QPushButton* bpButton_ = nullptr;
  QCheckBox* metronomeBox_ = nullptr;
  QSlider* volumeSlider_ = nullptr;
  QLabel* stateLabel_ = nullptr;
  QLabel* batteryLabel_ = nullptr;
  QDockWidget* logDock_ = nullptr;
  QPlainTextEdit* logView_ = nullptr;

  SettingsTab* settingsTab_ = nullptr;
  AlarmsTab* alarmsTab_ = nullptr;
  NotifyTab* notifyTab_ = nullptr;
  HistoryTab* historyTab_ = nullptr;
  WorkoutsTab* workoutsTab_ = nullptr;

  QString controllerOverride_;
  s226::WatchState state_ = s226::WatchState::Stopped;
  bool active_ = false;
  bool bpRunning_ = false;
  QElapsedTimer sinceSample_;
  QTimer staleTimer_;
  s226::StepRate stepRate_;
  quint32 steps_ = 0;
  bool haveSteps_ = false;
  MeasurementLog log_;
};
