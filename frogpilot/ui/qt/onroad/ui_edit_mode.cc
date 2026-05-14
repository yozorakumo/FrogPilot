#include "frogpilot/ui/qt/onroad/ui_edit_mode.h"

#include <QPainter>
#include <QDateTime>
#include <QMouseEvent>
#include <QWidget>
#include <cmath>
#include <cstdio>

UIEditModeManager::UIEditModeManager(QObject *parent) : QObject(parent) {
  // 長押しタイマー
  long_press_timer_ = new QTimer(this);
  long_press_timer_->setSingleShot(true);
  connect(long_press_timer_, &QTimer::timeout, this, [this]() {
    if (press_pending_) {
      edit_mode_ = !edit_mode_;
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

void UIEditModeManager::toggleEditMode() {
  edit_mode_ = !edit_mode_;
  if (!edit_mode_) {
    saveSettings();
    selected_element_.clear();
  }
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
  press_pos_ = pos;
  press_pending_ = true;
  long_press_timer_->start(LONG_PRESS_MS);

  if (edit_mode_) {
    // オーバーレイボタンの判定
    if (reset_btn_rect_.contains(pos)) {
      long_press_timer_->stop();
      press_pending_ = false;
      resetAll();
      return true;
    }
    if (save_btn_rect_.contains(pos)) {
      long_press_timer_->stop();
      press_pending_ = false;
      saveSettings();
      return true;
    }
    if (exit_btn_rect_.contains(pos)) {
      long_press_timer_->stop();
      press_pending_ = false;
      edit_mode_ = false;
      saveSettings();
      selected_element_.clear();
      return true;
    }

    // 要素選択
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
    float new_x = drag_start_offset_x_ + delta.x();
    float new_y = drag_start_offset_y_ + delta.y();

    // 境界外ガード
    auto &elem = elements_[selected_element_];
    if (!elem.bounds.isEmpty()) {
      QRect moved_bounds = elem.bounds.translated(new_x, new_y);
      // 簡易的な画面内制約（boundsが大きく画面外に出ないように）
      new_x = std::clamp(new_x, -500.0f, 500.0f);
      new_y = std::clamp(new_y, -500.0f, 500.0f);
    }

    elem.offset_x = new_x;
    elem.offset_y = new_y;
    return true;
  }
  return false;
}

bool UIEditModeManager::handleMouseRelease() {
  long_press_timer_->stop();
  press_pending_ = false;
  is_dragging_ = false;
  return edit_mode_;
}

bool UIEditModeManager::handleDoubleClick(const QPoint &pos) {
  if (!edit_mode_) return false;

  // ヒットテスト
  for (auto it = elements_.begin(); it != elements_.end(); ++it) {
    QRect expanded = it->bounds.adjusted(-30, -30, 30, 30);
    if (expanded.contains(pos)) {
      selected_element_ = it.key();
      // スケールサイクル: 0.75 → 1.0 → 1.25 → 1.5 → 0.75...
      float current = it->scale;
      if (current < 0.875f) {
        it->scale = 1.0f;
      } else if (current < 1.125f) {
        it->scale = 1.25f;
      } else if (current < 1.375f) {
        it->scale = 1.5f;
      } else {
        it->scale = 0.75f;
      }
      return true;
    }
  }
  return false;
}

bool UIEditModeManager::handlePinchZoom(const QPoint &center, float scale_delta) {
  if (!edit_mode_) return false;

  // ピンチ中心点にある要素を選択
  if (selected_element_.isEmpty()) {
    for (auto it = elements_.begin(); it != elements_.end(); ++it) {
      QRect expanded = it->bounds.adjusted(-30, -30, 30, 30);
      if (expanded.contains(center)) {
        selected_element_ = it.key();
        break;
      }
    }
  }

  if (!selected_element_.isEmpty()) {
    auto &elem = elements_[selected_element_];
    elem.scale = std::clamp(elem.scale * scale_delta, SCALE_MIN, SCALE_MAX);
    return true;
  }
  return false;
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
  p.drawText(QRect(0, 50, width, 40), Qt::AlignCenter,
    "UI EDIT MODE - Drag/pinch to move & resize, long press to exit");

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

  // オーバーレイボタン（画面下部）
  int btn_y = height - BTN_HEIGHT - BTN_MARGIN;
  int total_btn_width = BTN_WIDTH * 3 + BTN_MARGIN * 2;
  int btn_start_x = (width - total_btn_width) / 2;

  // RESET ボタン
  reset_btn_rect_ = QRect(btn_start_x, btn_y, BTN_WIDTH, BTN_HEIGHT);
  p.setBrush(QColor(0, 0, 0, 180));
  p.setPen(QPen(QColor(255, 165, 0), 2));
  p.drawRoundedRect(reset_btn_rect_, 24, 24);
  p.setFont(QFont("Inter", 22, QFont::Bold));
  p.setPen(QColor(255, 165, 0));
  p.drawText(reset_btn_rect_, Qt::AlignCenter, "RESET");

  // SAVE ボタン
  save_btn_rect_ = QRect(btn_start_x + BTN_WIDTH + BTN_MARGIN, btn_y, BTN_WIDTH, BTN_HEIGHT);
  p.setBrush(QColor(0, 0, 0, 180));
  p.setPen(QPen(QColor(0, 200, 100), 2));
  p.drawRoundedRect(save_btn_rect_, 24, 24);
  p.setFont(QFont("Inter", 22, QFont::Bold));
  p.setPen(QColor(0, 200, 100));
  p.drawText(save_btn_rect_, Qt::AlignCenter, "SAVE");

  // EXIT ボタン
  exit_btn_rect_ = QRect(btn_start_x + (BTN_WIDTH + BTN_MARGIN) * 2, btn_y, BTN_WIDTH, BTN_HEIGHT);
  p.setBrush(QColor(0, 0, 0, 180));
  p.setPen(QPen(QColor(255, 80, 80), 2));
  p.drawRoundedRect(exit_btn_rect_, 24, 24);
  p.setFont(QFont("Inter", 22, QFont::Bold));
  p.setPen(QColor(255, 80, 80));
  p.drawText(exit_btn_rect_, Qt::AlignCenter, "EXIT");
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