#include "selfdrive/ui/qt/onroad/onroad_home.h"

#include <QCoreApplication>
#include <QPainter>
#include <QStackedLayout>

#ifdef ENABLE_MAPS
#include "selfdrive/ui/qt/maps/map_helpers.h"
#include "selfdrive/ui/qt/maps/map_panel.h"
#endif

#include "selfdrive/ui/qt/util.h"

OnroadWindow::OnroadWindow(QWidget *parent) : QWidget(parent) {
  QVBoxLayout *main_layout  = new QVBoxLayout(this);
  main_layout->setMargin(UI_BORDER_SIZE);
  QStackedLayout *stacked_layout = new QStackedLayout;
  stacked_layout->setStackingMode(QStackedLayout::StackAll);
  main_layout->addLayout(stacked_layout);

  nvg = new AnnotatedCameraWidget(VISION_STREAM_ROAD, this);

  QWidget * split_wrapper = new QWidget;
  split = new QHBoxLayout(split_wrapper);
  split->setContentsMargins(0, 0, 0, 0);
  split->setSpacing(0);
  split->addWidget(nvg);

  if (getenv("DUAL_CAMERA_VIEW")) {
    CameraWidget *arCam = new CameraWidget("camerad", VISION_STREAM_ROAD, true, this);
    split->insertWidget(0, arCam);
  }

  if (getenv("MAP_RENDER_VIEW")) {
    CameraWidget *map_render = new CameraWidget("navd", VISION_STREAM_MAP, false, this);
    split->insertWidget(0, map_render);
  }

  stacked_layout->addWidget(split_wrapper);

  alerts = new OnroadAlerts(this);
  alerts->setAttribute(Qt::WA_TransparentForMouseEvents, true);
  stacked_layout->addWidget(alerts);

  // setup stacking order
  alerts->raise();

  setAttribute(Qt::WA_OpaquePaintEvent);
  QObject::connect(uiState(), &UIState::uiUpdate, this, &OnroadWindow::updateState);
  QObject::connect(uiState(), &UIState::offroadTransition, this, &OnroadWindow::offroadTransition);
  QObject::connect(uiState(), &UIState::primeChanged, this, &OnroadWindow::primeChanged);

  // FrogPilot variables
  frogpilot_onroad = new FrogPilotOnroadWindow(this);
  frogpilot_onroad->setEditModeManager(edit_manager_);

  // UI Edit Mode - 長押し検出はMainWindow::eventFilter()に統合済み
  // UIEditModeManagerはオフセット管理のみに使用
  edit_manager_ = new UIEditModeManager(this);
  nvg->setEditModeManager(edit_manager_);
}

void OnroadWindow::resizeEvent(QResizeEvent *event) {
  QWidget::resizeEvent(event);

  frogpilot_onroad->setGeometry(rect());
}

void OnroadWindow::updateState(const UIState &s, const FrogPilotUIState &fs) {
  if (!s.scene.started) {
    return;
  }

  if (s.scene.map_on_left) {
    split->setDirection(QBoxLayout::LeftToRight);
  } else {
    split->setDirection(QBoxLayout::RightToLeft);
  }

  alerts->updateState(s, fs);
  nvg->updateState(s, fs);

  QColor bgColor = bg_colors[s.status];
  if (bg != bgColor) {
    // repaint border
    bg = bgColor;
    update();
  }

  // FrogPilot variables
  frogpilot_onroad->bg = bg;
  frogpilot_onroad->fps = nvg->fps;

  nvg->frogpilot_nvg->alertHeight = alerts->alertHeight;

  frogpilot_onroad->updateState(s, fs);
}

void OnroadWindow::mousePressEvent(QMouseEvent* e) {
  // 再入防止: sendEvent → nvgがignore() → Qtが親に伝播 → 再びmousePressEvent
  // という無限再帰を防ぐ
  if (handling_mouse_event_) {
    QWidget::mousePressEvent(e);
    return;
  }
  handling_mouse_event_ = true;

  // 編集モード中または長押し追跡中は AnnotatedCameraWidget に転送
  if (edit_manager_ && (edit_manager_->isEditMode() || edit_manager_->isPressPending())) {
    if (nvg) {
      QPoint nvg_pos = nvg->mapFromGlobal(e->globalPos());
      QMouseEvent forwarded(QEvent::MouseButtonPress, nvg_pos, e->globalPos(),
                            e->button(), e->buttons(), e->modifiers());
      // 直接メソッド呼び出し（sendEventを使わない）で再帰を防止
      nvg->forwardMousePress(&forwarded);
      bool nvg_handled = forwarded.isAccepted();
      handling_mouse_event_ = false;
      if (nvg_handled) {
        e->accept();
      } else {
        e->ignore();
      }
      return;
    }
  }

  handling_mouse_event_ = false;

  // 通常時は特別な処理なしでイベントを HomeWindow に伝播
  // （サイドバー/開発者サイドバーのトグルは HomeWindow::mousePressEvent で処理）
  e->ignore();
}

void OnroadWindow::createMapWidget() {
  FrogPilotUIState &fs = *frogpilotUIState();
  QJsonObject &frogpilot_toggles = fs.frogpilot_toggles;

  if (frogpilot_toggles.value("hide_map").toBool()) {
    return;
  }

#ifdef ENABLE_MAPS
  auto m = new MapPanel(get_mapbox_settings());
  map = m;
  QObject::connect(m, &MapPanel::mapPanelRequested, this, &OnroadWindow::mapPanelRequested);
  QObject::connect(nvg->map_settings_btn, &MapSettingsButton::clicked, m, &MapPanel::toggleMapSettings);
  nvg->map_settings_btn->setEnabled(true);

  m->setFixedWidth(topWidget(this)->width() / 2 - UI_BORDER_SIZE);
  split->insertWidget(0, m);
  // hidden by default, made visible when navRoute is published
  m->setVisible(false);
#endif
}

void OnroadWindow::offroadTransition(bool offroad) {
#ifdef ENABLE_MAPS
  if (!offroad) {
    if (map == nullptr && !MAPBOX_TOKEN.isEmpty()) {
      createMapWidget();
    }
  }
#endif
  alerts->clear();
  if (!offroad) {
    alerts->enableFerg = util::random_int(0, 1) == 1;
  } else {
    alerts->displayFerg = false;
  }
}

void OnroadWindow::primeChanged(bool prime) {
#ifdef ENABLE_MAPS
  if (map && (!prime && MAPBOX_TOKEN.isEmpty())) {
    nvg->map_settings_btn->setEnabled(false);
    nvg->map_settings_btn->setVisible(false);
    map->deleteLater();
    map = nullptr;
  } else if (!map && (prime || !MAPBOX_TOKEN.isEmpty())) {
    createMapWidget();
  }
#endif
}

void OnroadWindow::mouseMoveEvent(QMouseEvent* e) {
  // 再入防止
  if (handling_mouse_event_) {
    QWidget::mouseMoveEvent(e);
    return;
  }
  handling_mouse_event_ = true;

  // 編集モード中または長押し追跡中はAnnotatedCameraWidgetに転送
  if (edit_manager_ && (edit_manager_->isEditMode() || edit_manager_->isPressPending())) {
    if (nvg) {
      QPoint nvg_pos = nvg->mapFromGlobal(e->globalPos());
      QMouseEvent forwarded(QEvent::MouseMove, nvg_pos, e->globalPos(),
                            e->button(), e->buttons(), e->modifiers());
      nvg->forwardMouseMove(&forwarded);
    }
    handling_mouse_event_ = false;
    e->accept();
    return;
  }

  handling_mouse_event_ = false;
  QWidget::mouseMoveEvent(e);
}

void OnroadWindow::mouseReleaseEvent(QMouseEvent* e) {
  // 再入防止
  if (handling_mouse_event_) {
    QWidget::mouseReleaseEvent(e);
    return;
  }
  handling_mouse_event_ = true;

  // 編集モード中または長押し追跡中はAnnotatedCameraWidgetに転送
  if (edit_manager_ && (edit_manager_->isEditMode() || edit_manager_->isPressPending())) {
    if (nvg) {
      QPoint nvg_pos = nvg->mapFromGlobal(e->globalPos());
      QMouseEvent forwarded(QEvent::MouseButtonRelease, nvg_pos, e->globalPos(),
                            e->button(), e->buttons(), e->modifiers());
      nvg->forwardMouseRelease(&forwarded);
    }
    handling_mouse_event_ = false;
    e->accept();
    return;
  }

  handling_mouse_event_ = false;
  QWidget::mouseReleaseEvent(e);
}

void OnroadWindow::paintEvent(QPaintEvent *event) {
  QPainter p(this);
  p.fillRect(rect(), QColor(bg.red(), bg.green(), bg.blue(), 255));
}
