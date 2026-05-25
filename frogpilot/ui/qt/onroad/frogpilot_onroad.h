#pragma once

#include <QPushButton>
#include <QMouseEvent>
#include <atomic>
#include <mutex>
#include <thread>

#include "selfdrive/ui/qt/onroad/annotated_camera.h"

#include "frogpilot/ui/qt/onroad/playback_overlay.h"

// Cached playback state shared between background Params reader and UI thread
struct PlaybackState {
  double position = 0.0;
  double duration = 0.0;
  double speed = 1.0;
  bool playing = false;
  bool can_playback = true;
  int loading_progress = 0;
  QString real_time;
};

class FrogPilotOnroadWindow : public QWidget {
  Q_OBJECT

public:
  FrogPilotOnroadWindow(QWidget* parent = 0);
  ~FrogPilotOnroadWindow();

  void updateState(const UIState &s, const FrogPilotUIState &fs);
  void resizeEvent(QResizeEvent *event) override;

  // UIEditMode マネージャー設定（OnroadWindow から呼び出し）
  void setEditModeManager(UIEditModeManager *manager) { edit_manager_ = manager; }

protected:
  void mousePressEvent(QMouseEvent *e) override;
  void mouseReleaseEvent(QMouseEvent *e) override;
  void mouseMoveEvent(QMouseEvent *e) override;

public:
  double fps;

  QColor bg;

private:
  void paintEvent(QPaintEvent *event);
  void paintFPS(QPainter &p, const QRect &rect);
  void paintSteeringTorqueBorder(QPainter &p, const QRect &rect);
  void paintTurnSignalBorder(QPainter &p, const QRect &rect);
  void initPlaybackOverlay();
  void readPlaybackParams();       // Runs on background thread
  void applyPlaybackState();       // Runs on UI thread via QTimer
  void updatePlaybackRealTime();
  void stopPlayback();
  void stopParamsThread();

  bool blindSpotLeft;
  bool blindSpotRight;
  bool flickerActive;
  bool isCanPlayback;
  bool showBlindspot;
  bool showFPS;
  bool showSignal;
  bool showSteering;
  bool turnSignalLeft;
  bool turnSignalRight;

  float steer;

  QTimer *signalTimer;
  QTimer *playback_timer_;

  PlaybackOverlay *playback_overlay_;
  QPushButton *stop_playback_btn_;

  double playback_position_ = 0.0;
  double playback_duration_ = 0.0;
  QString playback_start_time_;
  int loading_progress_ = 0;

  // Background Params reader thread
  std::thread params_thread_;
  std::atomic<bool> params_thread_running_{false};
  std::mutex state_mutex_;
  PlaybackState cached_state_;

  // Last applied state for diff detection in applyPlaybackState()
  PlaybackState last_applied_state_;

  // UIEditMode マネージャー（編集モード中のイベント処理制御用）
  UIEditModeManager *edit_manager_ = nullptr;
};
