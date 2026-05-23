#pragma once

#include "selfdrive/ui/qt/onroad/annotated_camera.h"

#include "frogpilot/ui/qt/onroad/playback_overlay.h"

class FrogPilotOnroadWindow : public QWidget {
  Q_OBJECT

public:
  FrogPilotOnroadWindow(QWidget* parent = 0);

  void updateState(const UIState &s, const FrogPilotUIState &fs);
  void resizeEvent(QResizeEvent *event) override;

  double fps;

  QColor bg;

private:
  void paintEvent(QPaintEvent *event);
  void paintFPS(QPainter &p, const QRect &rect);
  void paintSteeringTorqueBorder(QPainter &p, const QRect &rect);
  void paintTurnSignalBorder(QPainter &p, const QRect &rect);
  void initPlaybackOverlay();
  void updatePlaybackPosition();
  void updatePlaybackRealTime();

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

  double playback_position_ = 0.0;
  double playback_duration_ = 0.0;
  QString playback_start_time_;
  int loading_progress_ = 0;
};
