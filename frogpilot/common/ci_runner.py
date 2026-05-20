#!/usr/bin/env python3
"""CI Runner management module for FrogPilot.

Detects and manages the GitHub Actions self-hosted runner on the device.
When the CI runner is active, the openpilot updater is disabled to prevent
build artifacts from being destroyed by git reset --hard operations.
"""
import json
import os
import subprocess
import time
from pathlib import Path

from openpilot.common.params import Params
from openpilot.common.swaglog import cloudlog

ACTIONS_RUNNER_DIR = Path("/data/actions-runner")
RUNNER_SERVICE_NAME = "actions.runner."
CIRUNNER_STATUS_FILE = Path("/data/params/d/CIRunnerStatus")
CIRUNNER_LOCK_FILE = Path("/tmp/ci_runner_check.lock")


def is_runner_installed() -> bool:
  """Check if GitHub Actions runner is installed on this device."""
  return ACTIONS_RUNNER_DIR.is_dir() and (ACTIONS_RUNNER_DIR / "config.sh").is_file()


def is_runner_running() -> bool:
  """Check if the GitHub Actions runner process is currently active."""
  try:
    result = subprocess.run(
      ["systemctl", "list-units", "--type=service", "--state=running", "--no-legend"],
      capture_output=True, text=True, timeout=5
    )
    for line in result.stdout.strip().split('\n'):
      if RUNNER_SERVICE_NAME in line and "running" in line:
        return True
    return False
  except (subprocess.TimeoutExpired, subprocess.CalledProcessError, FileNotFoundError):
    pass

  # Fallback: check for Runner.Listener or Runner.Worker process
  try:
    result = subprocess.run(
      ["pgrep", "-f", "Runner.Listener"],
      capture_output=True, text=True, timeout=5
    )
    return result.returncode == 0
  except (subprocess.TimeoutExpired, subprocess.CalledProcessError, FileNotFoundError):
    return False


def get_runner_status() -> dict:
  """Get comprehensive CI runner status information."""
  status = {
    "installed": is_runner_installed(),
    "running": False,
    "service_name": "",
    "last_job": "",
    "last_check": time.strftime("%Y-%m-%d %H:%M:%S"),
  }

  if not status["installed"]:
    return status

  status["running"] = is_runner_running()

  # Get service name
  try:
    result = subprocess.run(
      ["systemctl", "list-units", "--type=service", "--no-legend"],
      capture_output=True, text=True, timeout=5
    )
    for line in result.stdout.strip().split('\n'):
      if RUNNER_SERVICE_NAME in line:
        parts = line.split()
        if parts:
          status["service_name"] = parts[0].replace(".service", "")
  except (subprocess.TimeoutExpired, subprocess.CalledProcessError, FileNotFoundError):
    pass

  # Get last job info from _diag logs
  diag_dir = ACTIONS_RUNNER_DIR / "_diag"
  if diag_dir.is_dir():
    try:
      log_files = sorted(diag_dir.glob("Runner_*.log"), key=lambda f: f.stat().st_mtime, reverse=True)
      if log_files:
        # Read last few lines for recent activity
        with open(log_files[0], 'r', errors='ignore') as f:
          lines = f.readlines()
          for line in reversed(lines[-50:]):
            if "Running job" in line or "Job" in line:
              status["last_job"] = line.strip()[-200:]
              break
    except (OSError, IndexError):
      pass

  return status


def save_runner_status(params: Params = None) -> dict:
  """Save runner status to params for UI access."""
  if params is None:
    params = Params()

  status = get_runner_status()
  params.put("CIRunnerStatus", json.dumps(status))
  params.put_bool("CIRunnerActive", status["running"])
  return status


def start_runner() -> bool:
  """Start the GitHub Actions runner service."""
  status = get_runner_status()
  if not status["installed"]:
    cloudlog.warning("Cannot start runner: not installed")
    return False

  service_name = status.get("service_name", "")
  if not service_name:
    # Try to find the service
    try:
      result = subprocess.run(
        ["systemctl", "list-unit-files", "--type=service", "--no-legend"],
        capture_output=True, text=True, timeout=5
      )
      for line in result.stdout.strip().split('\n'):
        if RUNNER_SERVICE_NAME in line:
          service_name = line.split()[0].replace(".service", "")
          break
    except (subprocess.TimeoutExpired, subprocess.CalledProcessError, FileNotFoundError):
      pass

  if not service_name:
    cloudlog.warning("Cannot start runner: service name not found")
    return False

  try:
    result = subprocess.run(
      ["sudo", "systemctl", "start", service_name],
      capture_output=True, text=True, timeout=30
    )
    if result.returncode == 0:
      cloudlog.info(f"CI Runner started: {service_name}")
      return True
    else:
      cloudlog.warning(f"Failed to start runner: {result.stderr}")
      return False
  except (subprocess.TimeoutExpired, subprocess.CalledProcessError) as e:
    cloudlog.warning(f"Failed to start runner: {e}")
    return False


def stop_runner() -> bool:
  """Stop the GitHub Actions runner service."""
  status = get_runner_status()
  service_name = status.get("service_name", "")
  if not service_name:
    return False

  try:
    result = subprocess.run(
      ["sudo", "systemctl", "stop", service_name],
      capture_output=True, text=True, timeout=30
    )
    if result.returncode == 0:
      cloudlog.info(f"CI Runner stopped: {service_name}")
      return True
    else:
      cloudlog.warning(f"Failed to stop runner: {result.stderr}")
      return False
  except (subprocess.TimeoutExpired, subprocess.CalledProcessError) as e:
    cloudlog.warning(f"Failed to stop runner: {e}")
    return False


def restart_runner() -> bool:
  """Restart the GitHub Actions runner service."""
  stop_runner()
  time.sleep(2)
  return start_runner()


def should_disable_updater() -> bool:
  """Check if the updater should be disabled because CI runner is active."""
  if not is_runner_installed():
    return False
  return is_runner_running()


if __name__ == "__main__":
  status = get_runner_status()
  print(json.dumps(status, indent=2))
  print(f"Updater should be disabled: {should_disable_updater()}")