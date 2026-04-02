#!/bin/bash
# GitHub Actions Runner 復元スクリプト (A1m用)
# 秘密情報を含む設定ファイルは PC の作業ディレクトリからコピーされたものを復元します

RUNNER_VERSION="2.315.0"
INSTALL_DIR="/data/actions-runner"
# リポジトリ直下の runner_backup ディレクトリ（PC側で保持されているもの）
BACKUP_DIR="/data/openpilot/runner_backup"

echo "Restoring GitHub Actions Runner..."

# ディレクトリ作成
sudo mkdir -p $INSTALL_DIR
sudo chown $(whoami):$(whoami) $INSTALL_DIR
cd $INSTALL_DIR

# ランナー本体をダウンロードして解凍 (存在しない場合)
if [ ! -f "config.sh" ]; then
  echo "Downloading runner binary..."
  curl -o actions-runner-linux-arm64-$RUNNER_VERSION.tar.gz -L https://github.com/actions/runner/releases/download/v$RUNNER_VERSION/actions-runner-linux-arm64-$RUNNER_VERSION.tar.gz
  tar xzf ./actions-runner-linux-arm64-$RUNNER_VERSION.tar.gz
fi

# バックアップから設定ファイルを復元
if [ -f "$BACKUP_DIR/.runner" ]; then
  echo "Restoring configuration files from $BACKUP_DIR..."
  cp $BACKUP_DIR/.runner .
  cp $BACKUP_DIR/.credentials .
  cp $BACKUP_DIR/.credentials_rsaparams .
  
  # 権限の調整
  chmod 600 .credentials .credentials_rsaparams
  
  # サービスとして再登録・起動
  echo "Installing and starting runner service..."
  sudo ./svc.sh install
  sudo ./svc.sh start
  echo "Runner restoration complete and started!"
else
  echo "Error: Backup directory $BACKUP_DIR or configuration files not found."
  echo "Please ensure runner_backup/ is placed in the repository root on the device."
  exit 1
fi