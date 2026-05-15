#pragma once

#include <QMap>
#include <QPainter>
#include <QPoint>
#include <QRect>
#include <QTimer>
#include <QString>
#include <QJsonObject>
#include <QJsonDocument>
#include <QWidget>
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

public:

  // 編集モード状態
  bool isEditMode() const { return edit_mode_; }
  void toggleEditMode();

  // UI要素の設定取得
  float getOffsetX(const QString &name) const;
  float getOffsetY(const QString &name) const;
  float getScale(const QString &name) const;

  // 要素の境界矩形を更新（各描画関数から呼ぶ）
  void updateBounds(const QString &name, const QRect &bounds);

  // スケールを反映した有効な境界矩形を取得
  QRect getEffectiveBounds(const QString &name) const;

  // マウス/タッチイベント処理
  bool handleMousePress(const QPoint &pos);
  bool handleMouseMove(const QPoint &pos);
  bool handleMouseRelease();
  bool handleDoubleClick(const QPoint &pos);
  bool handlePinchZoom(const QPoint &center, float scale_delta);

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
  static constexpr int LONG_PRESS_MS = 2000;
  static constexpr int MOVE_THRESHOLD = 20;

  // ピンチズーム状態
  float pinch_start_scale_ = 1.0f;
  float last_pinch_distance_ = 0.0f;

  // 要素設定
  QMap<QString, UIElementConfig> elements_;

  // オーバーレイボタン
  QRect reset_btn_rect_;
  QRect save_btn_rect_;
  QRect exit_btn_rect_;
  QRect zoom_in_btn_rect_;
  QRect zoom_out_btn_rect_;
  static constexpr int BTN_WIDTH = 180;
  static constexpr int BTN_HEIGHT = 60;
  static constexpr int BTN_MARGIN = 20;
  static constexpr int ZOOM_BTN_SIZE = 60;
  static constexpr int ZOOM_BTN_GAP = 20;

  // スケール制限
  static constexpr float SCALE_MIN = 0.5f;
  static constexpr float SCALE_MAX = 2.0f;

  // 設定をParamsに保存
  void saveToParams();
  void loadFromParams();
};