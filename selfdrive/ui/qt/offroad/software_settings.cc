#include "selfdrive/ui/qt/offroad/settings.h"

#include <cassert>
#include <cmath>
#include <string>

#include <QDebug>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QTimer>

#include "common/params.h"
#include "common/util.h"
#include "selfdrive/ui/ui.h"
#include "selfdrive/ui/qt/util.h"
#include "selfdrive/ui/qt/widgets/controls.h"
#include "selfdrive/ui/qt/widgets/input.h"
#include "system/hardware/hw.h"


void SoftwarePanel::checkForUpdates() {
  std::system("pkill -SIGUSR1 -f system.updated.updated");
}

SoftwarePanel::SoftwarePanel(QWidget* parent) : ListWidget(parent) {
  onroadLbl = new QLabel(tr("Updates are only downloaded while the car is off or in park."));
  onroadLbl->setStyleSheet("font-size: 50px; font-weight: 400; text-align: left; padding-top: 30px; padding-bottom: 30px;");
  addItem(onroadLbl);

  // current version
  versionLbl = new LabelControl(tr("Current Version"), "");
  addItem(versionLbl);

  // automatic updates toggle
  ParamControl *automaticUpdatesToggle = new ParamControl("AutomaticUpdates", tr("Automatically Update FrogPilot"),
                                                       tr("FrogPilot will automatically update itself and it's assets when you're offroad and have an active internet connection."), "");
  automaticUpdatesToggle->setVisible(params.getBool("IsReleaseBranch"));
  addItem(automaticUpdatesToggle);

  // download update btn
  downloadBtn = new ButtonControl(tr("Download"), tr("CHECK"));
  connect(downloadBtn, &ButtonControl::clicked, [=]() {
    downloadBtn->setEnabled(false);
    if (downloadBtn->text() == tr("CHECK")) {
      checkForUpdates();
    } else {
      std::system("pkill -SIGHUP -f system.updated.updated");
    }
    frogpilotUIState()->params_memory.putBool("ManualUpdateInitiated", true);
  });
  addItem(downloadBtn);

  // install update btn
  installBtn = new ButtonControl(tr("Install Update"), tr("INSTALL"));
  connect(installBtn, &ButtonControl::clicked, [=]() {
    installBtn->setEnabled(false);
    params.putBool("DoReboot", true);
  });
  addItem(installBtn);

  // branch selecting
  targetBranchBtn = new ButtonControl(tr("Target Branch"), tr("SELECT"));
  connect(targetBranchBtn, &ButtonControl::clicked, [=]() {
    auto current = params.get("GitBranch");
    QStringList branches = QString::fromStdString(params.get("UpdaterAvailableBranches")).split(",");
    if (!frogpilotUIState()->frogpilot_scene.frogpilot_toggles.value("frogs_go_moo").toBool()) {
      for (int i = branches.size() - 1; i >= 0; --i) {
        if (branches[i].startsWith("FrogPilot-Development", Qt::CaseInsensitive)) {
          branches.removeAt(i);
        }
      }
      branches.removeAll("FrogPilot-Vetting");
      branches.removeAll("MAKE-PRS-HERE");
    }
    for (QString b : {current.c_str(), "devel-staging", "devel", "nightly", "master-ci", "master"}) {
      auto i = branches.indexOf(b);
      if (i >= 0) {
        branches.removeAt(i);
        branches.insert(0, b);
      }
    }

    QString cur = QString::fromStdString(params.get("UpdaterTargetBranch"));
    QString selection = MultiOptionDialog::getSelection(tr("Select a branch"), branches, cur, this);
    if (!selection.isEmpty()) {
      params.put("UpdaterTargetBranch", selection.toStdString());
      targetBranchBtn->setValue(QString::fromStdString(params.get("UpdaterTargetBranch")));
      checkForUpdates();

      if (selection.toStdString() != current) {
        if (FrogPilotConfirmationDialog::yesorno(tr("This branch must be downloaded before switching. Would you like to download it now?"), this)) {
          std::system("pkill -SIGHUP -f system.updated.updated");

          frogpilotUIState()->params_memory.putBool("ManualUpdateInitiated", true);
        }
      }
    }
  });
  addItem(targetBranchBtn);

  // uninstall button
  auto uninstallBtn = new ButtonControl(tr("Uninstall %1").arg(getBrand()), tr("UNINSTALL"));
  connect(uninstallBtn, &ButtonControl::clicked, [&]() {
    if (ConfirmationDialog::confirm(tr("Are you sure you want to uninstall?"), tr("Uninstall"), this)) {
      if (FrogPilotConfirmationDialog::yesorno(tr("Do you want to perform a full factory reset? All saved assets and settings will be permanently deleted!"), this)) {
        if (FrogPilotConfirmationDialog::yesorno(tr("This is a complete factory reset and cannot be undone. Are you absolutely sure you want to continue?"), this)) {
          std::system("rm -rf /cache/params/d");
        }
      }
      params.putBool("DoUninstall", true);
    }
  });
  addItem(uninstallBtn);

  // error log button
  auto errorLogBtn = new ButtonControl(tr("Error Log"), tr("VIEW"), tr("View the error log for openpilot crashes."));
  connect(errorLogBtn, &ButtonControl::clicked, [=]() {
    std::string txt = util::read_file("/data/error_logs/error.txt");
    ConfirmationDialog::rich(QString::fromStdString(txt), this);
  });
  addItem(errorLogBtn);

  // CI Runner section
  ciRunnerStatusLbl = new LabelControl(tr("CI Runner Status"), tr("Checking..."));
  addItem(ciRunnerStatusLbl);

  ciRunnerInfoLbl = new LabelControl(tr("Updater Status"), "");
  addItem(ciRunnerInfoLbl);

  ciRunnerStartBtn = new ButtonControl(tr("CI Runner"), tr("START"), tr("Start, stop, or restart the GitHub Actions CI runner on this device."));
  connect(ciRunnerStartBtn, &ButtonControl::clicked, [=]() {
    ciRunnerStartBtn->setEnabled(false);
    std::system("sudo python3 /data/openpilot/frogpilot/common/ci_runner.py start &");
    QTimer::singleShot(5000, [=]() {
      updateCIRunnerStatus();
      ciRunnerStartBtn->setEnabled(true);
    });
  });
  addItem(ciRunnerStartBtn);

  ciRunnerStopBtn = new ButtonControl(tr("Stop CI Runner"), tr("STOP"));
  connect(ciRunnerStopBtn, &ButtonControl::clicked, [=]() {
    ciRunnerStopBtn->setEnabled(false);
    std::system("sudo python3 /data/openpilot/frogpilot/common/ci_runner.py stop &");
    QTimer::singleShot(5000, [=]() {
      updateCIRunnerStatus();
      ciRunnerStopBtn->setEnabled(true);
    });
  });
  addItem(ciRunnerStopBtn);

  ciRunnerRestartBtn = new ButtonControl(tr("Restart CI Runner"), tr("RESTART"));
  connect(ciRunnerRestartBtn, &ButtonControl::clicked, [=]() {
    ciRunnerRestartBtn->setEnabled(false);
    std::system("sudo python3 /data/openpilot/frogpilot/common/ci_runner.py restart &");
    QTimer::singleShot(8000, [=]() {
      updateCIRunnerStatus();
      ciRunnerRestartBtn->setEnabled(true);
    });
  });
  addItem(ciRunnerRestartBtn);

  fs_watch = new ParamWatcher(this);
  QObject::connect(fs_watch, &ParamWatcher::paramChanged, [=](const QString &param_name, const QString &param_value) {
    updateLabels();
  });

  connect(uiState(), &UIState::offroadTransition, [=](bool offroad) {
    is_onroad = !offroad;
    updateLabels();
  });

  updateLabels();
}

void SoftwarePanel::showEvent(QShowEvent *event) {
  // nice for testing on PC
  installBtn->setEnabled(true);

  updateLabels();
  updateCIRunnerStatus();

  // FrogPilot variables
  FrogPilotUIState &fs = *frogpilotUIState();
  FrogPilotUIScene &frogpilot_scene = fs.frogpilot_scene;

  if (frogpilot_scene.online && params.get("UpdaterState") == "idle") {
    checkForUpdates();
  }
}

void SoftwarePanel::updateLabels() {
  FrogPilotUIState &fs = *frogpilotUIState();
  FrogPilotUIScene &frogpilot_scene = fs.frogpilot_scene;

  // add these back in case the files got removed
  fs_watch->addParam("LastUpdateTime");
  fs_watch->addParam("UpdateFailedCount");
  fs_watch->addParam("UpdaterState");
  fs_watch->addParam("UpdateAvailable");
  fs_watch->addParam("CIRunnerStatus");
  fs_watch->addParam("CIRunnerActive");
  fs_watch->addParam("CIRunnerBlockingUpdate");

  if (!isVisible()) {
    frogpilot_scene.downloading_update = false;
    return;
  }

  // updater only runs offroad or when parked
  bool parked = frogpilot_scene.parked || frogpilot_scene.frogpilot_toggles.value("frogs_go_moo").toBool();

  onroadLbl->setVisible(is_onroad && !parked);
  downloadBtn->setVisible(!is_onroad || parked);

  // download update
  QString updater_state = QString::fromStdString(params.get("UpdaterState"));
  bool failed = std::atoi(params.get("UpdateFailedCount").c_str()) > 0;
  if (updater_state != "idle") {
    downloadBtn->setEnabled(false);
    QString stateText = updater_state;
    if (updater_state == "downloading...") {
      stateText = tr("downloading…");
    } else if (updater_state == "checking...") {
      stateText = tr("checking…");
    } else if (updater_state == "waiting for vehicle to go offroad...") {
      stateText = tr("waiting for vehicle to go offroad...");
    } else if (updater_state == "finalizing update...") {
      stateText = tr("finalizing update...");
    }

    downloadBtn->setValue(stateText);
    frogpilot_scene.downloading_update = true;
  } else {
    frogpilot_scene.downloading_update = false;
    if (failed) {
      downloadBtn->setText(tr("CHECK"));
      downloadBtn->setValue(tr("failed to check for update"));
    } else if (params.getBool("UpdaterFetchAvailable")) {
      downloadBtn->setText(tr("DOWNLOAD"));
      downloadBtn->setValue(tr("update available"));
    } else {
      QString lastUpdate = tr("never");
      auto tm = params.get("LastUpdateTime");
      if (!tm.empty()) {
        lastUpdate = timeAgo(QDateTime::fromString(QString::fromStdString(tm + "Z"), Qt::ISODate));
      }
      downloadBtn->setText(tr("CHECK"));
      downloadBtn->setValue(tr("up to date, last checked %1").arg(lastUpdate));
    }
    downloadBtn->setEnabled(true);
  }

  // Update CI Runner status display
  updateCIRunnerStatus();

  targetBranchBtn->setValue(QString::fromStdString(params.get("UpdaterTargetBranch")));

  // current + new versions
  versionLbl->setText(QString::fromStdString(params.get("UpdaterCurrentDescription")));
  versionLbl->setDescription(QString::fromStdString(params.get("UpdaterCurrentReleaseNotes")));

  installBtn->setVisible((!is_onroad || parked) && params.getBool("UpdateAvailable"));
  installBtn->setValue(QString::fromStdString(params.get("UpdaterNewDescription")));
  installBtn->setDescription(QString::fromStdString(params.get("UpdaterNewReleaseNotes")));

  update();
}

void SoftwarePanel::updateCIRunnerStatus() {
  // Read CI Runner status from params (set by the updated process)
  std::string status_json = params.get("CIRunnerStatus");
  bool ci_active = params.getBool("CIRunnerActive");
  bool ci_installed = false;
  bool ci_running = false;
  QString service_name = "";
  QString last_job = "";

  if (!status_json.empty()) {
    QJsonDocument doc = QJsonDocument::fromJson(QString::fromStdString(status_json).toUtf8());
    if (!doc.isNull()) {
      QJsonObject obj = doc.object();
      ci_installed = obj.value("installed").toBool(false);
      ci_running = obj.value("running").toBool(false);
      service_name = obj.value("service_name").toString();
      last_job = obj.value("last_job").toString();
    }
  }

  // Check if /data/actions-runner exists as fallback
  if (!ci_installed) {
    ci_installed = QDir("/data/actions-runner").exists();
  }

  // CI Runner status label
  if (!ci_installed) {
    ciRunnerStatusLbl->setText(tr("Not Installed"));
    ciRunnerStatusLbl->setDescription(tr("No GitHub Actions runner found on this device."));
  } else if (ci_running) {
    ciRunnerStatusLbl->setText(tr("● Running"));
    ciRunnerStatusLbl->setDescription(tr("CI Runner is currently active and processing jobs."));
  } else {
    ciRunnerStatusLbl->setText(tr("○ Stopped"));
    ciRunnerStatusLbl->setDescription(tr("CI Runner is installed but not currently running."));
  }

  // Updater status label - updates disabled whenever CI Runner is installed
  bool ci_blocking = params.get("CIRunnerBlockingUpdate") == "1";
  if (ci_installed) {
    ciRunnerInfoLbl->setText(tr("⚠ Update Disabled"));
    ciRunnerInfoLbl->setDescription(tr("CI Runner is installed on this device.\n\n"
      "Software updates are disabled to prevent build artifacts from being destroyed.\n"
      "Uninstall the CI Runner to re-enable automatic updates."));
  } else {
    ciRunnerInfoLbl->setText(tr("N/A"));
    ciRunnerInfoLbl->setVisible(false);
  }

  // Show/hide CI runner controls
  bool show_ci_controls = ci_installed;
  ciRunnerStatusLbl->setVisible(show_ci_controls);
  ciRunnerInfoLbl->setVisible(show_ci_controls);
  ciRunnerStartBtn->setVisible(show_ci_controls && !ci_running);
  ciRunnerStopBtn->setVisible(show_ci_controls && ci_running);
  ciRunnerRestartBtn->setVisible(show_ci_controls);

  // Disable update buttons when CI runner is installed
  if (ci_installed || ci_blocking) {
    downloadBtn->setEnabled(false);
    downloadBtn->setValue(tr("disabled - CI Runner is installed"));
    installBtn->setVisible(false);
  }

  update();
}

void SoftwarePanel::startCIRunner() {
  std::system("sudo python3 /data/openpilot/frogpilot/common/ci_runner.py start");
  updateCIRunnerStatus();
}

void SoftwarePanel::stopCIRunner() {
  std::system("sudo python3 /data/openpilot/frogpilot/common/ci_runner.py stop");
  updateCIRunnerStatus();
}

void SoftwarePanel::restartCIRunner() {
  std::system("sudo python3 /data/openpilot/frogpilot/common/ci_runner.py restart");
  updateCIRunnerStatus();
}
