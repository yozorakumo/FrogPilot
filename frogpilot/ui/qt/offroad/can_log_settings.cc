#include "frogpilot/ui/qt/offroad/can_log_settings.h"

#include "selfdrive/ui/qt/widgets/controls.h"
#include "frogpilot/ui/qt/widgets/frogpilot_controls.h"

// CanLogFileItem - 個別のログファイルエントリ

CanLogFileItem::CanLogFileItem(const QString &filepath, QWidget *parent) : QWidget(parent), filePath(filepath) {
  QHBoxLayout *layout = new QHBoxLayout(this);
  layout->setContentsMargins(20, 10, 20, 10);
  layout->setSpacing(15);

  QFileInfo fileInfo(filepath);

  // ファイル名（日付部分を抽出してフォーマット）
  QString baseName = fileInfo.completeBaseName();
  // can_log_20240101_120000 → 2024-01-01 12:00:00
  QString displayDate = baseName;
  displayDate.remove("can_log_");
  if (displayDate.length() >= 15) {
    displayDate = displayDate.mid(0, 4) + "-" + displayDate.mid(4, 2) + "-" + displayDate.mid(6, 2) +
                  " " + displayDate.mid(9, 2) + ":" + displayDate.mid(11, 2) + ":" + displayDate.mid(13, 2);
  }

  // ファイルサイズ
  qint64 fileSize = fileInfo.size();
  QString sizeStr;
  if (fileSize >= 1024 * 1024) {
    sizeStr = QString::number(fileSize / (1024 * 1024.0), 'f', 1) + " MB";
  } else if (fileSize >= 1024) {
    sizeStr = QString::number(fileSize / 1024.0, 'f', 1) + " KB";
  } else {
    sizeStr = QString::number(fileSize) + " B";
  }

  QString compressedStr = filepath.endsWith(".gz") ? tr(" [Compressed]") : "";

  // 情報ラベル
  QLabel *infoLabel = new QLabel(displayDate + "\n" + sizeStr + compressedStr, this);
  infoLabel->setStyleSheet("QLabel { color: #E4E4E4; font-size: 35px; }");
  infoLabel->setWordWrap(true);
  layout->addWidget(infoLabel, 1);

  // 再生ボタン
  QPushButton *playButton = new QPushButton(tr("▶ Play"), this);
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
  )");
  QObject::connect(playButton, &QPushButton::clicked, [this]() {
    emit playClicked(filePath);
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
    emit deleteClicked(filePath);
  });
  layout->addWidget(deleteButton);

  setStyleSheet("QWidget { border-bottom: 1px solid #393939; }");
}

// FrogPilotCanLogPanel - CAN Log管理パネル

FrogPilotCanLogPanel::FrogPilotCanLogPanel(FrogPilotSettingsWindow *parent)
  : FrogPilotListWidget(parent), parent(parent) {

  // ステータスラベル
  statusLabel = new QLabel(tr("No CAN log files found."), this);
  statusLabel->setStyleSheet("QLabel { color: #808080; font-size: 35px; padding: 20px; }");
  statusLabel->setAlignment(Qt::AlignCenter);
  statusLabel->setWordWrap(true);

  // ログ記録ON/OFFトグル
  ParamControl *canLoggingToggle = new ParamControl("CanLoggingEnabled",
    tr("CAN Bus Logging"),
    tr("<b>Enable CAN bus message logging while driving.</b> Log files are saved to /data/can_logs/."),
    "../../frogpilot/assets/toggle_icons/icon_system.png");
  addItem(canLoggingToggle);

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
    tr("<b>Permanently delete all CAN log files from the device.</b>"));
  QObject::connect(deleteAllButton, &ButtonControl::clicked, [this]() {
    if (FrogPilotConfirmationDialog::yesorno(tr("Are you sure you want to delete ALL CAN log files? This cannot be undone."), this)) {
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
    statusLabel = new QLabel(tr("No CAN log directory found.\nLogs will be saved to /data/can_logs/ when recording is enabled."), this);
    statusLabel->setStyleSheet("QLabel { color: #808080; font-size: 35px; padding: 20px; }");
    statusLabel->setAlignment(Qt::AlignCenter);
    statusLabel->setWordWrap(true);
    fileListLayout->addWidget(statusLabel);
    return;
  }

  // canbinファイルとcanbin.gzファイルを収集
  QStringList filters;
  filters << "can_log_*.canbin" << "can_log_*.canbin.gz";
  QFileInfoList files = logDir.entryInfoList(filters, QDir::Files, QDir::Time | QDir::Reversed);

  if (files.isEmpty()) {
    statusLabel = new QLabel(tr("No CAN log files found."), this);
    statusLabel->setStyleSheet("QLabel { color: #808080; font-size: 35px; padding: 20px; }");
    statusLabel->setAlignment(Qt::AlignCenter);
    statusLabel->setWordWrap(true);
    fileListLayout->addWidget(statusLabel);
    return;
  }

  // ヘッダーラベル
  QLabel *headerLabel = new QLabel(tr("CAN Log Files (%1)").arg(files.size()), this);
  headerLabel->setStyleSheet("QLabel { color: #E0E879; font-size: 40px; font-weight: bold; padding: 15px 20px; }");
  fileListLayout->addWidget(headerLabel);

  // 各ファイルエントリを追加
  for (const QFileInfo &fileInfo : files) {
    CanLogFileItem *item = new CanLogFileItem(fileInfo.absoluteFilePath(), this);

    QObject::connect(item, &CanLogFileItem::playClicked, [this](const QString &filepath) {
      startPlayback(filepath);
    });

    QObject::connect(item, &CanLogFileItem::deleteClicked, [this](const QString &filepath) {
      if (FrogPilotConfirmationDialog::yesorno(tr("Delete this log file?"), this)) {
        deleteFile(filepath);
      }
    });

    fileListLayout->addWidget(item);
  }

  // 合計サイズを表示
  qint64 totalSize = 0;
  for (const QFileInfo &fileInfo : files) {
    totalSize += fileInfo.size();
  }

  QString totalStr;
  if (totalSize >= 1024 * 1024 * 1024) {
    totalStr = QString::number(totalSize / (1024.0 * 1024 * 1024), 'f', 2) + " GB";
  } else if (totalSize >= 1024 * 1024) {
    totalStr = QString::number(totalSize / (1024.0 * 1024), 'f', 1) + " MB";
  } else {
    totalStr = QString::number(totalSize / 1024.0, 'f', 1) + " KB";
  }

  QLabel *totalLabel = new QLabel(tr("Total: %1 (%2 files)").arg(totalStr).arg(files.size()), this);
  totalLabel->setStyleSheet("QLabel { color: #808080; font-size: 30px; padding: 10px 20px; }");
  totalLabel->setAlignment(Qt::AlignRight);
  fileListLayout->addWidget(totalLabel);
}

void FrogPilotCanLogPanel::deleteFile(const QString &filepath) {
  QFile file(filepath);
  if (file.remove()) {
    refreshFileList();
  }
}

void FrogPilotCanLogPanel::deleteAllLogs() {
  QDir logDir(LOG_DIR);
  if (!logDir.exists()) return;

  QStringList filters;
  filters << "can_log_*.canbin" << "can_log_*.canbin.gz";
  QStringList files = logDir.entryList(filters, QDir::Files);

  for (const QString &fileName : files) {
    QFile::remove(logDir.absoluteFilePath(fileName));
  }

  refreshFileList();
}

void FrogPilotCanLogPanel::startPlayback(const QString &filepath) {
  // 再生ファイルパスをParamsに設定してCAN_PLAYBACKモードで再起動
  params.put("CanPlaybackFile", filepath.toStdString());
  params.putBool("CanPlaybackMode", true);

  // 再起動を促すダイアログ
  if (FrogPilotConfirmationDialog::yesorno(
    tr("Start CAN playback with this file?\nThe device will reboot into playback mode."), this)) {
    Hardware::reboot();
  }
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