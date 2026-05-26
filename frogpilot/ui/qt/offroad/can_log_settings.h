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
#include <QThread>
#include <QMutex>
#include <QWaitCondition>
#include <QReadWriteLock>

#include "frogpilot/ui/qt/offroad/frogpilot_settings.h"

// GpsTimeExtractor - バックグラウンドGPS時刻抽出クラス
// QThreadを使用してextract_gps_time.pyをバックグラウンドで実行し、
// 完了時にシグナルを発行する（非同期処理）
class GpsTimeExtractor : public QObject {
  Q_OBJECT

public:
  explicit GpsTimeExtractor(QObject *parent = nullptr);
  ~GpsTimeExtractor();

  // 非同期GPS時刻抽出を開始
  // routePath: rlogファイルを含むルートディレクトリ
  // rlogPath: rlogファイルのパス（rlog または rlog.bz2）
  void extractAsync(const QString &routePath, const QString &rlogPath);

  // 実行中の抽出をキャンセル
  void cancel();

  // ロックを取得して安全なアクセスを提供
  qint64 getTimestamp() const;
  bool isExtracting() const;

signals:
  // GPS時刻の抽出が完了した時に発火
  // routePath: ルートディレクトリのパス
  // timestamp: UNIXタイムスタンプ（秒）、失敗時は0
  void gpsTimeExtracted(const QString &routePath, qint64 timestamp);

  // 抽出に失敗した時に発火
  void extractionFailed(const QString &routePath);

private:
  QThread *workerThread;
  QString currentRoutePath;
  QString currentRlogPath;
  QMutex mutex;
  volatile bool cancelled;
  qint64 m_timestamp;
  bool m_extracting;

  static void workerThreadFunc(GpsTimeExtractor *extractor);
};

// CanLogRouteItem - 個別のルート（走行ログ）エントリ
class CanLogRouteItem : public QWidget {
  Q_OBJECT

public:
  explicit CanLogRouteItem(const QString &routePath, QWidget *parent = nullptr);
  ~CanLogRouteItem();

  QString routePath() const { return m_routePath; }
  QString rlogPath() const { return m_rlogPath; }

  // GPS時刻が非同期で抽出された後に呼び出し
  void updateGpsTime(qint64 timestamp);

signals:
  void playClicked(const QString &routePath);
  void deleteClicked(const QString &routePath);

private:
  QString m_routePath;
  QString m_rlogPath;
  QLabel *infoLabel;
  qint64 m_timestamp;
  bool m_gpsTimeUpdated;

  void updateDisplay();
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