#!/bin/bash
# Fix scons build environment on AGNOS device
# This script diagnoses and fixes two issues:
# 1. cereal/services.py: dict[str, tuple] requires Python 3.9+
# 2. body/crypto/sign.py: Missing pycryptodome module

set -e

echo "========================================="
echo "Step 1: Checking Python versions"
echo "========================================="

echo "--- System Python ---"
/usr/bin/python3 --version 2>&1 || echo "System python3 not found"

echo "--- pyenv Python ---"
/usr/local/pyenv/versions/3.11.4/bin/python3 --version 2>&1 || echo "pyenv python3.11.4 not found"

echo "--- venv Python ---"
/data/openpilot/.venv/bin/python3 --version 2>&1 || echo "venv python3 not found"

echo "--- scons shebang ---"
head -1 /data/openpilot/.venv/bin/scons 2>&1 || echo "scons not found in venv"

echo ""
echo "========================================="
echo "Step 2: Checking if pycryptodome is installed"
echo "========================================="

/data/openpilot/.venv/bin/pip list 2>&1 | grep -i crypto || echo "pycryptodome NOT found in venv"

echo ""
echo "========================================="
echo "Step 3: Finding poetry binary"
echo "========================================="

PYENV_ROOT="/usr/local/pyenv"
POETRY_BIN=$(find "$PYENV_ROOT/versions" -name "poetry" -type f 2>/dev/null | head -1)

if [ -z "$POETRY_BIN" ]; then
  echo "ERROR: poetry not found in $PYENV_ROOT/versions"
  echo "Trying alternative locations..."
  POETRY_BIN=$(which poetry 2>/dev/null || true)
  if [ -z "$POETRY_BIN" ]; then
    echo "ERROR: poetry not found anywhere. Installing pycryptodome directly via pip..."
    /data/openpilot/.venv/bin/pip install pycryptodome
  fi
fi

if [ -n "$POETRY_BIN" ]; then
  echo "Found poetry: $POETRY_BIN"
  echo ""
  echo "========================================="
  echo "Step 4: Running poetry install --no-root"
  echo "========================================="
  
  cd /data/openpilot
  
  export POETRY_CACHE_DIR=/data/poetry_cache
  export POETRY_CONFIG_DIR=/data/poetry_config
  export POETRY_VIRTUALENVS_IN_PROJECT=true
  export PIP_CACHE_DIR=/data/pip_cache
  export TMPDIR=/data/tmp
  export VIRTUALENV_OVERRIDE_APP_DATA=/data/virtualenv_app_data
  export XDG_CACHE_HOME=/data/xdg_cache
  
  "$POETRY_BIN" install --no-root
fi

echo ""
echo "========================================="
echo "Step 5: Verifying pycryptodome installation"
echo "========================================="

/data/openpilot/.venv/bin/pip list 2>&1 | grep -i crypto || echo "WARNING: pycryptodome still not found!"

echo ""
echo "========================================="
echo "Step 6: Verifying Python version in venv"
echo "========================================="

VENV_PYTHON_VERSION=$(/data/openpilot/.venv/bin/python3 -c 'import sys; print(f"{sys.version_info.major}.{sys.version_info.minor}.{sys.version_info.micro}")')
echo "venv Python version: $VENV_PYTHON_VERSION"

# Check if dict[str, tuple] syntax works
/data/openpilot/.venv/bin/python3 -c "
_services: dict[str, tuple] = {'test': (True, 1.0)}
print('dict[str, tuple] syntax: OK')
" 2>&1 || echo "ERROR: dict[str, tuple] syntax FAILED - Python version too old!"

echo ""
echo "========================================="
echo "Step 7: Running scons build"
echo "========================================="

cd /data/openpilot

# Use poetry run scons if poetry is available, otherwise use venv scons directly
if [ -n "$POETRY_BIN" ]; then
  "$POETRY_BIN" run scons -j$(nproc) 2>&1 | tail -50
else
  /data/openpilot/.venv/bin/scons -j$(nproc) 2>&1 | tail -50
fi

echo ""
echo "========================================="
echo "Build fix script completed!"
echo "========================================="