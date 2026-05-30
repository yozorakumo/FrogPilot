#include "frogpilot/ui/qt/onroad/playback_overlay.h"

#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QStyle>
#include <QStyleOptionSlider>

#include "selfdrive/ui/qt/util.h"

namespace {
const QColor kSliderTrackColor = QColor(80, 80, 80, 200);
const QColor kSliderProgressColor = QColor(23, 134, 68, 242);
const QColor kSliderHandleColor = QColor(255, 255, 255, 230);
const QColor kTextColor = Qt::white;
const QColor kButtonColor = QColor(255, 255, 255, 200);
}

PlaybackOverlay::PlaybackOverlay(QWidget *parent) : QWidget(parent) {
  setAttribute(Qt::WA_TransparentForMouseEvents, false);
  setAttribute(Qt::WA_NoSystemBackground, true);
  setAttribute(Qt::WA_TranslucentBackground, true);
  setStyleSheet("background: transparent;");

  // Main layout
  QHBoxLayout *main_layout = new QHBoxLayout(this);
  main_layout->setContentsMargins(20, 10, 20, 10);
  main_layout->setSpacing(15);

  // Play/Pause button
  play_pause_btn_ = new QPushButton("▶", this);
  play_pause_btn_->setFixedSize(40, 40);
  play_pause_btn_->setStyleSheet(R"(
    QPushButton {
      background: rgba(255, 255, 255, 30);
      color: white;
      border: none;
      border-radius: 20px;
      font-size: 18px;
    }
    QPushButton:pressed {
      background: rgba(255, 255, 255, 80);
    }
  )");
  QObject::connect(play_pause_btn_, &QPushButton::clicked, [this]() {
    emit playPauseRequested();
  });
  main_layout->addWidget(play_pause_btn_);

  // Time label (left)
  time_label_ = new QLabel("00:00 / 00:00", this);
  time_label_->setStyleSheet(R"(
    QLabel {
      color: white;
      font-size: 14px;
      font-weight: bold;
      background: transparent;
    }
  )");
  time_label_->setFixedWidth(130);
  time_label_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
  main_layout->addWidget(time_label_);

  // Seek slider
  seek_slider_ = new QSlider(Qt::Horizontal, this);
  seek_slider_->setRange(0, 1000);
  seek_slider_->setValue(0);
  seek_slider_->setStyleSheet(R"(
    QSlider::groove:horizontal {
      background: rgba(80, 80, 80, 200);
      height: 6px;
      border-radius: 3px;
    }
    QSlider::sub-page:horizontal {
      background: rgba(23, 134, 68, 242);
      height: 6px;
      border-radius: 3px;
    }
    QSlider::handle:horizontal {
      background: white;
      width: 16px;
      height: 16px;
      margin: -5px 0;
      border-radius: 8px;
    }
    QSlider::handle:horizontal:pressed {
      background: rgba(23, 134, 68, 242);
    }
  )");
  QObject::connect(seek_slider_, &QSlider::sliderMoved, [this](int value) {
    double position = (static_cast<double>(value) / 1000.0) * duration_seconds_;
    emit seekRequested(position);
    resetHideTimer();
  });
  main_layout->addWidget(seek_slider_, 1);

  // Speed button
  speed_btn_ = new QPushButton("1.0x", this);
  speed_btn_->setFixedSize(55, 35);
  speed_btn_->setStyleSheet(R"(
    QPushButton {
      background: rgba(255, 255, 255, 30);
      color: white;
      border: none;
      border-radius: 5px;
      font-size: 13px;
      font-weight: bold;
    }
    QPushButton:pressed {
      background: rgba(255, 255, 255, 80);
    }
  )");
  QObject::connect(speed_btn_, &QPushButton::clicked, [this]() {
    cycleSpeed();
    resetHideTimer();
  });
  main_layout->addWidget(speed_btn_);

  // Real time label (right)
  real_time_label_ = new QLabel("", this);
  real_time_label_->setStyleSheet(R"(
    QLabel {
      color: rgba(255, 255, 255, 180);
      font-size: 13px;
      background: transparent;
    }
  )");
  real_time_label_->setFixedWidth(190);
  real_time_label_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
  main_layout->addWidget(real_time_label_);

  // Auto-hide timer
  hide_timer_ = new QTimer(this);
  hide_timer_->setSingleShot(true);
  QObject::connect(hide_timer_, &QTimer::timeout, [this]() {
    bg_alpha_ = 40;
    update();
  });

  setFixedHeight(kOverlayHeight);
  hide();
}

void PlaybackOverlay::setDuration(double seconds) {
  if (duration_seconds_ == seconds) return;
  duration_seconds_ = seconds;
  updateDisplay();
}

void PlaybackOverlay::setPosition(double seconds) {
  position_seconds_ = seconds;
  if (!seek_slider_->isSliderDown()) {
    int slider_value = (duration_seconds_ > 0)
                           ? static_cast<int>((position_seconds_ / duration_seconds_) * 1000)
                           : 0;
    int new_value = std::clamp(slider_value, 0, 1000);
    if (seek_slider_->value() != new_value) {
      seek_slider_->setValue(new_value);
    }
  }
  // Only update time display when whole seconds change
  int new_secs = static_cast<int>(seconds);
  if (new_secs != prev_displayed_secs_) {
    prev_displayed_secs_ = new_secs;
    updateDisplay();
  }
}

void PlaybackOverlay::setRealTime(const QString &time_str) {
  if (real_time_str_ == time_str) return;
  real_time_str_ = time_str;
  real_time_label_->setText(time_str);
}

void PlaybackOverlay::setPlaybackSpeed(double speed) {
  if (playback_speed_ == speed) return;
  playback_speed_ = speed;
  speed_btn_->setText(QString::number(speed, 'f', 1) + "x");
}

void PlaybackOverlay::setPlaying(bool playing) {
  if (is_playing_ == playing) return;
  is_playing_ = playing;
  play_pause_btn_->setText(playing ? "⏸" : "▶");
}

void PlaybackOverlay::showOverlay() {
  show();
  update();
  resetHideTimer();
}

void PlaybackOverlay::showEvent(QShowEvent *event) {
  QWidget::showEvent(event);
  if (parentWidget()) {
    // 親ウィジェットの幅に合わせてオーバーレイのgeometryを更新
    setGeometry(0, parentWidget()->height() - height(), parentWidget()->width(), height());
    updateGeometry();
    update();
  }
}

void PlaybackOverlay::hideOverlay() {
  hide();
  hide_timer_->stop();
}

void PlaybackOverlay::paintEvent(QPaintEvent *event) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);

  // Semi-transparent background
  p.fillRect(rect(), QColor(0, 0, 0, bg_alpha_));

  // Bottom border line
  QPen line_pen(QColor(255, 255, 255, 40));
  line_pen.setWidth(1);
  p.setPen(line_pen);
  p.drawLine(rect().topLeft(), rect().topRight());

  QWidget::paintEvent(event);
}

void PlaybackOverlay::mousePressEvent(QMouseEvent *event) {
  resetHideTimer();
  event->accept();
}

void PlaybackOverlay::mouseReleaseEvent(QMouseEvent *event) {
  event->accept();
}

bool PlaybackOverlay::event(QEvent *event) {
  // 子widget内のイベント処理は吸収
  // 子widgetの範囲外のイベントのみ親widgetに伝播させる
  switch (event->type()) {
    case QEvent::TouchBegin:
    case QEvent::TouchUpdate:
    case QEvent::TouchEnd:
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
    case QEvent::MouseMove:
      // Press/Release/TouchBegin/TouchEnd: 子widgetの範囲外なら親に伝播
      // （再生オーバーレイ表示中にサイドバートグルを可能にするため）
      // MouseMove: パフォーマンス上の理由で常にaccept
      if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::TouchBegin ||
          event->type() == QEvent::MouseButtonRelease || event->type() == QEvent::TouchEnd) {
        QMouseEvent *mouseEvent = nullptr;
        QTouchEvent *touchEvent = nullptr;
        if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonRelease) {
          mouseEvent = static_cast<QMouseEvent*>(event);
        } else {
          touchEvent = static_cast<QTouchEvent*>(event);
        }

        QPoint pos;
        if (mouseEvent) {
          pos = mouseEvent->pos();
        } else if (touchEvent && !touchEvent->touchPoints().isEmpty()) {
          pos = touchEvent->touchPoints().first().pos().toPoint();
        }

        // 子widgetの範囲内チェック
        bool inChildWidget = false;
        inChildWidget |= (play_pause_btn_ && play_pause_btn_->geometry().contains(pos));
        inChildWidget |= (seek_slider_ && seek_slider_->geometry().contains(pos));
        inChildWidget |= (speed_btn_ && speed_btn_->geometry().contains(pos));
        inChildWidget |= (time_label_ && time_label_->geometry().contains(pos));
        inChildWidget |= (real_time_label_ && real_time_label_->geometry().contains(pos));

        if (!inChildWidget) {
          // 子widget範囲外なら親widgetに伝播
          event->ignore();
          return false;
        }
      }
      return true;
    default:
      return QWidget::event(event);
  }
}

void PlaybackOverlay::updateDisplay() {
  // ローディング表示: durationが0またはpositionが0でまだ再生開始前
  if (duration_seconds_ <= 0.0) {
    time_label_->setText("Loading...");
    return;
  }
  time_label_->setText(formatTime(position_seconds_) + " / " + formatTime(duration_seconds_));
}

void PlaybackOverlay::resetHideTimer() {
  bg_alpha_ = 128;
  update();
  hide_timer_->start(kHideTimeoutMs);
}

QString PlaybackOverlay::formatTime(double seconds) const {
  int total_secs = static_cast<int>(seconds);
  int mins = total_secs / 60;
  int secs = total_secs % 60;
  return QString("%1:%2")
      .arg(mins, 2, 10, QChar('0'))
      .arg(secs, 2, 10, QChar('0'));
}

void PlaybackOverlay::cycleSpeed() {
  // 0.5x → 1.0x → 2.0x → 4.0x → 0.5x cycle
  const double speeds[] = {0.5, 1.0, 2.0, 4.0};
  const int num_speeds = 4;

  int current_index = 0;
  for (int i = 0; i < num_speeds; ++i) {
    if (qFuzzyCompare(speeds[i], playback_speed_)) {
      current_index = i;
      break;
    }
  }

  int next_index = (current_index + 1) % num_speeds;
  double new_speed = speeds[next_index];
  setPlaybackSpeed(new_speed);
  emit speedChangeRequested(new_speed);
}