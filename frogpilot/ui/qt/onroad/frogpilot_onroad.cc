#include <QDateTime>

#include "frogpilot/ui/qt/onroad/frogpilot_onroad.h"

FrogPilotOnroadWindow::FrogPilotOnroadWindow(QWidget *parent) : QWidget(parent) {
  signalTimer = new QTimer(this);

  QObject::connect(signalTimer, &QTimer::timeout, [this] {
    flickerActive = !flickerActive;
  });

  // CAN Playback overlay - only enabled when CAN_PLAYBACK env var is set
  isCanPlayback = getenv("CAN_PLAYBACK") != nullptr;

  if (isCanPlayback) {
    playback_overlay_ = new PlaybackOverlay(this);
    playback_overlay_->setDuration(playback_duration_);
    playback_overlay_->setPlaying(true);
    playback_overlay_->showOverlay();

    // Timer to simulate playback position updates (mock for UI testing)
    playback_timer_ = new QTimer(this);
    QObject::connect(playback_timer_, &QTimer::timeout, [this]() {
      updatePlaybackPosition();
    });
    playback_timer_->start(100);  // Update every 100ms

    // Connect overlay signals
    QObject::connect(playback_overlay_, &PlaybackOverlay::seekRequested, [this](double position) {
      playback_position_ = position;
      playback_overlay_->setPosition(position);
      updatePlaybackRealTime();
    });

    QObject::connect(playback_overlay_, &PlaybackOverlay::playPauseRequested, [this]() {
      // Toggle play/pause (mock)
      static bool playing = true;
      playing = !playing;
      playback_overlay_->setPlaying(playing);
    });

    QObject::connect(playback_overlay_, &PlaybackOverlay::speedChangeRequested, [this](double speed) {
      // Speed change handled by overlay internally, this signal is for future backend integration
    });
  }
}

void FrogPilotOnroadWindow::updateState(const UIState &s, const FrogPilotUIState &fs) {
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
}

void FrogPilotOnroadWindow::updatePlaybackPosition() {
  if (!isCanPlayback || !playback_overlay_) return;

  // Mock: advance position by 100ms
  playback_position_ += 0.1;
  if (playback_position_ >= playback_duration_) {
    playback_position_ = 0.0;  // Loop
  }

  playback_overlay_->setPosition(playback_position_);
  updatePlaybackRealTime();
}

void FrogPilotOnroadWindow::updatePlaybackRealTime() {
  if (!isCanPlayback || !playback_overlay_) return;

  // Calculate real time from start time + current position
  // For mock: parse the start time and add position seconds
  QDateTime start_dt = QDateTime::fromString(playback_start_time_, "yyyy-MM-dd HH:mm:ss.zzz");
  if (start_dt.isValid()) {
    qint64 msecs = static_cast<qint64>(playback_position_ * 1000.0);
    QDateTime real_dt = start_dt.addMSecs(msecs);
    playback_overlay_->setRealTime(real_dt.toString("yyyy-MM-dd HH:mm:ss.zzz"));
  }
}

void FrogPilotOnroadWindow::paintEvent(QPaintEvent *event) {
  QPainter p(this);
  p.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);

  QRect rect = this->rect();

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
