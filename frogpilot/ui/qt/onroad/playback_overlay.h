#pragma once

#include <QWidget>
#include <QSlider>
#include <QLabel>
#include <QPushButton>
#include <QTimer>

class PlaybackOverlay : public QWidget {
  Q_OBJECT

public:
  explicit PlaybackOverlay(QWidget *parent = nullptr);

  void setDuration(double seconds);
  void setPosition(double seconds);
  void setRealTime(const QString &time_str);
  void setPlaybackSpeed(double speed);
  void setPlaying(bool playing);
  bool isPlaying() const { return is_playing_; }

  void showOverlay();
  void hideOverlay();

  static constexpr int kOverlayHeight = 80;

signals:
  void seekRequested(double position_seconds);
  void playPauseRequested();
  void speedChangeRequested(double speed);

protected:
  void paintEvent(QPaintEvent *event) override;
  void mousePressEvent(QMouseEvent *event) override;

private:
  void updateDisplay();
  void resetHideTimer();
  QString formatTime(double seconds) const;
  void cycleSpeed();

  QSlider *seek_slider_;
  QLabel *time_label_;
  QLabel *real_time_label_;
  QPushButton *play_pause_btn_;
  QPushButton *speed_btn_;

  double duration_seconds_ = 0.0;
  double position_seconds_ = 0.0;
  QString real_time_str_;
  double playback_speed_ = 1.0;
  bool is_playing_ = false;

  QTimer *hide_timer_;
  int bg_alpha_ = 128;

  static constexpr int kHideTimeoutMs = 3000;
};