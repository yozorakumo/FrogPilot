#!/bin/bash
# GitHub Actions Runner 復元スクリプト (A1m用)

RUNNER_VERSION="2.315.0"
INSTALL_DIR="/data/actions-runner"
BACKUP_DIR="/data/openpilot/runner_backup"

echo "Restoring GitHub Actions Runner..."

# ディレクトリ作成
sudo mkdir -p $INSTALL_DIR
sudo chown $(whoami):$(whoami) $INSTALL_DIR
cd $INSTALL_DIR

# ランナー本体をダウンロードして解凍 (存在しない場合)
if [ ! -f "config.sh" ]; then
  curl -o actions-runner-linux-arm64-$RUNNER_VERSION.tar.gz -L https://github.com/actions/runner/releases/download/v$RUNNER_VERSION/actions-runner-linux-arm64-$RUNNER_VERSION.tar.gz
  tar xzf ./actions-runner-linux-arm64-$RUNNER_VERSION.tar.gz
fi

# バックアップから設定ファイルを復元
if [ -d "$BACKUP_DIR" ]; then
  cp $BACKUP_DIR/.runner .
  cp $BACKUP_DIR/.credentials .
  cp $BACKUP_DIR/.credentials_rsaparams .
  echo "Configuration files restored from $BACKUP_DIR"
else
  echo "Error: Backup directory $BACKUP_DIR not found. You need to run ./config.sh manually."
  exit 1
fi

# サービスとして再登録・起動
sudo ./svc.sh install
sudo ./svc.sh start

echo "Runner restoration complete and started!"