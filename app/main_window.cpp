#include "main_window.hpp"

#include <QAction>
#include <functional>
#include <memory>
#include <QApplication>
#include <QEvent>
#include <QMenu>
#include <QSystemTrayIcon>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDockWidget>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QShortcut>
#include <QSizePolicy>
#include <QSlider>
#include <QStatusBar>
#include <QTabWidget>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>

#include "alarms_tab.hpp"
#include "bpm_view.hpp"
#include "history_tab.hpp"
#include "notify_tab.hpp"
#include "settings_tab.hpp"
#include "trend_graph.hpp"
#include "workouts_tab.hpp"
#include "sleep_tab.hpp"
#include "mpris_controller.hpp"
#include "notification_forwarder.hpp"
#include "s226/usb.hpp"

namespace {

constexpr int kStaleMs = 6000;
constexpr int kMaxKnownWatches = 8;
constexpr int kWindowStateVersion = 3;

bool isAddress(const QString& s) {
  static const QRegularExpression re(QStringLiteral("^([0-9A-F]{2}:){5}[0-9A-F]{2}$"));
  return re.match(s).hasMatch();
}

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

MainWindow::MainWindow(const QString& controllerOverride, const QString& addressOverride,
                       QWidget* parent)
    : QMainWindow(parent), controllerOverride_(controllerOverride) {
  setWindowTitle(tr("S226 Heart Rate"));
  setMinimumSize(720, 520);
  mpris_ = new MprisController(this);
  desktopNotify_ = new NotificationForwarder(this);
  buildUi();
  buildTray();
  refreshControllers();
  updateTray();

  connect(&bridge_, &WatchBridge::stateChanged, this, &MainWindow::onStateChanged);
  connect(&bridge_, &WatchBridge::heartRate, this, &MainWindow::onHeartRate);
  connect(&bridge_, &WatchBridge::musicControl, mpris_,
          &MprisController::handleWatchAction);
  connect(mpris_, &MprisController::trackChanged, this, [this](const s226::protocol::NowPlaying& np) {
    if (!bridge_.isConnected()) return;
    const auto packets = s226::protocol::nowPlayingPackets(np);
    for (const auto& pkt : packets) bridge_.send(pkt);
  });
  connect(desktopNotify_, &NotificationForwarder::notificationReceived, this,
          [this](const QString& app, const QString& summary, const QString& body) {
            if (!bridge_.isConnected()) return;
            QString text;
            if (!summary.isEmpty() && !body.isEmpty())
              text = summary + QStringLiteral(": ") + body;
            else if (!summary.isEmpty())
              text = summary;
            else
              text = body;
            if (!app.isEmpty() && !text.isEmpty())
              text = app + QStringLiteral("\n") + text;
            else if (!app.isEmpty())
              text = app;
            text = text.simplified();
            if (text.isEmpty()) return;
            if (text.size() > 120) text = text.left(117) + QStringLiteral("…");
            // Rate-limit identical messages.
            static QString last;
            static qint64 lastMs = 0;
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            if (text == last && now - lastMs < 2500) return;
            last = text;
            lastMs = now;
            // H-Band expects ~120 ms between 0xC2 fragments; do not burst.
            auto packets = std::make_shared<std::vector<s226::protocol::Bytes>>(
                s226::protocol::messagePackets(text.toStdString(),
                                               s226::protocol::MessageType::Other));
            auto index = std::make_shared<size_t>(0);
            auto sendNext = std::make_shared<std::function<void()>>();
            *sendNext = [this, packets, index, sendNext]() {
              if (!bridge_.isConnected() || *index >= packets->size()) return;
              bridge_.send((*packets)[(*index)++]);
              if (*index < packets->size())
                QTimer::singleShot(120, this, *sendNext);
            };
            (*sendNext)();
            appendLog(tr("Desktop notification → watch: %1").arg(text));
          });
  connect(desktopNotify_, &NotificationForwarder::listenFailed, this, [this](const QString& reason) {
    appendLog(tr("Desktop notification forward: %1").arg(reason));
  });
  connect(desktopNotify_, &NotificationForwarder::debugLog, this, &MainWindow::appendLog);
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
  connect(&bridge_, &WatchBridge::deviceInfo, this, [this](int number, const QString& fw) {
    appendLog(tr("Watch firmware %1, device number %2").arg(fw).arg(number));
    batteryLabel_->setToolTip(tr("Firmware %1, device number %2").arg(fw).arg(number));
  });
  connect(&bridge_, &WatchBridge::battery, this, [this](int percent, int level) {
    batteryLabel_->setText(percent >= 0 ? tr("Battery %1%").arg(percent)
                                        : tr("Battery %1/4").arg(level));
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
  const QString watch = addressOverride.isEmpty() ? settings.value("watch").toString()
                                                  : addressOverride.trimmed().toUpper();
  if (!watch.isEmpty()) {
    if (watchBox_->findText(watch) < 0) watchBox_->addItem(watch, watch);
    watchBox_->setCurrentIndex(watchBox_->findText(watch));
  }
  const int graphIdx = graphWindowBox_->findData(settings.value("graphWindow", 300).toInt());
  graphWindowBox_->setCurrentIndex(graphIdx >= 0 ? graphIdx : 1);
  graph_->setWindowSeconds(graphWindowBox_->currentData().toInt());
  loadHistory();
  restoreState(settings.value("windowState").toByteArray(), kWindowStateVersion);
  metronomeBox_->setChecked(settings.value("metronome", false).toBool());
  volumeSlider_->setValue(settings.value("volume", 60).toInt());

  onStateChanged(s226::WatchState::Stopped, {});
}

MainWindow::~MainWindow() = default;

void MainWindow::buildUi() {
  tabs_ = new QTabWidget(this);
  setCentralWidget(tabs_);

  // Live tab
  auto* live = new QWidget(tabs_);
  auto* liveLay = new QVBoxLayout(live);
  liveLay->setContentsMargins(0, 0, 0, 0);
  liveLay->setSpacing(0);
  view_ = new BpmView(live);
  graph_ = new TrendGraph(live);
  liveLay->addWidget(view_, 3);
  liveLay->addWidget(graph_, 1);

  auto* liveBar = new QWidget(live);
  auto* liveBarLay = new QHBoxLayout(liveBar);
  liveBarLay->setContentsMargins(8, 4, 8, 4);

  metronomeBox_ = new QCheckBox(tr("&Metronome"), liveBar);
  metronomeBox_->setToolTip(tr("Click along with the heart rate (M)"));
  connect(metronomeBox_, &QCheckBox::toggled, &metronome_, &Metronome::setSoundEnabled);
  liveBarLay->addWidget(metronomeBox_);

  volumeSlider_ = new QSlider(Qt::Horizontal, liveBar);
  volumeSlider_->setRange(0, 100);
  volumeSlider_->setFixedWidth(100);
  volumeSlider_->setToolTip(tr("Metronome volume"));
  connect(volumeSlider_, &QSlider::valueChanged, this,
          [this](int v) { metronome_.setVolume(v / 100.0); });
  liveBarLay->addWidget(volumeSlider_);
  liveBarLay->addSpacing(12);

  bpButton_ = new QPushButton(tr("Blood pressure"), liveBar);
  bpButton_->setToolTip(tr("Start a blood-pressure measurement on the watch"));
  bpButton_->setEnabled(false);
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
  liveBarLay->addWidget(bpButton_);
  liveBarLay->addSpacing(12);

  liveBarLay->addWidget(new QLabel(tr("Graph:"), liveBar));
  graphWindowBox_ = new QComboBox(liveBar);
  for (int minutes : {1, 5, 15, 30, 60, 120})
    graphWindowBox_->addItem(tr("%1 min").arg(minutes), minutes * 60);
  graphWindowBox_->setToolTip(tr("Time span shown in the graph"));
  connect(graphWindowBox_, &QComboBox::currentIndexChanged, this, [this] {
    graph_->setWindowSeconds(graphWindowBox_->currentData().toInt());
  });
  liveBarLay->addWidget(graphWindowBox_);
  liveBarLay->addStretch(1);
  liveLay->addWidget(liveBar);
  tabs_->addTab(live, tr("Live"));

  settingsTab_ = new SettingsTab(bridge_, tabs_);
  tabs_->addTab(settingsTab_, tr("Settings"));
  alarmsTab_ = new AlarmsTab(bridge_, tabs_);
  tabs_->addTab(alarmsTab_, tr("Alarms"));
  notifyTab_ = new NotifyTab(bridge_, mpris_, desktopNotify_, tabs_);
  tabs_->addTab(notifyTab_, tr("Notify"));
  historyTab_ = new HistoryTab(bridge_, tabs_);
  tabs_->addTab(historyTab_, tr("History"));
  sleepTab_ = new SleepTab(bridge_, tabs_);
  tabs_->addTab(sleepTab_, tr("Sleep"));
  workoutsTab_ = new WorkoutsTab(bridge_, tabs_);
  tabs_->addTab(workoutsTab_, tr("Workouts"));

  connect(tabs_, &QTabWidget::currentChanged, this, [this](int index) {
    if (index < 0) return;
    QWidget* w = tabs_->widget(index);
    if (w == settingsTab_) settingsTab_->onShown();
    else if (w == alarmsTab_) alarmsTab_->onShown();
  });

  // Connection toolbar
  auto* bar = addToolBar(tr("Connection"));
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

  bar->addWidget(new QLabel(tr("Watch:"), bar));
  watchBox_ = new QComboBox(bar);
  watchBox_->setEditable(true);
  watchBox_->setInsertPolicy(QComboBox::NoInsert);
  watchBox_->setMinimumContentsLength(17);
  watchBox_->addItem(tr("Any S226"), QString());
  watchBox_->lineEdit()->setPlaceholderText(QStringLiteral("XX:XX:XX:XX:XX:XX"));
  watchBox_->setToolTip(
      tr("Connect to any S226, or only to the watch with this Bluetooth address.\n"
         "Watches connected before are listed; the watch shows the last two bytes."));
  for (const QString& a : QSettings().value("knownWatches").toStringList())
    if (isAddress(a)) watchBox_->addItem(a, a);
  bar->addWidget(watchBox_);

  connectButton_ = new QPushButton(tr("Connect"), bar);
  connect(connectButton_, &QPushButton::clicked, this, [this] {
    if (active_) disconnectWatch();
    else connectWatch();
  });
  bar->addWidget(connectButton_);

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
  connect(quit, &QShortcut::activated, this, &MainWindow::quitApp);

  stateLabel_ = new QLabel(this);
  statusBar()->addWidget(stateLabel_, 1);
  batteryLabel_ = new QLabel(this);
  statusBar()->addPermanentWidget(batteryLabel_);
}

void MainWindow::setTabsEnabled(bool connected) {
  settingsTab_->setConnected(connected);
  alarmsTab_->setConnected(connected);
  notifyTab_->setConnected(connected);
  historyTab_->setConnected(connected);
  workoutsTab_->setConnected(connected);
  sleepTab_->setConnected(connected);
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
        QString::fromStdString(c.vidPid()) == wanted)
      controllerBox_->setCurrentIndex(idx);
  }
  if (controllers.empty()) appendLog(tr("No USB Bluetooth controller found."));
}

QString MainWindow::selectedWatch() const {
  const QString text = watchBox_->currentText().trimmed();
  if (text.isEmpty() || text == watchBox_->itemText(0)) return {};
  return text.toUpper();
}

void MainWindow::rememberWatch(const QString& address) {
  if (!isAddress(address)) return;
  QSettings settings;
  QStringList known = settings.value("knownWatches").toStringList();
  known.removeAll(address);
  known.prepend(address);
  while (known.size() > kMaxKnownWatches) known.removeLast();
  settings.setValue("knownWatches", known);
  if (watchBox_->findText(address) < 0) watchBox_->addItem(address, address);
}

QString MainWindow::selectedController() const {
  if (!controllerOverride_.isEmpty() && controllerBox_->currentIndex() <= 0)
    return controllerOverride_;
  return controllerBox_->currentData().toString();
}

void MainWindow::connectWatch() {
  if (active_) return;
  const QString watch = selectedWatch();
  if (!watch.isEmpty() && !isAddress(watch)) {
    const QString msg = tr("Invalid watch address \"%1\", expected XX:XX:XX:XX:XX:XX").arg(watch);
    stateLabel_->setText(msg);
    view_->setStatus(msg);
    return;
  }
  active_ = true;
  QSettings().setValue("watch", watch);
  QSettings().setValue("controller", selectedController());
  connectButton_->setText(tr("Disconnect"));
  controllerBox_->setEnabled(false);
  watchBox_->setEnabled(false);
  bridge_.start(selectedController(), watch);
}

void MainWindow::disconnectWatch() {
  if (!active_) return;
  active_ = false;
  bridge_.stop();
  connectButton_->setText(tr("Connect"));
  controllerBox_->setEnabled(true);
  watchBox_->setEnabled(true);
  setTabsEnabled(false);
}

void MainWindow::onStateChanged(s226::WatchState state, const QString& detail) {
  state_ = state;
  const QString text = stateText(state, detail);
  stateLabel_->setText(text);
  const bool connected = (state == s226::WatchState::Connected);
  setTabsEnabled(connected);
  bpButton_->setEnabled(connected);
  if (!connected) lastBpm_ = 0;
  updateTray();
  if (connected && !detail.isEmpty()) rememberWatch(detail);
  if (connected) {
    // Auto-load the visible feature tab once connected.
    if (tabs_->currentWidget() == settingsTab_) settingsTab_->onShown();
    else if (tabs_->currentWidget() == alarmsTab_) alarmsTab_->onShown();
  }
  if (state != s226::WatchState::Connected) {
    batteryLabel_->clear();
    stepRate_.reset();
    updateActivity();
    view_->setStale(true);
    metronome_.setBpm(0);
    if (bpRunning_) {
      bpRunning_ = false;
      bpButton_->setText(tr("Blood pressure"));
      view_->setSecondary({});
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
  lastBpm_ = bpm;
  sinceSample_.restart();
  const QDateTime now = QDateTime::currentDateTime();
  graph_->addSample(TrendGraph::Bpm, now.toMSecsSinceEpoch(), bpm);
  log_.write(now, bpm, std::nullopt);
  view_->setBpm(bpm);
  view_->setStale(false);
  view_->setStatus({});
  metronome_.setBpm(bpm);
  updateTray();
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
  if (auto spm = stepRate_.stepsPerMinute())
    text += tr("  \u00B7  %1 spm").arg(qRound(*spm));
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
  if (isFullScreen()) showNormal();
  else showFullScreen();
}

void MainWindow::closeEvent(QCloseEvent* event) {
  if (!quitting_ && tray_ && tray_->isVisible()) {
    // Keep running in the tray; save geometry but leave the session up.
    QSettings settings;
    settings.setValue("geometry", saveGeometry());
    settings.setValue("windowState", saveState(kWindowStateVersion));
    settings.setValue("metronome", metronomeBox_->isChecked());
    settings.setValue("volume", volumeSlider_->value());
    settings.setValue("graphWindow", graphWindowBox_->currentData().toInt());
    hide();
    event->ignore();
    return;
  }
  QSettings settings;
  settings.setValue("geometry", saveGeometry());
  settings.setValue("windowState", saveState(kWindowStateVersion));
  settings.setValue("metronome", metronomeBox_->isChecked());
  settings.setValue("volume", volumeSlider_->value());
  settings.setValue("graphWindow", graphWindowBox_->currentData().toInt());
  bridge_.stop();
  if (tray_) tray_->hide();
  event->accept();
}

void MainWindow::changeEvent(QEvent* event) {
  QMainWindow::changeEvent(event);
  if (event->type() == QEvent::WindowStateChange && tray_ && tray_->isVisible()) {
    // Optional: could hide on minimize; keep window in taskbar for now.
  }
}

void MainWindow::buildTray() {
  if (!QSystemTrayIcon::isSystemTrayAvailable()) {
    appendLog(tr("No system tray available; the window will quit on close."));
    return;
  }

  trayMenu_ = new QMenu(this);
  trayShowAction_ = trayMenu_->addAction(tr("Show window"));
  connect(trayShowAction_, &QAction::triggered, this, &MainWindow::showFromTray);
  trayConnectAction_ = trayMenu_->addAction(tr("Connect"));
  connect(trayConnectAction_, &QAction::triggered, this, [this] {
    if (active_) disconnectWatch();
    else connectWatch();
  });
  trayMenu_->addSeparator();
  trayQuitAction_ = trayMenu_->addAction(tr("Quit"));
  connect(trayQuitAction_, &QAction::triggered, this, &MainWindow::quitApp);

  tray_ = new QSystemTrayIcon(this);
  tray_->setIcon(windowIcon().isNull()
                     ? QIcon::fromTheme(QStringLiteral("s226-hr"), QIcon(QStringLiteral(":/s226-hr.svg")))
                     : windowIcon());
  tray_->setContextMenu(trayMenu_);
  tray_->setToolTip(tr("S226 Heart Rate"));
  connect(tray_, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
    if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick)
      showFromTray();
  });
  tray_->show();
}

void MainWindow::updateTray() {
  if (!tray_) return;
  if (trayConnectAction_) {
    trayConnectAction_->setText(active_ ? tr("Disconnect") : tr("Connect"));
  }
  QString tip = tr("S226 Heart Rate");
  if (state_ == s226::WatchState::Connected) {
    if (lastBpm_ > 0)
      tip = tr("S226 — %1 bpm").arg(lastBpm_);
    else
      tip = tr("S226 — connected");
  } else if (active_) {
    tip = tr("S226 — %1").arg(stateText(state_, {}));
  } else {
    tip = tr("S226 — disconnected");
  }
  tray_->setToolTip(tip);
}

void MainWindow::bringToFront() { showFromTray(); }

void MainWindow::showFromTray() {
  showNormal();
  raise();
  activateWindow();
}

void MainWindow::quitApp() {
  quitting_ = true;
  bridge_.stop();
  if (tray_) tray_->hide();
  QApplication::quit();
}
