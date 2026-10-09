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
class QAction;
class QCheckBox;
class QComboBox;
class QDockWidget;
class QLabel;
class QMenu;
class QPlainTextEdit;
class QPushButton;
class QSlider;
class QSystemTrayIcon;
class QTabWidget;
class SettingsTab;
class AlarmsTab;
class NotifyTab;
class HistoryTab;
class WorkoutsTab;
class SleepTab;
class MprisController;

class MainWindow : public QMainWindow {
  Q_OBJECT

public:
  MainWindow(const QString& controllerOverride, const QString& addressOverride,
             QWidget* parent = nullptr);
  ~MainWindow() override;

  void connectWatch();
  void disconnectWatch();
  MprisController* mpris() const { return mpris_; }

protected:
  void closeEvent(QCloseEvent* event) override;
  void changeEvent(QEvent* event) override;

private:
  void buildUi();
  void buildTray();
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
  void updateTray();
  void showFromTray();
  void quitApp();

  WatchBridge bridge_;
  Metronome metronome_;
  MprisController* mpris_ = nullptr;

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
  SleepTab* sleepTab_ = nullptr;

  QSystemTrayIcon* tray_ = nullptr;
  QMenu* trayMenu_ = nullptr;
  QAction* trayShowAction_ = nullptr;
  QAction* trayConnectAction_ = nullptr;
  QAction* trayQuitAction_ = nullptr;
  bool quitting_ = false;
  int lastBpm_ = 0;

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
