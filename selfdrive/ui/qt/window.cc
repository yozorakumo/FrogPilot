#include "selfdrive/ui/qt/window.h"

#include <QDateTime>
#include <QFontDatabase>
#include <QMouseEvent>
#include <QTouchEvent>

#include "common/params.h"
#include "common/swaglog.h"
#include "system/hardware/hw.h"

MainWindow::MainWindow(QWidget *parent) : QWidget(parent) {
  main_layout = new QStackedLayout(this);
  main_layout->setMargin(0);

  homeWindow = new HomeWindow(this);
  main_layout->addWidget(homeWindow);
  QObject::connect(homeWindow, &HomeWindow::openSettings, this, &MainWindow::openSettings);
  QObject::connect(homeWindow, &HomeWindow::closeSettings, this, &MainWindow::closeSettings);

  settingsWindow = new SettingsWindow(this);
  main_layout->addWidget(settingsWindow);
  QObject::connect(settingsWindow, &SettingsWindow::closeSettings, this, &MainWindow::closeSettings);
  QObject::connect(settingsWindow, &SettingsWindow::reviewTrainingGuide, [=]() {
    onboardingWindow->showTrainingGuide();
    main_layout->setCurrentWidget(onboardingWindow);
  });
  QObject::connect(settingsWindow, &SettingsWindow::showDriverView, [=] {
    homeWindow->showDriverView(true);
  });

  onboardingWindow = new OnboardingWindow(this);
  main_layout->addWidget(onboardingWindow);
  QObject::connect(onboardingWindow, &OnboardingWindow::onboardingDone, [=]() {
    main_layout->setCurrentWidget(homeWindow);
  });
  if (!onboardingWindow->completed()) {
    main_layout->setCurrentWidget(onboardingWindow);
  }

  QObject::connect(uiState(), &UIState::offroadTransition, [=](bool offroad) {
    if (!offroad) {
      closeSettings();
    }
  });
  QObject::connect(device(), &Device::interactiveTimeout, [=]() {
    if (main_layout->currentWidget() == settingsWindow) {
      closeSettings();
    }
  });

  // load fonts
  QFontDatabase::addApplicationFont("../assets/fonts/Inter-Black.ttf");
  QFontDatabase::addApplicationFont("../assets/fonts/Inter-Bold.ttf");
  QFontDatabase::addApplicationFont("../assets/fonts/Inter-ExtraBold.ttf");
  QFontDatabase::addApplicationFont("../assets/fonts/Inter-ExtraLight.ttf");
  QFontDatabase::addApplicationFont("../assets/fonts/Inter-Medium.ttf");
  QFontDatabase::addApplicationFont("../assets/fonts/Inter-Regular.ttf");
  QFontDatabase::addApplicationFont("../assets/fonts/Inter-SemiBold.ttf");
  QFontDatabase::addApplicationFont("../assets/fonts/Inter-Thin.ttf");
  QFontDatabase::addApplicationFont("../assets/fonts/JetBrainsMono-Medium.ttf");

  // no outline to prevent the focus rectangle
  setStyleSheet(R"(
    * {
      font-family: Inter;
      outline: none;
    }
  )");
  setAttribute(Qt::WA_NoSystemBackground);
}

void MainWindow::openSettings(int index, const QString &param) {
  main_layout->setCurrentWidget(settingsWindow);
  settingsWindow->setCurrentPanel(index, param);
}

void MainWindow::closeSettings() {
  main_layout->setCurrentWidget(homeWindow);

  if (uiState()->scene.started) {
    // Map is always shown when using navigate on openpilot
    if (uiState()->scene.navigate_on_openpilot) {
      homeWindow->showMapPanel(true);
    } else {
      homeWindow->showSidebar(params.getBool("Sidebar") || frogpilotUIState()->frogpilot_toggles.value("debug_mode").toBool());
    }
  }
}

bool MainWindow::eventFilter(QObject *obj, QEvent *event) {
  // ===== DEBUG: すべてのイベントをファイルに記録 =====
  {
    static int event_count = 0;
    if (event_count < 500) {  // 最初の500イベントのみ記録（無限ログ防止）
      FILE *f = fopen("/tmp/ui_edit_debug.log", "a");
      if (f) {
        fprintf(f, "event[%d] type=%d obj=%s\n", event_count, (int)event->type(), obj->metaObject()->className());
        fclose(f);
      }
      event_count++;
    }
  }

  // ===== UI EDIT MODE: 長押し検出（タイムスタンプベース・QTimer不使用） =====

  // Press検出
  if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::TouchBegin) {
    QPoint pos;
    if (event->type() == QEvent::TouchBegin) {
      QTouchEvent *te = static_cast<QTouchEvent*>(event);
      if (te->touchPoints().count() > 0) {
        pos = te->touchPoints().first().pos().toPoint();
      }
    } else {
      QMouseEvent *me = static_cast<QMouseEvent*>(event);
      pos = me->pos();
    }
    edit_press_pending_ = true;
    edit_press_time_ = QDateTime::currentMSecsSinceEpoch();
    Params().put("UIEditPressTime", std::to_string(edit_press_time_));
    { FILE *f = fopen("/tmp/ui_edit_debug.log", "a"); if(f) { fprintf(f, "UI EDIT MODE: press detected at (%d, %d), time=%lld\n", pos.x(), pos.y(), edit_press_time_); fclose(f); } }
  }

  // Release検出
  if (event->type() == QEvent::MouseButtonRelease || event->type() == QEvent::TouchEnd) {
    if (edit_press_pending_) {
      qint64 elapsed = QDateTime::currentMSecsSinceEpoch() - edit_press_time_;
      edit_press_pending_ = false;
      Params().put("UIEditPressTime", "0");
      { FILE *f = fopen("/tmp/ui_edit_debug.log", "a"); if(f) { fprintf(f, "UI EDIT MODE: release detected, elapsed=%lldms\n", elapsed); fclose(f); } }
    }
  }

  // NOTE: 長押しチェックは AnnotatedCameraWidget::paintEvent() で行う（~20Hz）
  // Waylandでは指静止中にイベントが来ないため、描画サイクルでチェックする

  // ===== FrogPilot variables (after edit mode detection) =====
  FrogPilotUIState &fs = *frogpilotUIState();
  FrogPilotUIScene &frogpilot_scene = fs.frogpilot_scene;
  QJsonObject &frogpilot_toggles = fs.frogpilot_toggles;

  bool ignore = false;
  switch (event->type()) {
    case QEvent::TouchBegin:
      LOGW("MainWindow eventFilter: TouchBegin, awake=%d, driver_cam=%d", device()->isAwake(), frogpilot_scene.driver_camera_timer >= UI_FREQ / 2);
      // fallthrough
    case QEvent::TouchUpdate:
    case QEvent::TouchEnd:
    case QEvent::MouseButtonPress:
      if (event->type() == QEvent::MouseButtonPress) {
        QMouseEvent *me = static_cast<QMouseEvent*>(event);
        LOGW("MainWindow eventFilter: MouseButtonPress at (%d, %d), awake=%d", me->pos().x(), me->pos().y(), device()->isAwake());
      }
      // fallthrough
    case QEvent::MouseMove: {
      // ignore events when device is awakened by resetInteractiveTimeout
      ignore = !device()->isAwake() || frogpilot_scene.driver_camera_timer >= UI_FREQ / 2;
      device()->resetInteractiveTimeout(frogpilot_toggles.value("screen_timeout").toInt(), frogpilot_toggles.value("screen_timeout_onroad").toInt());
      break;
    }
    default:
      break;
  }
  return ignore;
}
