#pragma once

#include <QDir>
#include <QFileInfo>
#include <QDateTime>
#include <QProcess>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTimer>

#include "frogpilot/ui/qt/offroad/frogpilot_settings.h"

class CanLogRouteItem : public QWidget {
  Q_OBJECT

public:
  explicit CanLogRouteItem(const QString &routePath, QWidget *parent = nullptr);

  QString routePath() const { return m_routePath; }

signals:
  void playClicked(const QString &routePath);
  void deleteClicked(const QString &routePath);

private:
  QString m_routePath;
};

class FrogPilotCanLogPanel : public FrogPilotListWidget {
  Q_OBJECT

public:
  explicit FrogPilotCanLogPanel(FrogPilotSettingsWindow *parent);

signals:
  void openSubPanel();
  void requestCloseSettings();

protected:
  void showEvent(QShowEvent *event) override;

private:
  void refreshFileList();
  void deleteRoute(const QString &routePath);
  void deleteAllLogs();
  void startPlayback(const QString &routePath);
  QString formatFileSize(qint64 bytes) const;
  QString formatDuration(float seconds) const;

  FrogPilotSettingsWindow *parent;

  QVBoxLayout *fileListLayout;
  QWidget *fileListWidget;

  QLabel *statusLabel;

  Params params;
  Params params_memory{"/dev/shm/params"};

  static constexpr const char *LOG_DIR = "/data/media/0/realdata/";
};