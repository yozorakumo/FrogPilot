#include <QDateTime>
#include <chrono>
#include <string>

#include "common/params.h"
#include "frogpilot/ui/qt/onroad/frogpilot_onroad.h"

void FrogPilotOnroadWindow::mousePressEvent(QMouseEvent *e) {
  // イベントがここに到達した時点で、子ウィジェット（PlaybackOverlay、
  // stop_playback_btn_等）にはヒットしていない（Qtは最も深い子ウィジェットに
  // 直接配送するため）。親（OnroadWindow）に伝播して、サイドバートグルを可能にする。
  e->ignore();
}

void FrogPilotOnroadWindow::mouseReleaseEvent(QMouseEvent *e) {
  e->ignore();
}

void FrogPilotOnroadWindow::mouseMoveEvent(QMouseEvent *e) {
  e->ignore();
}

FrogPilotOnroadWindow::FrogPilotOnroadWindow(QWidget *parent) : QWidget(parent) {
  // WA_TransparentForMouseEvents は設定しない
  // 子ウィジェット（PlaybackOverlay, stop_playback_btn_, 描画領域）が
  // 正常にマウスイベントを受け取れるようにする
  // イベント伝播は mousePressEvent/mouseReleaseEvent/mouseMoveEvent で制御

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

  // UIEditMode切替ボタン（画面右上の歯車アイコン）
  // FrogPilotOnroadWindowは最前面ウィジェットなので、ここに配置することで
  // タッチイベントが正しく届く
  edit_mode_btn_ = new QPushButton(QString::fromUtf8("⚙"), this);
  edit_mode_btn_->setFixedSize(60, 60);
  edit_mode_btn_->setStyleSheet(R"(
    QPushButton {
      background: rgba(0, 0, 0, 128);
      color: white;
      border: none;
      border-radius: 10px;
      font-size: 24px;
    }
    QPushButton:pressed {
      background: rgba(255, 165, 0, 200);
    }
  )");
  connect(edit_mode_btn_, &QPushButton::clicked, this, [this]() {
    if (edit_manager_) {
      edit_manager_->toggleEditMode();
      updateEditModeButtonStyle();
    }
  });
  edit_mode_btn_->raise();
  edit_mode_btn_->show();
}

void FrogPilotOnroadWindow::setEditModeManager(UIEditModeManager *manager) {
  edit_manager_ = manager;
  if (edit_manager_) {
    connect(edit_manager_, &UIEditModeManager::settingsChanged, this, [this]() {
      updateEditModeButtonStyle();
    });
  }
}

FrogPilotOnroadWindow::~FrogPilotOnroadWindow() {
  stopParamsThread();
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
    playback_start_time_ = realtime_str;
  }

  // PlaybackOverlay and stop_playback_btn_ receive mouse events normally via Qt's event dispatch.
  // Mouse events on empty areas are propagated to parent via e->ignore() in mousePressEvent.

  playback_overlay_ = new PlaybackOverlay(this);
  playback_overlay_->setDuration(playback_duration_);
  playback_overlay_->setPlaying(!playing_str.isEmpty() && playing_str != "0");
  playback_overlay_->showOverlay();

  // Start background Params reader thread
  params_thread_running_ = true;
  params_thread_ = std::thread(&FrogPilotOnroadWindow::readPlaybackParams, this);

  // Timer to apply cached state to UI (runs on UI thread, no file I/O)
  playback_timer_ = new QTimer(this);
  QObject::connect(playback_timer_, &QTimer::timeout, [this]() {
    applyPlaybackState();
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
  // stop_playback_btn_ は親 FrogPilotOnroadWindow からマウスイベントを受け取る
  // （親は WA_TransparentForMouseEvents を設定不再）
  QObject::connect(stop_playback_btn_, &QPushButton::clicked, [this]() {
    stopPlayback();
  });
  stop_playback_btn_->raise();
  stop_playback_btn_->show();

  // 問題1修正: setGeometryは親widgetのgeometry確定後に実行する必要がある
  // QTimer::singleShot(0, ...)でイベントキュー経由で次回イベントループで実行
  QTimer::singleShot(0, this, [this]() {
    if (stop_playback_btn_) {
      stop_playback_btn_->setGeometry(20, 80, 120, 60);
      stop_playback_btn_->raise();
    }
  });
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

  // 設定ボタンの配置（右上）
  if (edit_mode_btn_) {
    edit_mode_btn_->move(width() - 70, 10);
    edit_mode_btn_->raise();
  }

  if (isCanPlayback && playback_overlay_) {
    // Position overlay at the bottom of the widget
    int overlay_x = 0;
    int overlay_y = height() - PlaybackOverlay::kOverlayHeight;
    playback_overlay_->setGeometry(overlay_x, overlay_y, width(), PlaybackOverlay::kOverlayHeight);
    playback_overlay_->updateGeometry();
    playback_overlay_->update();
    playback_overlay_->raise();
  }

  if (isCanPlayback && stop_playback_btn_) {
    // Position stop button at top-left corner (below sidebar area)
    stop_playback_btn_->setGeometry(20, 80, 120, 60);
    stop_playback_btn_->raise();
  }
}

void FrogPilotOnroadWindow::updateEditModeButtonStyle() {
  if (!edit_mode_btn_) return;
  bool is_edit = edit_manager_ && edit_manager_->isEditMode();
  if (is_edit) {
    edit_mode_btn_->setStyleSheet(R"(
      QPushButton {
        background: rgba(255, 165, 0, 200);
        color: white;
        border: 2px solid rgba(255, 255, 255, 180);
        border-radius: 10px;
        font-size: 24px;
      }
      QPushButton:pressed {
        background: rgba(255, 200, 100, 220);
      }
    )");
  } else {
    edit_mode_btn_->setStyleSheet(R"(
      QPushButton {
        background: rgba(0, 0, 0, 128);
        color: white;
        border: none;
        border-radius: 10px;
        font-size: 24px;
      }
      QPushButton:pressed {
        background: rgba(255, 165, 0, 200);
      }
    )");
  }
}

void FrogPilotOnroadWindow::readPlaybackParams() {
  // Use a separate Params instance for the background thread
  Params params;

  while (params_thread_running_) {
    PlaybackState new_state;

    // Read individual Params keys
    std::string pos_str = params.get("CanPlaybackPosition");
    if (!pos_str.empty()) {
      try { new_state.position = std::stod(pos_str); } catch (...) {}
    }
    std::string dur_str = params.get("CanPlaybackDuration");
    if (!dur_str.empty()) {
      try { new_state.duration = std::stod(dur_str); } catch (...) {}
    }
    std::string spd_str = params.get("CanPlaybackSpeed");
    if (!spd_str.empty()) {
      try { new_state.speed = std::stod(spd_str); } catch (...) {}
    }
    std::string playing_str = params.get("CanPlaybackPlaying");
    new_state.playing = (playing_str == "1");
    new_state.real_time = QString::fromStdString(params.get("CanPlaybackRealTime"));
    std::string lp_str = params.get("CanPlaybackLoadingProgress");
    if (!lp_str.empty()) {
      try { new_state.loading_progress = std::stoi(lp_str); } catch (...) {}
    }

    // Check if playback is still active
    new_state.can_playback = params.getBool("CAN_PLAYBACK");

    // Store to cached state (protected by mutex)
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      cached_state_ = new_state;
    }

    // Sleep for 100ms
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

void FrogPilotOnroadWindow::applyPlaybackState() {
  if (!isCanPlayback || !playback_overlay_) return;

  // Read cached state from background thread
  PlaybackState state;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    state = cached_state_;
  }

  // Check if playback has ended
  if (!state.can_playback) {
    isCanPlayback = false;

    if (playback_timer_) {
      playback_timer_->stop();
      delete playback_timer_;
      playback_timer_ = nullptr;
    }
    if (playback_overlay_) {
      playback_overlay_->hideOverlay();
      delete playback_overlay_;
      playback_overlay_ = nullptr;
    }
    if (stop_playback_btn_) {
      stop_playback_btn_->hide();
      delete stop_playback_btn_;
      stop_playback_btn_ = nullptr;
    }

    // Reset state so initPlaybackOverlay() can recreate everything on next playback
    last_applied_state_ = PlaybackState();
    cached_state_ = PlaybackState();
    playback_position_ = 0.0;
    playback_duration_ = 0.0;
    loading_progress_ = 0;
    return;
  }

  // Diff detection: only update UI elements when values change
  // 一時停止中は位置更新をスキップ（UI上で再生時間が停止するように）
  if (state.playing && state.position != last_applied_state_.position) {
    playback_position_ = state.position;
    playback_overlay_->setPosition(state.position);
  }

  if (state.duration != last_applied_state_.duration) {
    playback_duration_ = state.duration;
    playback_overlay_->setDuration(state.duration);
  }

  if (state.playing != last_applied_state_.playing) {
    playback_overlay_->setPlaying(state.playing);
  }

  if (state.real_time != last_applied_state_.real_time) {
    playback_overlay_->setRealTime(state.real_time);
  }

  if (state.speed != last_applied_state_.speed) {
    playback_overlay_->setPlaybackSpeed(state.speed);
  }

  // Loading progress is used in paintEvent
  loading_progress_ = state.loading_progress;

  last_applied_state_ = state;
}

void FrogPilotOnroadWindow::updatePlaybackRealTime() {
  if (!isCanPlayback || !playback_overlay_) return;

  // CanPlaybackRealTimeには録画日時（YYYY-MM-DD HH:MM）が設定される
  // （can_player.pyがセグメントディレクトリのmtimeから取得）
  // そのまま表示する（readPlaybackParamsで既にrealtimeを読み取っている）
}

void FrogPilotOnroadWindow::stopPlayback() {
  Params params;
  params.put("CanPlaybackPlaying", "0");
  params.remove("CAN_PLAYBACK");

  isCanPlayback = false;

  stopParamsThread();

  if (playback_timer_) {
    playback_timer_->stop();
    delete playback_timer_;
    playback_timer_ = nullptr;
  }

  if (playback_overlay_) {
    playback_overlay_->hideOverlay();
    delete playback_overlay_;
    playback_overlay_ = nullptr;
  }

  if (stop_playback_btn_) {
    stop_playback_btn_->hide();
    delete stop_playback_btn_;
    stop_playback_btn_ = nullptr;
  }

  // Reset state so initPlaybackOverlay() can recreate everything on next playback
  last_applied_state_ = PlaybackState();
  cached_state_ = PlaybackState();
  playback_position_ = 0.0;
  playback_duration_ = 0.0;
  loading_progress_ = 0;
}

void FrogPilotOnroadWindow::stopParamsThread() {
  params_thread_running_ = false;
  if (params_thread_.joinable()) {
    params_thread_.join();
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
