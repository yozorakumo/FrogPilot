#include "frogpilot/ui/qt/offroad/can_log_settings.h"

#include <QFile>
#include <QProcess>

#include "selfdrive/ui/qt/widgets/controls.h"
#include "frogpilot/ui/qt/widgets/frogpilot_controls.h"

// CanLogRouteItem - 個別のルート（走行ログ）エントリ

CanLogRouteItem::CanLogRouteItem(const QString &routePath, QWidget *parent) : QWidget(parent), m_routePath(routePath) {
  QHBoxLayout *layout = new QHBoxLayout(this);
  layout->setContentsMargins(20, 10, 20, 10);
  layout->setSpacing(15);

  QFileInfo routeInfo(routePath);
  QString routeName = routeInfo.fileName();

  // rlogファイルのmtimeから記録日時を取得（ディレクトリmtimeより正確）
  QString displayText;
  QDateTime recordTime;
  // 最初のセグメントのrlogファイルを探す
  QDir routeDir(routePath);
  QStringList segFilters;
  segFilters << "--*";
  QStringList segDirs = routeDir.entryList(segFilters, QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
  if (!segDirs.isEmpty()) {
    // 最初のセグメントのrlog/rlog.bz2のmtimeを使用
    for (const QString &segDir : segDirs) {
      QString rlogPath = routeDir.filePath(segDir + "/rlog");
      QString rlogBz2Path = routeDir.filePath(segDir + "/rlog.bz2");
      QFileInfo rlogInfo(rlogPath);
      QFileInfo rlogBz2Info(rlogBz2Path);
      if (rlogInfo.exists()) {
        recordTime = rlogInfo.lastModified().toUTC();
        break;
      } else if (rlogBz2Info.exists()) {
        recordTime = rlogBz2Info.lastModified().toUTC();
        break;
      }
    }
  }
  // フォールバック: rlogが見つからなければディレクトリのmtime
  if (!recordTime.isValid()) {
    // ルート直下のrlogをチェック
    QFileInfo rlogInfo(routePath + "/rlog");
    QFileInfo rlogBz2Info(routePath + "/rlog.bz2");
    if (rlogInfo.exists()) {
      recordTime = rlogInfo.lastModified().toUTC();
    } else if (rlogBz2Info.exists()) {
      recordTime = rlogBz2Info.lastModified().toUTC();
    } else {
      recordTime = routeInfo.lastModified().toUTC();
    }
  }
  if (recordTime.isValid()) {
    // UTC時間をローカルタイムゾーンで表示
    displayText = recordTime.toLocalTime().toString("yyyy/MM/dd HH:mm");
  } else {
    displayText = routeName;
  }

  // セグメント数をカウント
  QDir routeDir(routePath);
  QStringList segFilters;
  segFilters << "--*";
  int segCount = routeDir.entryList(segFilters, QDir::Dirs).size();

  // ルートの総サイズを計算
  qint64 totalSize = 0;
  QDirIterator it(routePath, QDir::Files, QDirIterator::Subdirectories);
  while (it.hasNext()) {
    it.next();
    totalSize += it.fileInfo().size();
  }

  QString sizeStr;
  if (totalSize >= 1024 * 1024 * 1024) {
    sizeStr = QString::number(totalSize / (1024.0 * 1024 * 1024), 'f', 2) + " GB";
  } else if (totalSize >= 1024 * 1024) {
    sizeStr = QString::number(totalSize / (1024.0 * 1024), 'f', 1) + " MB";
  } else {
    sizeStr = QString::number(totalSize / 1024.0, 'f', 1) + " KB";
  }

  // rlogの存在確認
  bool hasRlog = false;
  QDirIterator rit(routePath, QStringList() << "rlog" << "rlog.bz2", QDir::Files, QDirIterator::Subdirectories);
  if (rit.hasNext()) {
    hasRlog = true;
  }

  QString rlogStr = hasRlog ? tr("✓ CAN data available") : tr("✕ No CAN data");

  // 情報ラベル（日付/時刻 + サイズ + セグメント数 + CAN データ有無）
  QLabel *infoLabel = new QLabel(displayText + "\n" + sizeStr + " | " + QString::number(segCount) + " segments\n" + rlogStr, this);
  infoLabel->setStyleSheet("QLabel { color: #E4E4E4; font-size: 35px; }");
  infoLabel->setWordWrap(true);
  layout->addWidget(infoLabel, 1);

  // 再生ボタン
  QPushButton *playButton = new QPushButton(tr("▶ Play"), this);
  playButton->setEnabled(hasRlog);
  playButton->setStyleSheet(R"(
    QPushButton {
      padding: 0px 20px;
      border-radius: 50px;
      font-size: 30px;
      font-weight: 500;
      height: 80px;
      min-width: 140px;
      color: #E4E4E4;
      background-color: #393939;
    }
    QPushButton:pressed {
      background-color: #4a4a4a;
    }
    QPushButton:disabled {
      color: #606060;
      background-color: #2a2a2a;
    }
  )");
  QObject::connect(playButton, &QPushButton::clicked, [this]() {
    emit playClicked(m_routePath);
  });
  layout->addWidget(playButton);

  // 削除ボタン
  QPushButton *deleteButton = new QPushButton(tr("✕"), this);
  deleteButton->setStyleSheet(R"(
    QPushButton {
      padding: 0px 15px;
      border-radius: 50px;
      font-size: 30px;
      font-weight: 500;
      height: 80px;
      min-width: 80px;
      color: #E4E4E4;
      background-color: #393939;
    }
    QPushButton:pressed {
      background-color: #4a4a4a;
    }
  )");
  QObject::connect(deleteButton, &QPushButton::clicked, [this]() {
    emit deleteClicked(m_routePath);
  });
  layout->addWidget(deleteButton);

  setStyleSheet("QWidget { border-bottom: 1px solid #393939; }");
}

// FrogPilotCanLogPanel - CAN Log管理パネル

FrogPilotCanLogPanel::FrogPilotCanLogPanel(FrogPilotSettingsWindow *parent)
  : FrogPilotListWidget(parent), parent(parent) {

  // ステータスラベル
  statusLabel = new QLabel(tr("No driving logs found."), this);
  statusLabel->setStyleSheet("QLabel { color: #808080; font-size: 35px; padding: 20px; }");
  statusLabel->setAlignment(Qt::AlignCenter);
  statusLabel->setWordWrap(true);

  // 説明ラベル
  QLabel *descLabel = new QLabel(tr("Driving logs are automatically recorded by loggerd.\nCAN data is included in each route's rlog file."), this);
  descLabel->setStyleSheet("QLabel { color: #A0A0A0; font-size: 30px; padding: 15px 20px; }");
  descLabel->setAlignment(Qt::AlignCenter);
  descLabel->setWordWrap(true);
  addItem(descLabel);

  // ファイルリストコンテナ
  fileListWidget = new QWidget(this);
  fileListLayout = new QVBoxLayout(fileListWidget);
  fileListLayout->setContentsMargins(0, 0, 0, 0);
  fileListLayout->setSpacing(0);
  fileListLayout->addWidget(statusLabel);
  addItem(fileListWidget);

  // 全ログ削除ボタン
  ButtonControl *deleteAllButton = new ButtonControl(tr("Delete All Logs"),
    tr("DELETE ALL"),
    tr("<b>Permanently delete all driving log files from the device.</b>"));
  QObject::connect(deleteAllButton, &ButtonControl::clicked, [this]() {
    if (FrogPilotConfirmationDialog::yesorno(tr("Are you sure you want to delete ALL driving log files? This cannot be undone."), this)) {
      deleteAllLogs();
    }
  });
  addItem(deleteAllButton);
}

void FrogPilotCanLogPanel::showEvent(QShowEvent *event) {
  refreshFileList();
  FrogPilotListWidget::showEvent(event);
}

void FrogPilotCanLogPanel::refreshFileList() {
  // 既存のファイルリストをクリア
  QLayoutItem *child;
  while ((child = fileListLayout->takeAt(0)) != nullptr) {
    if (child->widget()) {
      child->widget()->deleteLater();
    }
    delete child;
  }

  QDir logDir(LOG_DIR);
  if (!logDir.exists()) {
    statusLabel = new QLabel(tr("No log directory found.\nLogs are saved to /data/media/0/realdata/ while driving."), this);
    statusLabel->setStyleSheet("QLabel { color: #808080; font-size: 35px; padding: 20px; }");
    statusLabel->setAlignment(Qt::AlignCenter);
    statusLabel->setWordWrap(true);
    fileListLayout->addWidget(statusLabel);
    return;
  }

  // ルートディレクトリを収集（--を含むディレクトリ名 = ルート）
  QStringList routeFilters;
  routeFilters << "*--*";
  QFileInfoList routes = logDir.entryInfoList(routeFilters, QDir::Dirs | QDir::NoDotAndDotDot, QDir::Time);

  if (routes.isEmpty()) {
    statusLabel = new QLabel(tr("No driving logs found."), this);
    statusLabel->setStyleSheet("QLabel { color: #808080; font-size: 35px; padding: 20px; }");
    statusLabel->setAlignment(Qt::AlignCenter);
    statusLabel->setWordWrap(true);
    fileListLayout->addWidget(statusLabel);
    return;
  }

  // ヘッダーラベル
  QLabel *headerLabel = new QLabel(tr("Driving Logs (%1 routes)").arg(routes.size()), this);
  headerLabel->setStyleSheet("QLabel { color: #E0E879; font-size: 40px; font-weight: bold; padding: 15px 20px; }");
  fileListLayout->addWidget(headerLabel);

  // 各ルートエントリを追加
  for (const QFileInfo &routeInfo : routes) {
    // rlogが存在するか確認
    bool hasRlog = false;
    QDir routeDir(routeInfo.absoluteFilePath());
    QDirIterator it(routeInfo.absoluteFilePath(), QStringList() << "rlog" << "rlog.bz2", QDir::Files, QDirIterator::Subdirectories);
    if (it.hasNext()) {
      hasRlog = true;
    }

    // rlogがないルートはスキップ（CAN再生に使用できないため）
    if (!hasRlog) {
      continue;
    }

    CanLogRouteItem *item = new CanLogRouteItem(routeInfo.absoluteFilePath(), this);

    QObject::connect(item, &CanLogRouteItem::playClicked, [this](const QString &routePath) {
      startPlayback(routePath);
    });

    QObject::connect(item, &CanLogRouteItem::deleteClicked, [this](const QString &routePath) {
      if (FrogPilotConfirmationDialog::yesorno(tr("Delete this driving log?"), this)) {
        deleteRoute(routePath);
      }
    });

    fileListLayout->addWidget(item);
  }

  // 合計サイズを表示
  qint64 totalSize = 0;
  QDirIterator totalIt(LOG_DIR, QDir::Files, QDirIterator::Subdirectories);
  while (totalIt.hasNext()) {
    totalIt.next();
    totalSize += totalIt.fileInfo().size();
  }

  QString totalStr;
  if (totalSize >= 1024 * 1024 * 1024) {
    totalStr = QString::number(totalSize / (1024.0 * 1024 * 1024), 'f', 2) + " GB";
  } else if (totalSize >= 1024 * 1024) {
    totalStr = QString::number(totalSize / (1024.0 * 1024), 'f', 1) + " MB";
  } else {
    totalStr = QString::number(totalSize / 1024.0, 'f', 1) + " KB";
  }

  QLabel *totalLabel = new QLabel(tr("Total: %1").arg(totalStr), this);
  totalLabel->setStyleSheet("QLabel { color: #808080; font-size: 30px; padding: 10px 20px; }");
  totalLabel->setAlignment(Qt::AlignRight);
  fileListLayout->addWidget(totalLabel);
}

void FrogPilotCanLogPanel::deleteRoute(const QString &routePath) {
  QDir routeDir(routePath);
  routeDir.removeRecursively();
  refreshFileList();
}

void FrogPilotCanLogPanel::deleteAllLogs() {
  QDir logDir(LOG_DIR);
  if (!logDir.exists()) return;

  QStringList routeFilters;
  routeFilters << "*--*";
  QFileInfoList routes = logDir.entryInfoList(routeFilters, QDir::Dirs | QDir::NoDotAndDotDot);

  for (const QFileInfo &routeInfo : routes) {
    QDir routeDir(routeInfo.absoluteFilePath());
    routeDir.removeRecursively();
  }

  refreshFileList();
}

void FrogPilotCanLogPanel::startPlayback(const QString &routePath) {
  // 確認ダイアログ
  if (!FrogPilotConfirmationDialog::yesorno(
    tr("Start CAN playback with this route?"), this)) {
    return;
  }

  // 再生パラメータを設定
  params.put("CanPlaybackFile", routePath.toStdString());
  params.putBool("CAN_PLAYBACK", true);

  // can_player.py をバックグラウンドで起動
  // デバイスの python3 は Python 3.8 (capnpなし) を指すため、
  // launch_env.sh から PYTHON パスを取得するか、pyenv のパスを使用
  QString python_path = "/usr/local/pyenv/versions/3.11.4/bin/python3";
  QFile env_file("/data/openpilot/launch_env.sh");
  if (env_file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    while (!env_file.atEnd()) {
      QString line = env_file.readLine();
      if (line.startsWith("export PYTHON=")) {
        python_path = line.split('=').last().trimmed().remove('"').remove('\'');
        break;
      }
    }
    env_file.close();
  }
  QProcess::startDetached(python_path, {"-m", "frogpilot.can_log.can_player", routePath}, "/data/openpilot");

  // video_player バイナリも起動（HEVC映像をVisionIPCで配信）
  // fcamera.hevcが存在する場合のみ映像配信が行われる
  QProcess::startDetached("/data/openpilot/frogpilot/can_log/video_player", {routePath}, "/data/openpilot");

  // 設定画面を閉じてホーム画面に戻る
  // CAN メッセージがパブリッシュされると自動的に onroad UI に切り替わる
  emit requestCloseSettings();
}

QString FrogPilotCanLogPanel::formatFileSize(qint64 bytes) const {
  if (bytes >= 1024 * 1024 * 1024) {
    return QString::number(bytes / (1024.0 * 1024 * 1024), 'f', 2) + " GB";
  } else if (bytes >= 1024 * 1024) {
    return QString::number(bytes / (1024.0 * 1024), 'f', 1) + " MB";
  } else if (bytes >= 1024) {
    return QString::number(bytes / 1024.0, 'f', 1) + " KB";
  }
  return QString::number(bytes) + " B";
}

QString FrogPilotCanLogPanel::formatDuration(float seconds) const {
  int mins = (int)(seconds / 60);
  int secs = (int)(seconds) % 60;
  return QString("%1:%2").arg(mins).arg(secs, 2, 10, QChar('0'));
}