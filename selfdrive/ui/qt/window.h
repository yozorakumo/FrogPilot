#pragma once

#include <QDateTime>
#include <QPoint>
#include <QStackedLayout>
#include <QWidget>

#include "selfdrive/ui/qt/home.h"
#include "selfdrive/ui/qt/offroad/onboarding.h"
#include "selfdrive/ui/qt/offroad/settings.h"

class MainWindow : public QWidget {
  Q_OBJECT

public:
  explicit MainWindow(QWidget *parent = 0);

private:
  bool eventFilter(QObject *obj, QEvent *event) override;
  void openSettings(int index = 0, const QString &param = "");
  void closeSettings();

  QStackedLayout *main_layout;
  HomeWindow *homeWindow;
  SettingsWindow *settingsWindow;
  OnboardingWindow *onboardingWindow;

  // FrogPilot variables
  Params params;

  // UI Edit Mode (long press detection in MainWindow::eventFilter)
  qint64 edit_press_time_ = 0;
  bool edit_mode_ = false;
  QPoint edit_press_pos_;
  bool edit_press_pending_ = false;
  static constexpr int EDIT_LONG_PRESS_MS = 2000;
  static constexpr int EDIT_MOVE_THRESHOLD = 50;
};
