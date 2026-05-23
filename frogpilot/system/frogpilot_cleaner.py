#!/usr/bin/env python3
"""
FrogPilot Automatic Disk Cleaner

Protects rlog (driving logs) from deletion by proactively cleaning
non-essential files before the openpilot deleter (system/loggerd/deleter.py)
reaches the critical threshold and starts deleting driving segments.

Priority order (highest first):
  1. /data/tmp/          - Temporary files (always cleaned on startup)
  2. /data/pip_cache/    - pip cache (cleaned when disk is low)
  3. /data/scons_cache/  - SCons build cache (cleaned when disk is low)
  4. /data/backups/      - Old backups (keep only latest 1, when disk is low)

This cleaner triggers at 20% free space, which is higher than deleter.py's
10% threshold, ensuring non-essential files are removed first.

NEVER deletes:
  - /data/params/        - System parameters
  - /data/openpilot/     - OpenPilot installation
  - /data/media/0/realdata/ - rlog driving logs (left to deleter.py as last resort)
"""

import os
import shutil
import time

from openpilot.common.swaglog import cloudlog

# Disk usage thresholds
CLEANUP_PERCENT_THRESHOLD = 20.0  # Start cleanup when free space drops below 20%
CHECK_INTERVAL_SEC = 60           # Check disk usage every 60 seconds

# Paths that must NEVER be deleted
PROTECTED_PATHS = {
  "/data/params/",
  "/data/openpilot/",
  "/data/media/0/realdata/",
  "/data/media/0/realdata_HD/",
  "/data/media/0/realdata_konik/",
}

# Cleanup target definitions (ordered by priority - highest first)
CLEANUP_TARGETS = [
  {
    "path": "/data/tmp/",
    "condition": "always",       # Always clean on startup
    "max_age_days": 0,           # Delete all files
    "description": "Temporary files",
  },
  {
    "path": "/data/pip_cache/",
    "condition": "disk_low",     # Only when disk is low
    "max_age_days": 0,           # Delete all files
    "description": "pip cache",
  },
  {
    "path": "/data/scons_cache/",
    "condition": "disk_low",
    "max_age_days": 7,           # Delete cache older than 7 days
    "description": "SCons build cache",
  },
  {
    "path": "/data/backups/",
    "condition": "disk_low",
    "keep_count": 1,             # Keep only the latest backup
    "description": "Old backups",
  },
]


def get_disk_usage(path="/data/"):
  """Get disk usage statistics for the given path.

  Returns:
    tuple: (total_bytes, used_bytes, available_bytes, available_percent)
  """
  try:
    statvfs = os.statvfs(path)
    total_bytes = statvfs.f_blocks * statvfs.f_frsize
    available_bytes = statvfs.f_bavail * statvfs.f_frsize
    used_bytes = total_bytes - available_bytes
    available_percent = 100.0 * statvfs.f_bavail / statvfs.f_blocks if statvfs.f_blocks > 0 else 100.0
    return total_bytes, used_bytes, available_bytes, available_percent
  except OSError:
    cloudlog.exception("frogpilot_cleaner: failed to get disk usage")
    return 0, 0, 0, 100.0


def format_size(size_bytes):
  """Format bytes into human-readable string."""
  for unit in ("B", "KB", "MB", "GB", "TB"):
    if abs(size_bytes) < 1024.0:
      return f"{size_bytes:.1f}{unit}"
    size_bytes /= 1024.0
  return f"{size_bytes:.1f}PB"


def is_protected(path):
  """Check if a path is in the protected list."""
  abs_path = os.path.abspath(path) + "/"
  for protected in PROTECTED_PATHS:
    if abs_path.startswith(protected) or protected.startswith(abs_path):
      return True
  return False


def get_dir_size(path):
  """Calculate total size of a directory tree."""
  total_size = 0
  try:
    for dirpath, dirnames, filenames in os.walk(path):
      for f in filenames:
        fp = os.path.join(dirpath, f)
        try:
          total_size += os.path.getsize(fp)
        except OSError:
          pass
  except OSError:
    pass
  return total_size


def clean_directory(path, max_age_days=0):
  """Clean files in a directory, optionally filtered by age.

  Args:
    path: Directory path to clean
    max_age_days: Only delete files older than this many days (0 = all)

  Returns:
    tuple: (number_of_files_deleted, bytes_freed)
  """
  if not os.path.exists(path):
    return 0, 0

  if is_protected(path):
    cloudlog.warning(f"frogpilot_cleaner: skipping protected path: {path}")
    return 0, 0

  deleted_count = 0
  freed_bytes = 0
  now = time.time()
  max_age_seconds = max_age_days * 86400

  try:
    for entry in os.listdir(path):
      entry_path = os.path.join(path, entry)

      if is_protected(entry_path):
        continue

      # Check age if max_age_days is set
      if max_age_days > 0:
        try:
          entry_mtime = os.path.getmtime(entry_path)
          if now - entry_mtime < max_age_seconds:
            continue  # Skip files newer than max_age_days
        except OSError:
          continue

      try:
        if os.path.isfile(entry_path) or os.path.islink(entry_path):
          size = os.path.getsize(entry_path)
          os.remove(entry_path)
          freed_bytes += size
          deleted_count += 1
        elif os.path.isdir(entry_path):
          size = get_dir_size(entry_path)
          shutil.rmtree(entry_path)
          freed_bytes += size
          deleted_count += 1
      except OSError:
        cloudlog.exception(f"frogpilot_cleaner: failed to delete {entry_path}")

  except OSError:
    cloudlog.exception(f"frogpilot_cleaner: failed to list {path}")

  return deleted_count, freed_bytes


def clean_old_backups(path, keep_count=1):
  """Remove old backups, keeping only the most recent N.

  Args:
    path: Backups directory path
    keep_count: Number of most recent backups to keep

  Returns:
    tuple: (number_of_backups_deleted, bytes_freed)
  """
  if not os.path.exists(path):
    return 0, 0

  if is_protected(path):
    cloudlog.warning(f"frogpilot_cleaner: skipping protected path: {path}")
    return 0, 0

  try:
    entries = []
    for entry in os.listdir(path):
      entry_path = os.path.join(path, entry)
      if os.path.isdir(entry_path) and not is_protected(entry_path):
        mtime = os.path.getmtime(entry_path)
        entries.append((entry_path, mtime))

    # Sort by modification time, newest first
    entries.sort(key=lambda x: x[1], reverse=True)

    deleted_count = 0
    freed_bytes = 0

    # Delete all but the most recent keep_count backups
    for entry_path, _ in entries[keep_count:]:
      try:
        size = get_dir_size(entry_path)
        shutil.rmtree(entry_path)
        freed_bytes += size
        deleted_count += 1
        cloudlog.info(f"frogpilot_cleaner: deleted old backup: {entry_path} ({format_size(size)})")
      except OSError:
        cloudlog.exception(f"frogpilot_cleaner: failed to delete backup {entry_path}")

    return deleted_count, freed_bytes

  except OSError:
    cloudlog.exception(f"frogpilot_cleaner: failed to clean backups in {path}")
    return 0, 0


def run_cleanup(disk_low=False):
  """Execute cleanup based on current conditions.

  Args:
    disk_low: If True, also clean caches and backups (not just temp files)

  Returns:
    tuple: (total_files_deleted, total_bytes_freed)
  """
  total_deleted = 0
  total_freed = 0

  for target in CLEANUP_TARGETS:
    path = target["path"]
    condition = target["condition"]

    # Skip disk_low targets if disk is not low
    if condition == "disk_low" and not disk_low:
      continue

    # Log disk usage before cleanup
    _, _, available_bytes, available_percent = get_disk_usage()
    cloudlog.info(
      f"frogpilot_cleaner: before cleaning {target['description']} "
      f"({path}): {format_size(available_bytes)} free ({available_percent:.1f}%)"
    )

    if "keep_count" in target:
      # Handle backup-style cleanup (keep N most recent)
      deleted, freed = clean_old_backups(path, keep_count=target["keep_count"])
    else:
      # Handle regular directory cleanup
      max_age = target.get("max_age_days", 0)
      deleted, freed = clean_directory(path, max_age_days=max_age)

    if deleted > 0 or freed > 0:
      cloudlog.info(
        f"frogpilot_cleaner: cleaned {target['description']} ({path}): "
        f"deleted {deleted} items, freed {format_size(freed)}"
      )

    total_deleted += deleted
    total_freed += freed

  return total_deleted, total_freed


def log_disk_status():
  """Log current disk usage status."""
  total, used, available, percent = get_disk_usage()
  cloudlog.info(
    f"frogpilot_cleaner: disk status - "
    f"total: {format_size(total)}, "
    f"used: {format_size(used)}, "
    f"available: {format_size(available)} ({percent:.1f}%)"
  )


def cleaner_thread():
  """Main cleaner thread that runs continuously.

  - On startup: always cleans temporary files
  - Every 60 seconds: checks disk usage
  - When free space < 20%: cleans caches and old backups
  """
  cloudlog.info("frogpilot_cleaner: starting up")

  # Log initial disk status
  log_disk_status()

  # Startup cleanup: always clean temp files
  cloudlog.info("frogpilot_cleaner: running startup cleanup (temp files)")
  startup_deleted, startup_freed = run_cleanup(disk_low=False)
  if startup_freed > 0:
    cloudlog.info(
      f"frogpilot_cleaner: startup cleanup freed {format_size(startup_freed)} "
      f"({startup_deleted} items)"
    )
    log_disk_status()

  cloudlog.info("frogpilot_cleaner: startup complete, entering monitoring loop")

  # Main monitoring loop
  while True:
    try:
      _, _, available_bytes, available_percent = get_disk_usage()

      if available_percent < CLEANUP_PERCENT_THRESHOLD:
        cloudlog.warning(
          f"frogpilot_cleaner: disk space low! "
          f"{format_size(available_bytes)} free ({available_percent:.1f}%)"
        )
        log_disk_status()

        deleted, freed = run_cleanup(disk_low=True)

        if freed > 0:
          cloudlog.info(
            f"frogpilot_cleaner: cleanup freed {format_size(freed)} ({deleted} items)"
          )
          # Log disk status after cleanup
          _, _, new_available, new_percent = get_disk_usage()
          cloudlog.info(
            f"frogpilot_cleaner: after cleanup: "
            f"{format_size(new_available)} free ({new_percent:.1f}%)"
          )
        else:
          cloudlog.warning(
            "frogpilot_cleaner: no non-essential files to clean. "
            "deleter.py may need to delete driving logs."
          )

    except Exception:
      cloudlog.exception("frogpilot_cleaner: error in monitoring loop")

    time.sleep(CHECK_INTERVAL_SEC)


def main():
  cleaner_thread()


if __name__ == "__main__":
  main()