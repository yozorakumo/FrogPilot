#!/bin/bash
RUNNER_VERSION="2.315.0"
INSTALL_DIR="/data/actions-runner"
BACKUP_DIR="/tmp"

echo "=== Creating install directory ==="
sudo mkdir -p "$INSTALL_DIR"
sudo chown "$(whoami):$(whoami)" "$INSTALL_DIR"
cd "$INSTALL_DIR" || exit 1

echo "=== Downloading runner binary ==="
if [ ! -f "config.sh" ]; then
  echo "Downloading actions-runner v${RUNNER_VERSION}..."
  curl -fL -o "actions-runner-linux-arm64-${RUNNER_VERSION}.tar.gz" \
    "https://github.com/actions/runner/releases/download/v${RUNNER_VERSION}/actions-runner-linux-arm64-${RUNNER_VERSION}.tar.gz"
  if [ $? -ne 0 ]; then
    echo "ERROR: Download failed"
    exit 1
  fi
  tar xzf "./actions-runner-linux-arm64-${RUNNER_VERSION}.tar.gz"
  if [ $? -ne 0 ]; then
    echo "ERROR: Extraction failed"
    exit 1
  fi
  echo "Download and extraction complete"
else
  echo "Runner already downloaded"
fi

echo "=== Restoring config files ==="
cp "${BACKUP_DIR}/.runner" .
cp "${BACKUP_DIR}/.credentials" .
cp "${BACKUP_DIR}/.credentials_rsaparams" .
chmod 600 ".credentials" ".credentials_rsaparams"
echo "Config files restored"

echo "=== Installing runner service ==="
sudo ./svc.sh install
sudo ./svc.sh start
echo "=== Done ==="
sudo ./svc.sh status