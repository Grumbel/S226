#include "main_window.hpp"

#include <QAction>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDockWidget>
#include <QLabel>
#include <QLocale>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QShortcut>
#include <QSlider>
#include <QStatusBar>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>

#include "bpm_view.hpp"
#include "trend_graph.hpp"
#include "s226/usb.hpp"

namespace {

constexpr int kStaleMs = 6000;

QString stateText(s226::WatchState state, const QString& detail) {
  using S = s226::WatchState;
  switch (state) {
  case S::Stopped: return MainWindow::tr("Disconnected");
  case S::OpeningController: return MainWindow::tr("Opening Bluetooth controller...");
  case S::Scanning: return MainWindow::tr("Searching for the watch - press a button on it");
  case S::Connecting: return MainWindow::tr("Connecting to %1...").arg(detail);
  case S::Discovering: return MainWindow::tr("Setting up %1...").arg(detail);
  case S::Connected: return MainWindow::tr("Connected to %1").arg(detail);
  case S::Error: return MainWindow::tr("Error: %1").arg(detail);
  }
  return {};
}

} // namespace

MainWindow::MainWindow(const QString& controllerOverride, QWidget* parent)
    : QMainWindow(parent), controllerOverride_(controllerOverride) {
  setWindowTitle(tr("S226 Heart Rate"));
  buildUi();
  refreshControllers();

  connect(&bridge_, &WatchBridge::stateChanged, this, &MainWindow::onStateChanged);
  connect(&bridge_, &WatchBridge::heartRate, this, &MainWindow::onHeartRate);
  connect(&bridge_, &WatchBridge::logMessage, this, &MainWindow::appendLog);
  connect(&bridge_, &WatchBridge::bloodPressureProgress, this, [this](int pct) {
    view_->setSecondary(tr("Blood pressure %1%").arg(pct));
  });
  connect(&bridge_, &WatchBridge::bloodPressure, this, [this](int sys, int dia) {
    view_->setSecondary(tr("Blood pressure %1/%2 mmHg").arg(sys).arg(dia));
    appendLog(tr("Blood pressure: %1/%2 mmHg").arg(sys).arg(dia));
    bpRunning_ = false;
    bpButton_->setText(tr("Blood pressure"));
  });

  connect(&bridge_, &WatchBridge::steps, this, [this](quint32 steps) {
    steps_ = steps;
    haveSteps_ = true;
    stepRate_.add(s226::StepRate::Clock::now(), steps);
    if (auto spm = stepRate_.stepsPerMinute()) {
      const QDateTime now = QDateTime::currentDateTime();
      graph_->addSample(TrendGraph::Spm, now.toMSecsSinceEpoch(), *spm);
      log_.write(now, std::nullopt, qRound(*spm));
    }
    updateActivity();
  });

  connect(&metronome_, &Metronome::beat, view_, &BpmView::pulse);

  staleTimer_.setInterval(1000);
  connect(&staleTimer_, &QTimer::timeout, this, &MainWindow::checkStale);
  staleTimer_.start();

  QSettings settings;
  restoreGeometry(settings.value("geometry").toByteArray());
  const int graphIdx = graphWindowBox_->findData(settings.value("graphWindow", 300).toInt());
  graphWindowBox_->setCurrentIndex(graphIdx >= 0 ? graphIdx : 1);
  graph_->setWindowSeconds(graphWindowBox_->currentData().toInt());
  loadHistory();
  restoreState(settings.value("windowState").toByteArray());
  metronomeBox_->setChecked(settings.value("metronome", false).toBool());
  volumeSlider_->setValue(settings.value("volume", 60).toInt());

  onStateChanged(s226::WatchState::Stopped, {});
}

MainWindow::~MainWindow() = default;

void MainWindow::buildUi() {
  auto* central = new QWidget(this);
  auto* layout = new QVBoxLayout(central);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);
  view_ = new BpmView(central);
  graph_ = new TrendGraph(central);
  layout->addWidget(view_, 3);
  layout->addWidget(graph_, 1);
  setCentralWidget(central);

  auto* bar = addToolBar(tr("Main"));
  bar->setObjectName("mainToolBar");
  bar->setMovable(false);

  bar->addWidget(new QLabel(tr("Controller:"), bar));
  controllerBox_ = new QComboBox(bar);
  controllerBox_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
  controllerBox_->setMaximumWidth(420);
  bar->addWidget(controllerBox_);

  auto* refresh = new QToolButton(bar);
  refresh->setIcon(QIcon::fromTheme("view-refresh"));
  refresh->setText(QStringLiteral("↻"));
  refresh->setToolTip(tr("Look for USB Bluetooth controllers again"));
  connect(refresh, &QToolButton::clicked, this, &MainWindow::refreshControllers);
  bar->addWidget(refresh);

  connectButton_ = new QPushButton(tr("Connect"), bar);
  connect(connectButton_, &QPushButton::clicked, this, [this] {
    if (active_) {
      disconnectWatch();
    } else {
      connectWatch();
    }
  });
  bar->addWidget(connectButton_);
  bar->addSeparator();

  metronomeBox_ = new QCheckBox(tr("&Metronome"), bar);
  metronomeBox_->setToolTip(tr("Click along with the heart rate (M)"));
  connect(metronomeBox_, &QCheckBox::toggled, &metronome_, &Metronome::setSoundEnabled);
  bar->addWidget(metronomeBox_);

  volumeSlider_ = new QSlider(Qt::Horizontal, bar);
  volumeSlider_->setRange(0, 100);
  volumeSlider_->setFixedWidth(100);
  volumeSlider_->setToolTip(tr("Metronome volume"));
  connect(volumeSlider_, &QSlider::valueChanged, this,
          [this](int v) { metronome_.setVolume(v / 100.0); });
  bar->addWidget(volumeSlider_);
  bar->addSeparator();

  bpButton_ = new QPushButton(tr("Blood pressure"), bar);
  bpButton_->setToolTip(tr("Start a blood-pressure measurement on the watch"));
  connect(bpButton_, &QPushButton::clicked, this, [this] {
    if (bpRunning_) {
      bridge_.stopBloodPressure();
      bpRunning_ = false;
      bpButton_->setText(tr("Blood pressure"));
      view_->setSecondary({});
    } else {
      bridge_.startBloodPressure();
      bpRunning_ = true;
      bpButton_->setText(tr("Stop BP"));
      view_->setSecondary(tr("Blood pressure..."));
    }
  });
  bar->addWidget(bpButton_);

  bar->addSeparator();
  bar->addWidget(new QLabel(tr("Graph:"), bar));
  graphWindowBox_ = new QComboBox(bar);
  for (int minutes : {1, 5, 15, 30, 60, 120}) {
    graphWindowBox_->addItem(minutes < 60 ? tr("%1 min").arg(minutes) : tr("%1 h").arg(minutes / 60),
                             minutes * 60);
  }
  graphWindowBox_->setToolTip(tr("Time span shown in the graph"));
  connect(graphWindowBox_, &QComboBox::currentIndexChanged, this, [this] {
    graph_->setWindowSeconds(graphWindowBox_->currentData().toInt());
  });
  bar->addWidget(graphWindowBox_);

  auto* spacer = new QWidget(bar);
  spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
  bar->addWidget(spacer);

  logView_ = new QPlainTextEdit(this);
  logView_->setReadOnly(true);
  logView_->setMaximumBlockCount(2000);
  logDock_ = new QDockWidget(tr("Log"), this);
  logDock_->setObjectName("logDock");
  logDock_->setWidget(logView_);
  addDockWidget(Qt::BottomDockWidgetArea, logDock_);
  logDock_->hide();
  QAction* logAction = logDock_->toggleViewAction();
  logAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_L));
  bar->addAction(logAction);

  auto* fullScreen = new QAction(tr("Full screen"), this);
  fullScreen->setShortcut(QKeySequence::FullScreen);
  connect(fullScreen, &QAction::triggered, this, &MainWindow::toggleFullScreen);
  bar->addAction(fullScreen);
  addAction(fullScreen);

  auto* f11 = new QShortcut(QKeySequence(Qt::Key_F11), this);
  connect(f11, &QShortcut::activated, this, &MainWindow::toggleFullScreen);
  auto* esc = new QShortcut(QKeySequence(Qt::Key_Escape), this);
  connect(esc, &QShortcut::activated, this, [this] {
    if (isFullScreen()) toggleFullScreen();
  });
  auto* m = new QShortcut(QKeySequence(Qt::Key_M), this);
  connect(m, &QShortcut::activated, metronomeBox_, &QCheckBox::toggle);
  auto* quit = new QShortcut(QKeySequence::Quit, this);
  connect(quit, &QShortcut::activated, this, &QWidget::close);

  stateLabel_ = new QLabel(this);
  statusBar()->addWidget(stateLabel_, 1);
}

void MainWindow::refreshControllers() {
  QSettings settings;
  const QString wanted = controllerOverride_.isEmpty()
                             ? settings.value("controller", "auto").toString()
                             : controllerOverride_;

  controllerBox_->clear();
  controllerBox_->addItem(tr("Automatic"), QStringLiteral("auto"));
  const auto controllers = s226::listUsbControllers();
  for (const auto& c : controllers) {
    controllerBox_->addItem(QString::fromStdString(c.displayName()),
                            QString::fromStdString(c.portPath));
    const int idx = controllerBox_->count() - 1;
    controllerBox_->setItemData(
        idx,
        tr("%1\nUSB %2, port %3, %4\nkernel driver: %5")
            .arg(QString::fromStdString(c.displayName()), QString::fromStdString(c.vidPid()),
                 QString::fromStdString(c.portPath), QString::fromStdString(c.devNode()),
                 c.kernelDriver.empty() ? tr("none") : QString::fromStdString(c.kernelDriver)),
        Qt::ToolTipRole);
    if (QString::fromStdString(c.portPath) == wanted ||
        QString::fromStdString(c.vidPid()) == wanted) {
      controllerBox_->setCurrentIndex(idx);
    }
  }
  if (controllers.empty()) {
    appendLog(tr("No USB Bluetooth controller found."));
  }
}

QString MainWindow::selectedController() const {
  if (!controllerOverride_.isEmpty() && controllerBox_->currentIndex() <= 0) {
    return controllerOverride_;
  }
  return controllerBox_->currentData().toString();
}

void MainWindow::connectWatch() {
  if (active_) return;
  active_ = true;
  QSettings().setValue("controller", controllerBox_->currentData().toString());
  appendLog(tr("Starting (controller: %1)").arg(selectedController()));
  bridge_.start(selectedController());
  onStateChanged(s226::WatchState::OpeningController, {});
}

void MainWindow::disconnectWatch() {
  bridge_.stop(); // emits Stopped
  active_ = false;
}

void MainWindow::onStateChanged(s226::WatchState state, const QString& detail) {
  state_ = state;
  const QString text = stateText(state, detail);
  stateLabel_->setText(text);
  if (state == s226::WatchState::Error || state == s226::WatchState::Stopped) {
    active_ = false;
  }
  connectButton_->setText(active_ ? tr("Disconnect") : tr("Connect"));
  controllerBox_->setEnabled(!active_);
  bpButton_->setEnabled(state == s226::WatchState::Connected);
  if (state != s226::WatchState::Connected) {
    stepRate_.reset();
    updateActivity();
    view_->setStale(true);
    metronome_.setBpm(0);
    if (bpRunning_) {
      bpRunning_ = false;
      bpButton_->setText(tr("Blood pressure"));
    }
  }
  view_->setStatus(state == s226::WatchState::Connected ? tr("Measuring...") : text);
  if (state == s226::WatchState::Error) appendLog(text);
}

void MainWindow::onHeartRate(int bpm) {
  if (bpm <= 0) {
    view_->setStatus(tr("Measuring..."));
    return;
  }
  sinceSample_.restart();
  const QDateTime now = QDateTime::currentDateTime();
  graph_->addSample(TrendGraph::Bpm, now.toMSecsSinceEpoch(), bpm);
  log_.write(now, bpm, std::nullopt);
  view_->setBpm(bpm);
  view_->setStale(false);
  view_->setStatus({});
  metronome_.setBpm(bpm);
}

void MainWindow::loadHistory() {
  const auto rows = log_.readSince(
      QDateTime::currentDateTime().addSecs(-TrendGraph::kMaxWindowSeconds));
  for (const auto& r : rows) {
    if (r.bpm) graph_->addSample(TrendGraph::Bpm, r.msecs, *r.bpm);
    if (r.spm) graph_->addSample(TrendGraph::Spm, r.msecs, *r.spm);
  }
  appendLog(tr("Logging readings to %1").arg(log_.directory()));
}

void MainWindow::updateActivity() {
  if (!haveSteps_) {
    view_->setActivity({});
    return;
  }
  QString text = tr("%1 steps").arg(QLocale().toString(steps_));
  if (auto spm = stepRate_.stepsPerMinute()) {
    text += tr("  \u00B7  %1 spm").arg(qRound(*spm));
  }
  view_->setActivity(text);
}

void MainWindow::checkStale() {
  if (sinceSample_.isValid() && sinceSample_.elapsed() > kStaleMs) {
    view_->setStale(true);
    metronome_.setBpm(0);
    if (state_ == s226::WatchState::Connected) view_->setStatus(tr("Waiting for a reading..."));
    sinceSample_.invalidate();
  }
}

void MainWindow::appendLog(const QString& line) {
  logView_->appendPlainText(QDateTime::currentDateTime().toString("HH:mm:ss  ") + line);
}

void MainWindow::toggleFullScreen() {
  if (isFullScreen()) {
    showNormal();
  } else {
    showFullScreen();
  }
}

void MainWindow::closeEvent(QCloseEvent* event) {
  QSettings settings;
  settings.setValue("geometry", saveGeometry());
  settings.setValue("windowState", saveState());
  settings.setValue("metronome", metronomeBox_->isChecked());
  settings.setValue("volume", volumeSlider_->value());
  settings.setValue("graphWindow", graphWindowBox_->currentData().toInt());
  bridge_.stop();
  event->accept();
}
