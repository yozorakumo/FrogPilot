#!/bin/bash
# GitHub Actions Self-hosted Runner セットアップスクリプト
# comma three (ARM64 Linux) 用
#
# 使い方:
#   ./setup_runner.sh <RUNNER_TOKEN>
#
# RUNNER_TOKEN は GitHub → Settings → Actions → Runners → New self-hosted runner から取得
# 例: ./setup_runner.sh AAAAAAxxxxx...

set -e

RUNNER_TOKEN="${1:?Usage: $0 <RUNNER_TOKEN>}"
RUNNER_VERSION="2.321.0"
INSTALL_DIR="/data/actions-runner"
REPO_URL="https://github.com/yozorakumo/FrogPilot"

echo "=== GitHub Actions Self-hosted Runner Setup ==="
echo "Repository: ${REPO_URL}"
echo "Install dir: ${INSTALL_DIR}"
echo "Runner version: ${RUNNER_VERSION}"
echo ""

# 既存のrunnerがあれば停止・削除
if [ -d "$INSTALL_DIR" ] && [ -f "$INSTALL_DIR/svc.sh" ]; then
  echo "Stopping existing runner service..."
  cd "$INSTALL_DIR"
  sudo ./svc.sh stop 2>/dev/null || true
  sudo ./svc.sh uninstall 2>/dev/null || true
  cd /
fi

# ディレクトリ作成
sudo mkdir -p "$INSTALL_DIR"
sudo chown "$(whoami):$(whoami)" "$INSTALL_DIR"
cd "$INSTALL_DIR"

# 既存のバイナリがなければダウンロード
if [ ! -f "config.sh" ]; then
  echo "Downloading runner binary (v${RUNNER_VERSION})..."
  curl -o "actions-runner-linux-arm64-${RUNNER_VERSION}.tar.gz" -L \
    "https://github.com/actions/runner/releases/download/v${RUNNER_VERSION}/actions-runner-linux-arm64-${RUNNER_VERSION}.tar.gz"
  echo "Extracting..."
  tar xzf "actions-runner-linux-arm64-${RUNNER_VERSION}.tar.gz"
  rm "actions-runner-linux-arm64-${RUNNER_VERSION}.tar.gz"
else
  echo "Runner binary already exists, skipping download."
fi

# 既存の設定があればクリーンアップ
if [ -f ".runner" ]; then
  echo "Removing old configuration..."
  ./config.sh remove --token "$(cat .runner 2>/dev/null | grep -o '"token":"[^"]*"' | head -1 | cut -d'"' -f4)" 2>/dev/null || true
  rm -f .runner .credentials .credentials_rsaparams
fi

# runner設定（非対話モード）
echo "Configuring runner..."
./config.sh --url "$REPO_URL" --token "$RUNNER_TOKEN" --name "c3" --labels "c3,self-hosted,linux,arm64" --unattended --replace

# サービスとしてインストール・起動
echo "Installing and starting runner service..."
sudo ./svc.sh install
sudo ./svc.sh start

echo ""
echo "=== Setup Complete ==="
echo "Runner status:"
sudo ./svc.sh status || true
echo ""
echo "Runner is now registered as 'c3' and running as a service."
echo "To check logs: sudo journalctl -u actions.runner.*"