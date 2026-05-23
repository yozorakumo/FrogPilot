#include <QDateTime>

#include "common/params.h"
#include "frogpilot/ui/qt/onroad/frogpilot_onroad.h"

FrogPilotOnroadWindow::FrogPilotOnroadWindow(QWidget *parent) : QWidget(parent) {
  signalTimer = new QTimer(this);

  QObject::connect(signalTimer, &QTimer::timeout, [this] {
    flickerActive = !flickerActive;
  });

  // CAN Playback overlay will be initialized lazily in updateState()
  // when CAN_PLAYBACK is detected in Params (set by UI from can_log_settings)
  isCanPlayback = false;
  playback_overlay_ = nullptr;
  stop_playback_btn_ = nullptr;
  playback_timer_ = nullptr;
}

void FrogPilotOnroadWindow::initPlaybackOverlay() {
  if (playback_overlay_ != nullptr) return;

  // Read initial playback state from Params (written by can_player.py)
  Params params;
  QString duration_str = QString::fromStdString(params.get("CanPlaybackDuration"));
  QString realtime_str = QString::fromStdString(params.get("CanPlaybackRealTime"));
  QString playing_str = QString::fromStdString(params.get("CanPlaybackPlaying"));

  if (!duration_str.isEmpty()) {
    playback_duration_ = duration_str.toDouble();
  }
  if (!realtime_str.isEmpty()) {
    // Parse real time to get start time: real_time = start_time + position
    // Store the raw real time string for display
    playback_start_time_ = realtime_str;
  }

  playback_overlay_ = new PlaybackOverlay(this);
  playback_overlay_->setDuration(playback_duration_);
  playback_overlay_->setPlaying(playing_str != "0");
  playback_overlay_->showOverlay();

  // Timer to update playback position from Params
  playback_timer_ = new QTimer(this);
  QObject::connect(playback_timer_, &QTimer::timeout, [this]() {
    updatePlaybackPosition();
  });
  playback_timer_->start(100);  // Update every 100ms

  // Connect overlay signals
  QObject::connect(playback_overlay_, &PlaybackOverlay::seekRequested, [this](double position) {
    // Write seek command to Params for can_player.py to read
    Params params;
    params.put("CanPlaybackSeek", std::to_string(position));
    playback_position_ = position;
    playback_overlay_->setPosition(position);
    updatePlaybackRealTime();
  });

  QObject::connect(playback_overlay_, &PlaybackOverlay::playPauseRequested, [this]() {
    Params params;
    bool currently_playing = playback_overlay_->isPlaying();
    params.put("CanPlaybackPause", currently_playing ? "1" : "0");
    playback_overlay_->setPlaying(!currently_playing);
  });

  QObject::connect(playback_overlay_, &PlaybackOverlay::speedChangeRequested, [](double speed) {
    // Write speed change command to Params for can_player.py to read
    Params params;
    params.put("CanPlaybackSpeedCmd", std::to_string(speed));
  });

  // Stop playback button (top-left corner)
  stop_playback_btn_ = new QPushButton("⏹ 停止", this);
  stop_playback_btn_->setFixedSize(120, 60);
  stop_playback_btn_->setStyleSheet(R"(
    QPushButton {
      background: rgba(180, 40, 40, 180);
      color: white;
      border: none;
      border-radius: 10px;
      font-size: 20px;
      font-weight: bold;
    }
    QPushButton:pressed {
      background: rgba(220, 60, 60, 220);
    }
  )");
  stop_playback_btn_->setAttribute(Qt::WA_TransparentForMouseEvents, false);
  QObject::connect(stop_playback_btn_, &QPushButton::clicked, [this]() {
    stopPlayback();
  });
  stop_playback_btn_->raise();
  stop_playback_btn_->show();
}

void FrogPilotOnroadWindow::updateState(const UIState &s, const FrogPilotUIState &fs) {
  // Check CAN_PLAYBACK from Params (lazy initialization)
  if (!isCanPlayback) {
    Params params;
    isCanPlayback = params.getBool("CAN_PLAYBACK");
    if (isCanPlayback) {
      initPlaybackOverlay();
    }
  }

  QJsonObject &frogpilot_toggles = fs.frogpilot_toggles;
  SubMaster &fpsm = *(fs.sm);

  const cereal::CarState::Reader &carState = fpsm["carState"].getCarState();
  const cereal::CarControl::Reader &carControl = fpsm["carControl"].getCarControl();

  blindSpotLeft = carState.getLeftBlindspot();
  blindSpotRight = carState.getRightBlindspot();
  steer = -carControl.getActuators().getSteer();
  turnSignalLeft = carState.getLeftBlinker();
  turnSignalRight = carState.getRightBlinker();

  showBlindspot = (blindSpotLeft || blindSpotRight) && frogpilot_toggles.value("blind_spot_metrics").toBool();
  showFPS = frogpilot_toggles.value("show_fps").toBool();
  showSignal = (turnSignalLeft || turnSignalRight) && frogpilot_toggles.value("signal_metrics").toBool();
  showSteering = frogpilot_toggles.value("steering_metrics").toBool();

  update();
}

void FrogPilotOnroadWindow::resizeEvent(QResizeEvent *event) {
  QWidget::resizeEvent(event);

  if (isCanPlayback && playback_overlay_) {
    // Position overlay at the bottom of the widget
    int overlay_x = 0;
    int overlay_y = height() - PlaybackOverlay::kOverlayHeight;
    playback_overlay_->setGeometry(overlay_x, overlay_y, width(), PlaybackOverlay::kOverlayHeight);
    playback_overlay_->raise();
  }

  if (isCanPlayback && stop_playback_btn_) {
    // Position stop button at top-left corner (below sidebar area)
    stop_playback_btn_->setGeometry(20, 80, 120, 60);
    stop_playback_btn_->raise();
  }
}

void FrogPilotOnroadWindow::updatePlaybackPosition() {
  if (!isCanPlayback || !playback_overlay_) return;

  // Read playback state from Params (written by can_player.py)
  Params params;
  QString position_str = QString::fromStdString(params.get("CanPlaybackPosition"));
  QString duration_str = QString::fromStdString(params.get("CanPlaybackDuration"));
  QString playing_str = QString::fromStdString(params.get("CanPlaybackPlaying"));
  QString realtime_str = QString::fromStdString(params.get("CanPlaybackRealTime"));
  QString speed_str = QString::fromStdString(params.get("CanPlaybackSpeed"));

  // Read loading progress (0-100%) - rlog loading from can_player.py
  QString loading_str = QString::fromStdString(params.get("CanPlaybackLoadingProgress"));
  if (!loading_str.isEmpty()) {
    loading_progress_ = loading_str.toInt();
  }

  if (!position_str.isEmpty()) {
    playback_position_ = position_str.toDouble();
    playback_overlay_->setPosition(playback_position_);
  }

  if (!duration_str.isEmpty()) {
    double duration = duration_str.toDouble();
    if (duration != playback_duration_) {
      playback_duration_ = duration;
      playback_overlay_->setDuration(playback_duration_);
    }
  }

  if (!playing_str.isEmpty()) {
    playback_overlay_->setPlaying(playing_str != "0");
  }

  if (!realtime_str.isEmpty()) {
    playback_overlay_->setRealTime(realtime_str);
  }

  if (!speed_str.isEmpty()) {
    playback_overlay_->setPlaybackSpeed(speed_str.toDouble());
  }

  // Check if playback has ended (CAN_PLAYBACK removed)
  if (!params.getBool("CAN_PLAYBACK")) {
    isCanPlayback = false;
    if (playback_overlay_) {
      playback_overlay_->hideOverlay();
    }
    if (stop_playback_btn_) {
      stop_playback_btn_->hide();
    }
    if (playback_timer_) {
      playback_timer_->stop();
    }
  }
}

void FrogPilotOnroadWindow::updatePlaybackRealTime() {
  if (!isCanPlayback || !playback_overlay_) return;

  // CanPlaybackRealTimeには録画日時（YYYY-MM-DD HH:MM）が設定される
  // （can_player.pyがセグメントディレクトリのmtimeから取得）
  // そのまま表示する（updatePlaybackPositionで既にrealtime_strを読み取っている）
}

void FrogPilotOnroadWindow::stopPlayback() {
  Params params;
  params.put("CanPlaybackPlaying", "0");
  params.remove("CAN_PLAYBACK");

  isCanPlayback = false;

  if (playback_overlay_) {
    playback_overlay_->hideOverlay();
  }
  if (stop_playback_btn_) {
    stop_playback_btn_->hide();
  }
  if (playback_timer_) {
    playback_timer_->stop();
  }
}

void FrogPilotOnroadWindow::paintEvent(QPaintEvent *event) {
  QPainter p(this);
  p.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);

  QRect rect = this->rect();

  // CAN Playback loading overlay
  // Show loading progress (rlog) only
  if (isCanPlayback && loading_progress_ < 100) {
    p.fillRect(rect, QColor(0, 0, 0, 180));

    QString loadingText;
    loadingText = QString("Loading %1%").arg(loading_progress_);

    p.setFont(InterFont(72, QFont::Bold));
    p.setPen(Qt::white);

    QRect textRect = p.fontMetrics().boundingRect(loadingText);
    int xPos = (rect.width() - textRect.width()) / 2;
    int yPos = (rect.height() - textRect.height()) / 2 + textRect.height() / 2;
    p.drawText(xPos, yPos, loadingText);
    return;
  }

  QRegion marginRegion;
  marginRegion += QRegion(0, 0, rect.width(), UI_BORDER_SIZE);
  marginRegion += QRegion(0, rect.height() - UI_BORDER_SIZE, rect.width(), UI_BORDER_SIZE);
  marginRegion += QRegion(0, UI_BORDER_SIZE, UI_BORDER_SIZE, rect.height() - 2 * UI_BORDER_SIZE);
  marginRegion += QRegion(rect.width() - UI_BORDER_SIZE, UI_BORDER_SIZE, UI_BORDER_SIZE, rect.height() - 2 * UI_BORDER_SIZE);
  p.setClipRegion(marginRegion);

  if (showSteering) {
    paintSteeringTorqueBorder(p, rect);
  }

  if (showBlindspot || showSignal) {
    int interval = showBlindspot ? 250 : 500;

    if (!signalTimer->isActive()) {
      signalTimer->start(interval);
    } else if (signalTimer->interval() != interval) {
      signalTimer->stop();
      signalTimer->start(interval);
    }

    paintTurnSignalBorder(p, rect);
  } else if (signalTimer->isActive()) {
    signalTimer->stop();
  }

  if (showFPS) {
    paintFPS(p, rect);
  }
}

void FrogPilotOnroadWindow::paintFPS(QPainter &p, const QRect &rect) {
  p.save();

  qint64 now = QDateTime::currentMSecsSinceEpoch();

  static double maxFPS = 0.0;
  static double minFPS = 99.9;
  static double totalFPS = 0.0;

  static QList<QPair<qint64, double>> fpsHistory;

  fpsHistory.append({now, fps});
  totalFPS += fps;

  while (!fpsHistory.isEmpty() && now - fpsHistory.first().first > 60000) {
    totalFPS -= fpsHistory.first().second;
    fpsHistory.removeFirst();
  }

  double avgFPS = fpsHistory.isEmpty() ? 0.0 : totalFPS / fpsHistory.size();

  minFPS = std::min(minFPS, fps);
  maxFPS = std::max(maxFPS, fps);

  QString fpsDisplayString = QString(tr("FPS: %1 | Min: %2 | Max: %3 | Avg: %4"))
                                .arg(qRound(fps))
                                .arg(qRound(minFPS))
                                .arg(qRound(maxFPS))
                                .arg(qRound(avgFPS));

  p.setFont(InterFont(28, QFont::DemiBold));
  p.setPen(Qt::white);

  int xPos = (rect.width() - p.fontMetrics().horizontalAdvance(fpsDisplayString)) / 2;
  int yPos = rect.bottom() - 5;

  p.drawText(xPos, yPos, fpsDisplayString);

  p.restore();
}

void FrogPilotOnroadWindow::paintSteeringTorqueBorder(QPainter &p, const QRect &rect) {
  p.save();

  static float smoothedSteer = 0.0;
  smoothedSteer = 0.25 * std::abs(steer) + 0.75 * smoothedSteer;
  if (std::abs(smoothedSteer - steer) < 0.01) {
    smoothedSteer = steer;
  }

  QLinearGradient gradient(rect.topLeft(), rect.bottomLeft());
  gradient.setColorAt(0.0, bg_colors[STATUS_TRAFFIC_MODE_ENABLED]);
  gradient.setColorAt(0.25, bg_colors[STATUS_EXPERIMENTAL_MODE_ENABLED]);
  gradient.setColorAt(0.5, bg_colors[STATUS_CONDITIONAL_OVERRIDDEN]);
  gradient.setColorAt(0.75, bg_colors[STATUS_ENGAGED]);
  gradient.setColorAt(1.0, bg_colors[STATUS_ENGAGED]);

  int visibleHeight = rect.height() * smoothedSteer;

  QRect rectToFill, rectToHide;
  if (steer < 0) {
    rectToFill = QRect(rect.x(), rect.y() + rect.height() - visibleHeight, UI_BORDER_SIZE, visibleHeight);
    rectToHide = QRect(rect.x(), rect.y(), UI_BORDER_SIZE, rect.height() - visibleHeight);
  } else {
    rectToFill = QRect(rect.x() + rect.width() - UI_BORDER_SIZE, rect.y() + rect.height() - visibleHeight, UI_BORDER_SIZE, visibleHeight);
    rectToHide = QRect(rect.x() + rect.width() - UI_BORDER_SIZE, rect.y(), UI_BORDER_SIZE, rect.height() - visibleHeight);
  }
  p.fillRect(rectToFill, QBrush(gradient));
  p.fillRect(rectToHide, Qt::transparent);

  p.restore();
}

void FrogPilotOnroadWindow::paintTurnSignalBorder(QPainter &p, const QRect &rect) {
  p.save();

  std::function<QColor(bool, bool)> getBorderColor = [&](bool blindSpot, bool turnSignal) {
    if (turnSignal && showSignal) {
      if (blindSpot) {
        return flickerActive ? bg_colors[STATUS_TRAFFIC_MODE_ENABLED] : bg_colors[STATUS_CONDITIONAL_OVERRIDDEN];
      } else {
        return flickerActive ? bg_colors[STATUS_CONDITIONAL_OVERRIDDEN] : bg;
      }
    } else if (blindSpot && showBlindspot) {
      return bg_colors[STATUS_TRAFFIC_MODE_ENABLED];
    } else {
      return bg;
    }
  };

  QColor borderColorLeft = getBorderColor(blindSpotLeft, turnSignalLeft);
  QColor borderColorRight = getBorderColor(blindSpotRight, turnSignalRight);

  p.fillRect(rect.x(), rect.y(), rect.width() / 2, rect.height(), borderColorLeft);
  p.fillRect(rect.x() + rect.width() / 2, rect.y(), rect.width() / 2, rect.height(), borderColorRight);

  p.restore();
}
