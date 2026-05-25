#pragma once

#include "selfdrive/ui/qt/onroad/alerts.h"
#include "selfdrive/ui/qt/onroad/annotated_camera.h"

#include "frogpilot/ui/qt/onroad/frogpilot_onroad.h"
#include "frogpilot/ui/qt/onroad/ui_edit_mode.h"

class OnroadWindow : public QWidget {
  Q_OBJECT

public:
  OnroadWindow(QWidget* parent = 0);
  bool isMapVisible() const { return map && map->isVisible(); }
  void showMapPanel(bool show) { if (map) map->setVisible(show); }

signals:
  void mapPanelRequested();

private:
  void createMapWidget();
  void paintEvent(QPaintEvent *event);
  void mousePressEvent(QMouseEvent* e) override;
  void mouseMoveEvent(QMouseEvent* e) override;
  void mouseReleaseEvent(QMouseEvent* e) override;
  OnroadAlerts *alerts;
  AnnotatedCameraWidget *nvg;
  QColor bg = bg_colors[STATUS_DISENGAGED];
  QWidget *map = nullptr;
  QHBoxLayout* split;

  // FrogPilot variables
  void resizeEvent(QResizeEvent *event);

  FrogPilotOnroadWindow *frogpilot_onroad;

  // UI Edit Mode - イベントはOnroadWindowレベルで処理
  UIEditModeManager *edit_manager_;

private slots:
  void offroadTransition(bool offroad);
  void primeChanged(bool prime);
  void updateState(const UIState &s, const FrogPilotUIState &fs);
};
