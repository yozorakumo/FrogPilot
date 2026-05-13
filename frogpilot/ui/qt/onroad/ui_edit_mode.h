#pragma once

#include <QMap>
#include <QPainter>
#include <QPoint>
#include <QRect>
#include <QTimer>
#include <QString>
#include <QJsonObject>
#include <QJsonDocument>
#include <common/params.h>

// 編集可能なUI要素の定義
struct UIElementConfig {
  QString name;
  float offset_x = 0.0f;  // デフォルト位置からのX オフセット (px)
  float offset_y = 0.0f;  // デフォルト位置からのY オフセット (px)
  float scale = 1.0f;      // スケール (0.5-2.0)
  QRect bounds;            // 要素のデフォルト境界矩形 (編集モードでのヒットテスト用)
};

class UIEditModeManager : public QObject {
  Q_OBJECT

public:
  explicit UIEditModeManager(QObject *parent = nullptr);

  // 編集モード状態
  bool isEditMode() const { return edit_mode_; }

  // UI要素の設定取得
  float getOffsetX(const QString &name) const;
  float getOffsetY(const QString &name) const;
  float getScale(const QString &name) const;

  // 要素の境界矩形を更新（各描画関数から呼ぶ）
  void updateBounds(const QString &name, const QRect &bounds);

  // マウスイベント処理
  bool handleMousePress(const QPoint &pos);
  bool handleMouseMove(const QPoint &pos);
  bool handleMouseRelease();

  // 編集モードオーバーレイ描画
  void paintOverlay(QPainter &p, int width, int height);

  // 設定の保存・読み込み
  void loadSettings();
  void saveSettings();

  // リセット
  void resetAll();

private:
  bool edit_mode_ = false;
  QString selected_element_;
  QPoint drag_start_pos_;
  float drag_start_offset_x_ = 0.0f;
  float drag_start_offset_y_ = 0.0f;
  bool is_dragging_ = false;

  // 長押し検出
  QTimer *long_press_timer_;
  QPoint press_pos_;
  bool press_pending_ = false;
  static constexpr int LONG_PRESS_MS = 3000;
  static constexpr int MOVE_THRESHOLD = 20;

  // 要素設定
  QMap<QString, UIElementConfig> elements_;

  // 設定をParamsに保存
  void saveToParams();
  void loadFromParams();
};