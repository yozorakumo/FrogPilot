#include "frogpilot/ui/qt/offroad/can_log_settings.h"

#include <QFile>
#include <QProcess>
#include <QThread>
#include <QDebug>

#include "selfdrive/ui/qt/widgets/controls.h"
#include "frogpilot/ui/qt/widgets/frogpilot_controls.h"

// CanLogRouteItem - 個別のルート（走行ログ）エントリ

CanLogRouteItem::CanLogRouteItem(const QString &routePath, QWidget *parent)
  : QWidget(parent), m_routePath(routePath), infoLabel(nullptr), m_timestamp(0), m_gpsTimeUpdated(false) {
  QHBoxLayout *layout = new QHBoxLayout(this);
  layout->setContentsMargins(20, 10, 20, 10);
  layout->setSpacing(15);

  QFileInfo routeInfo(routePath);
  QString routeName = routeInfo.fileName();

  // rlogファイルを探す
  m_rlogPath.clear();

  // 最初のセグメントのrlogファイルを探す
  QDir routeDir(routePath);
  QStringList segFilters;
  segFilters << "--*";
  QStringList segDirs = routeDir.entryList(segFilters, QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
  if (!segDirs.isEmpty()) {
    for (const QString &segDir : segDirs) {
      QString rlog = routeDir.filePath(segDir + "/rlog");
      QString rlogBz2 = routeDir.filePath(segDir + "/rlog.bz2");
      if (QFileInfo(rlog).exists()) {
        m_rlogPath = rlog;
        break;
      } else if (QFileInfo(rlogBz2).exists()) {
        m_rlogPath = rlogBz2;
        break;
      }
    }
  }
  // フォールバック: ルート直下のrlog
  if (m_rlogPath.isEmpty()) {
    if (QFileInfo(routePath + "/rlog").exists()) {
      m_rlogPath = routePath + "/rlog";
    } else if (QFileInfo(routePath + "/rlog.bz2").exists()) {
      m_rlogPath = routePath + "/rlog.bz2";
    }
  }

  // 初期表示（時刻抽出前はファイル更新日時）
  QDateTime recordTime;
  if (!m_rlogPath.isEmpty()) {
    recordTime = QFileInfo(m_rlogPath).lastModified().toUTC();
  }
  if (!recordTime.isValid()) {
    recordTime = routeInfo.lastModified().toUTC();
  }
  if (recordTime.isValid()) {
    m_timestamp = recordTime.toMSecsSinceEpoch() / 1000;
  }

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

  // 情報ラベル
  infoLabel = new QLabel(this);
  infoLabel->setStyleSheet("QLabel { color: #E4E4E4; font-size: 35px; }");
  infoLabel->setWordWrap(true);
  layout->addWidget(infoLabel, 1);

  updateDisplay();

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

CanLogRouteItem::~CanLogRouteItem() {
}

void CanLogRouteItem::updateGpsTime(qint64 timestamp) {
  m_timestamp = timestamp;
  m_gpsTimeUpdated = true;
  updateDisplay();
}

void CanLogRouteItem::updateDisplay() {
  // 時刻表示を更新
  QDateTime recordTime;
  if (m_timestamp > 0) {
    recordTime = QDateTime::fromMSecsSinceEpoch(m_timestamp * 1000, Qt::UTC);
  }

  QString displayText;
  if (recordTime.isValid()) {
    // UTC時間をローカルタイムゾーンで表示
    displayText = recordTime.toLocalTime().toString("yyyy/MM/dd HH:mm");
  } else {
    displayText = QFileInfo(m_routePath).fileName();
  }

  // サイズとセグメント数を再取得
  QDir countDir(m_routePath);
  QStringList countFilters;
  countFilters << "--*";
  int segCount = countDir.entryList(countFilters, QDir::Dirs | QDir::NoDotAndDotDot).size();

  qint64 totalSize = 0;
  QDirIterator it(m_routePath, QDir::Files, QDirIterator::Subdirectories);
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
  QDirIterator rit(m_routePath, QStringList() << "rlog" << "rlog.bz2", QDir::Files, QDirIterator::Subdirectories);
  if (rit.hasNext()) {
    hasRlog = true;
  }

  QString rlogStr = hasRlog ? tr("✓ CAN data available") : tr("✕ No CAN data");

  if (infoLabel) {
    infoLabel->setText(displayText + "\n" + sizeStr + " | " + QString::number(segCount) + " segments\n" + rlogStr);
  }
}

// FrogPilotCanLogPanel - CAN Log管理パネル

FrogPilotCanLogPanel::FrogPilotCanLogPanel(FrogPilotSettingsWindow *parent)
  : FrogPilotListWidget(parent), parent(parent), m_sortOrder(SortDescending) {
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

  // ソートボタン
  sortButton = new QPushButton(tr("↓ Newest First"), this);
  sortButton->setStyleSheet(R"(
    QPushButton {
      padding: 10px 20px;
      border-radius: 5px;
      font-size: 28px;
      font-weight: 500;
      color: #E4E4E4;
      background-color: #393939;
    }
    QPushButton:pressed {
      background-color: #4a4a4a;
    }
  )");
  QObject::connect(sortButton, &QPushButton::clicked, [this]() {
    if (m_sortOrder == SortDescending) {
      setSortOrder(SortAscending);
    } else {
      setSortOrder(SortDescending);
    }
  });
  addItem(sortButton);

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

void FrogPilotCanLogPanel::setSortOrder(SortOrder order) {
  m_sortOrder = order;
  sortButton->setText(order == SortDescending ? tr("↓ Newest First") : tr("↑ Oldest First"));
  refreshFileList();
}

qint64 FrogPilotCanLogPanel::extractGpsTime(const QString &routePath) {
  // extract_route_time.pyスクリプトを実行して補正済みGPS時刻を抽出
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

  QProcess process;
  process.setProgram(python_path);
  process.setArguments({"/data/openpilot/frogpilot/can_log/extract_route_time.py", routePath});
  process.setWorkingDirectory("/data/openpilot");

  process.start();
  if (!process.waitForFinished(15000)) {
    process.kill();
    // フォールバック: ファイル更新日時
    QString rlogPath = findRlogPath(routePath);
    if (!rlogPath.isEmpty()) {
      return QFileInfo(rlogPath).lastModified().toUTC().toMSecsSinceEpoch() / 1000;
    }
    return 0;
  }

  QString output = QString::fromUtfString(process.readAllStandardOutput()).trimmed();
  if (!output.isEmpty()) {
    bool ok;
    qint64 timestamp = output.toLongLong(&ok);
    if (ok && timestamp > 0) {
      return timestamp / 1000000;  // ns to seconds
    }
  }

  // フォールバック: ファイル更新日時
  QString rlogPath = findRlogPath(routePath);
  if (!rlogPath.isEmpty()) {
    return QFileInfo(rlogPath).lastModified().toUTC().toMSecsSinceEpoch() / 1000;
  }
  return 0;
}

QString FrogPilotCanLogPanel::findRlogPath(const QString &routePath) {
  QDir routeDir(routePath);
  QStringList segFilters;
  segFilters << "--*";
  QStringList segDirs = routeDir.entryList(segFilters, QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);

  if (!segDirs.isEmpty()) {
    for (const QString &segDir : segDirs) {
      QString rlog = routeDir.filePath(segDir + "/rlog");
      QString rlogBz2 = routeDir.filePath(segDir + "/rlog.bz2");
      if (QFileInfo(rlog).exists()) return rlog;
      if (QFileInfo(rlogBz2).exists()) return rlogBz2;
    }
  }

  if (QFileInfo(routePath + "/rlog").exists()) return routePath + "/rlog";
  if (QFileInfo(routePath + "/rlog.bz2").exists()) return routePath + "/rlog.bz2";
  return "";
}

void FrogPilotCanLogPanel::updateRouteTimestamps() {
  // 全ルートアイテムを走査してGPS時刻で更新
  QLayoutItem *item = fileListLayout->itemAt(0);
  int index = 0;
  while (item != nullptr) {
    QWidget *widget = item->widget();
    if (widget) {
      CanLogRouteItem *routeItem = qobject_cast<CanLogRouteItem*>(widget);
      if (routeItem) {
        qint64 ts = extractGpsTime(routeItem->routePath());
        if (ts > 0) {
          routeItem->updateGpsTime(ts);
        }
      }
    }
    index++;
    item = fileListLayout->itemAt(index);
  }
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

  // 各ルートエントリを収集（rlogがあるもののみ）
  QList<QPair<QFileInfo, qint64>> validRoutes;
  for (const QFileInfo &routeInfo : routes) {
    QDir routeDir(routeInfo.absoluteFilePath());
    QDirIterator it(routeInfo.absoluteFilePath(), QStringList() << "rlog" << "rlog.bz2", QDir::Files, QDirIterator::Subdirectories);
    if (it.hasNext()) {
      // GPS時刻を抽出（非同期で実行）
      qint64 gpsTime = extractGpsTime(routeInfo.absoluteFilePath());
      validRoutes.append({routeInfo, gpsTime});
    }
  }

  // ソート順 적용
  if (m_sortOrder == SortDescending) {
    std::sort(validRoutes.begin(), validRoutes.end(),
      [](const QPair<QFileInfo, qint64> &a, const QPair<QFileInfo, qint64> &b) {
        return a.second > b.second;
      });
  } else {
    std::sort(validRoutes.begin(), validRoutes.end(),
      [](const QPair<QFileInfo, qint64> &a, const QPair<QFileInfo, qint64> &b) {
        return a.second < b.second;
      });
  }

  // 有効なルートがない場合はステータスメッセージを表示
  if (validRoutes.isEmpty()) {
    statusLabel = new QLabel(tr("No driving logs with CAN data found.\nDrive with openpilot to generate logs."), this);
    statusLabel->setStyleSheet("QLabel { color: #808080; font-size: 35px; padding: 20px; }");
    statusLabel->setAlignment(Qt::AlignCenter);
    statusLabel->setWordWrap(true);
    fileListLayout->addWidget(statusLabel);
    return;
  }

  // ヘッダーラベル
  QLabel *headerLabel = new QLabel(tr("Driving Logs (%1 routes)").arg(validRoutes.size()), this);
  headerLabel->setStyleSheet("QLabel { color: #E0E879; font-size: 40px; font-weight: bold; padding: 15px 20px; }");
  fileListLayout->addWidget(headerLabel);

  // 各ルートエントリを追加
  for (const auto &routePair : validRoutes) {
    const QFileInfo &routeInfo = routePair.first;
    qint64 gpsTime = routePair.second;
    QString routePath = routeInfo.absoluteFilePath();

    CanLogRouteItem *item = new CanLogRouteItem(routePath, this);

    // GPS時刻を更新
    if (gpsTime > 0) {
      item->updateGpsTime(gpsTime);
    }

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