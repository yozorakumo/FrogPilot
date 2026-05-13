#include "frogpilot/ui/qt/onroad/ui_edit_mode.h"

#include <QPainter>
#include <QDateTime>
#include <QMouseEvent>
#include <QWidget>
#include "common/swaglog.h"

UIEditModeManager::UIEditModeManager(QObject *parent) : QObject(parent) {
  // 長押しタイマー
  long_press_timer_ = new QTimer(this);
  long_press_timer_->setSingleShot(true);
  connect(long_press_timer_, &QTimer::timeout, this, [this]() {
    LOGW("UI Edit: Long press timer fired, press_pending=%d", press_pending_);
    if (press_pending_) {
      edit_mode_ = !edit_mode_;
      LOGW("UI Edit: Edit mode toggled to %d", edit_mode_);
      press_pending_ = false;
      if (!edit_mode_) {
        saveSettings();  // 編集モード終了時に保存
        selected_element_.clear();
      }
    }
  });

  // デフォルト要素を登録
  elements_["speedometer"] = UIElementConfig{"Speedometer"};
  elements_["max_speed"] = UIElementConfig{"Max Speed"};
  elements_["compass"] = UIElementConfig{"Compass"};
  elements_["gear"] = UIElementConfig{"Gear"};
  elements_["speed_limit"] = UIElementConfig{"Speed Limit"};

  loadSettings();
}

void UIEditModeManager::installOnWidget(QWidget *widget) {
  widget->installEventFilter(this);
}

bool UIEditModeManager::eventFilter(QObject *obj, QEvent *e) {
  if (e->type() == QEvent::MouseButtonPress) {
    QMouseEvent *me = static_cast<QMouseEvent*>(e);
    LOGW("UI Edit eventFilter: MouseButtonPress at (%d, %d)", me->pos().x(), me->pos().y());
    handleMousePress(me->pos());
  } else if (e->type() == QEvent::MouseMove) {
    QMouseEvent *me = static_cast<QMouseEvent*>(e);
    handleMouseMove(me->pos());
  } else if (e->type() == QEvent::MouseButtonRelease) {
    LOGW("UI Edit eventFilter: MouseButtonRelease");
    handleMouseRelease();
  } else if (e->type() == QEvent::TouchBegin) {
    LOGW("UI Edit eventFilter: TouchBegin (touch event received)");
  }
  // イベントを消費しない - 親に伝播させる
  return QObject::eventFilter(obj, e);
}

float UIEditModeManager::getOffsetX(const QString &name) const {
  auto it = elements_.find(name);
  return it != elements_.end() ? it->offset_x : 0.0f;
}

float UIEditModeManager::getOffsetY(const QString &name) const {
  auto it = elements_.find(name);
  return it != elements_.end() ? it->offset_y : 0.0f;
}

float UIEditModeManager::getScale(const QString &name) const {
  auto it = elements_.find(name);
  return it != elements_.end() ? it->scale : 1.0f;
}

void UIEditModeManager::updateBounds(const QString &name, const QRect &bounds) {
  if (elements_.contains(name)) {
    elements_[name].bounds = bounds;
  }
}

bool UIEditModeManager::handleMousePress(const QPoint &pos) {
  LOGW("UI Edit: handleMousePress at (%d, %d), press_pending=%d, edit_mode=%d", pos.x(), pos.y(), press_pending_, edit_mode_);
  press_pos_ = pos;
  press_pending_ = true;
  long_press_timer_->start(LONG_PRESS_MS);

  if (edit_mode_) {
    // 既に編集モード: 要素選択
    selected_element_.clear();
    for (auto it = elements_.begin(); it != elements_.end(); ++it) {
      QRect expanded = it->bounds.adjusted(-30, -30, 30, 30);
      if (expanded.contains(pos)) {
        selected_element_ = it.key();
        drag_start_pos_ = pos;
        drag_start_offset_x_ = it->offset_x;
        drag_start_offset_y_ = it->offset_y;
        is_dragging_ = true;
        long_press_timer_->stop();
        press_pending_ = false;
        return true;
      }
    }
    return true;  // 編集モード中はイベントを消費
  }
  return false;
}

bool UIEditModeManager::handleMouseMove(const QPoint &pos) {
  if (press_pending_) {
    // 移動距離が閾値を超えたら長押しキャンセル
    if ((pos - press_pos_).manhattanLength() > MOVE_THRESHOLD) {
      long_press_timer_->stop();
      press_pending_ = false;
    }
  }

  if (edit_mode_ && is_dragging_ && !selected_element_.isEmpty()) {
    QPoint delta = pos - drag_start_pos_;
    elements_[selected_element_].offset_x = drag_start_offset_x_ + delta.x();
    elements_[selected_element_].offset_y = drag_start_offset_y_ + delta.y();
    return true;
  }
  return false;
}

bool UIEditModeManager::handleMouseRelease() {
  LOGW("UI Edit: handleMouseRelease, press_pending=%d, edit_mode=%d", press_pending_, edit_mode_);
  long_press_timer_->stop();
  press_pending_ = false;
  is_dragging_ = false;
  return edit_mode_;
}

void UIEditModeManager::paintOverlay(QPainter &p, int width, int height) {
  if (!edit_mode_) return;

  // 半透明オーバーレイ
  p.fillRect(0, 0, width, height, QColor(0, 0, 0, 40));

  // 各要素の境界を描画
  for (auto it = elements_.begin(); it != elements_.end(); ++it) {
    if (it->bounds.isEmpty()) continue;

    bool selected = (it.key() == selected_element_);
    QRect bounds = it->bounds.adjusted(-5, -5, 5, 5);

    // 境界矩形
    p.setPen(QPen(selected ? QColor(0, 255, 255) : QColor(255, 255, 255, 150), selected ? 3 : 1, Qt::DashLine));
    p.setBrush(Qt::NoBrush);
    p.drawRect(bounds);

    // 要素名ラベル
    p.setFont(QFont("Inter", 14, QFont::Normal));
    p.setPen(selected ? QColor(0, 255, 255) : QColor(255, 255, 255, 200));
    p.drawText(bounds.topLeft() + QPoint(0, -5), it->name);
  }

  // 操作説明テキスト
  p.setFont(QFont("Inter", 18, QFont::Bold));
  p.setPen(QColor(255, 255, 255));
  p.drawText(QRect(0, 50, width, 40), Qt::AlignCenter, "UI EDIT MODE - Drag to move, long press to exit");

  if (!selected_element_.isEmpty()) {
    auto &elem = elements_[selected_element_];
    p.setFont(QFont("Inter", 14));
    p.drawText(QRect(0, 90, width, 30), Qt::AlignCenter,
      QString("%1 | Offset: (%2, %3) | Scale: %4")
        .arg(elem.name)
        .arg(QString::number(elem.offset_x, 'f', 0))
        .arg(QString::number(elem.offset_y, 'f', 0))
        .arg(QString::number(elem.scale, 'f', 2)));
  }
}

void UIEditModeManager::loadSettings() {
  loadFromParams();
}

void UIEditModeManager::saveSettings() {
  saveToParams();
}

void UIEditModeManager::loadFromParams() {
  Params params;
  QString json = QString::fromStdString(params.get("UIElementPositions"));
  if (json.isEmpty()) return;

  QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
  if (!doc.isObject()) return;

  QJsonObject obj = doc.object();
  for (auto it = elements_.begin(); it != elements_.end(); ++it) {
    if (obj.contains(it.key())) {
      QJsonObject elem = obj[it.key()].toObject();
      it->offset_x = elem.value("x").toDouble();
      it->offset_y = elem.value("y").toDouble();
      it->scale = elem.value("s").toDouble(1.0);
    }
  }
}

void UIEditModeManager::saveToParams() {
  QJsonObject obj;
  for (auto it = elements_.begin(); it != elements_.end(); ++it) {
    QJsonObject elem;
    elem["x"] = it->offset_x;
    elem["y"] = it->offset_y;
    elem["s"] = it->scale;
    obj[it.key()] = elem;
  }
  QJsonDocument doc(obj);
  Params params;
  params.put("UIElementPositions", doc.toJson().toStdString());
}

void UIEditModeManager::resetAll() {
  for (auto it = elements_.begin(); it != elements_.end(); ++it) {
    it->offset_x = 0.0f;
    it->offset_y = 0.0f;
    it->scale = 1.0f;
  }
  saveSettings();
}