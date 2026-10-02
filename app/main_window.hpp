#pragma once

#include <QElapsedTimer>
#include <QMainWindow>
#include <QTimer>

#include "metronome.hpp"
#include "watch_bridge.hpp"

class BpmView;
class QCheckBox;
class QComboBox;
class QDockWidget;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QSlider;

class MainWindow : public QMainWindow {
  Q_OBJECT

public:
  explicit MainWindow(const QString& controllerOverride, QWidget* parent = nullptr);
  ~MainWindow() override;

  void connectWatch();
  void disconnectWatch();

protected:
  void closeEvent(QCloseEvent* event) override;

private:
  void buildUi();
  void refreshControllers();
  QString selectedController() const;
  void onStateChanged(s226::WatchState state, const QString& detail);
  void onHeartRate(int bpm);
  void checkStale();
  void appendLog(const QString& line);
  void toggleFullScreen();

  WatchBridge bridge_;
  Metronome metronome_;

  BpmView* view_ = nullptr;
  QComboBox* controllerBox_ = nullptr;
  QPushButton* connectButton_ = nullptr;
  QPushButton* bpButton_ = nullptr;
  QCheckBox* metronomeBox_ = nullptr;
  QSlider* volumeSlider_ = nullptr;
  QLabel* stateLabel_ = nullptr;
  QDockWidget* logDock_ = nullptr;
  QPlainTextEdit* logView_ = nullptr;

  QString controllerOverride_;
  s226::WatchState state_ = s226::WatchState::Stopped;
  bool active_ = false;
  bool bpRunning_ = false;
  QElapsedTimer sinceSample_;
  QTimer staleTimer_;
};
