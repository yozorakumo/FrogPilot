#pragma once

#include <QDir>
#include <QFileInfo>
#include <QDateTime>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTimer>

#include "frogpilot/ui/qt/offroad/frogpilot_settings.h"

class CanLogFileItem : public QWidget {
  Q_OBJECT

public:
  explicit CanLogFileItem(const QString &filepath, QWidget *parent = nullptr);

  QString filepath() const { return filePath; }

signals:
  void playClicked(const QString &filepath);
  void deleteClicked(const QString &filepath);

private:
  QString filePath;
};

class FrogPilotCanLogPanel : public FrogPilotListWidget {
  Q_OBJECT

public:
  explicit FrogPilotCanLogPanel(FrogPilotSettingsWindow *parent);

signals:
  void openSubPanel();

protected:
  void showEvent(QShowEvent *event) override;

private:
  void refreshFileList();
  void deleteFile(const QString &filepath);
  void deleteAllLogs();
  void startPlayback(const QString &filepath);
  QString formatFileSize(qint64 bytes) const;
  QString formatDuration(float seconds) const;

  FrogPilotSettingsWindow *parent;

  QVBoxLayout *fileListLayout;
  QWidget *fileListWidget;

  QLabel *statusLabel;

  Params params;
  Params params_memory{"/dev/shm/params"};

  static constexpr const char *LOG_DIR = "/data/can_logs/";
};