// SPDX-License-Identifier: MIT
/*
 * USBUtils GUI — Qt Widgets reimplementation of USBUtil v2.2's main form.
 * The layout mirrors the original TForm1 (see docs/dfm/TFORM1.dfm.txt):
 * drive selector, game list, action buttons, progress bar, status bar.
 */
#pragma once

#include <QMainWindow>
#include <QString>
#include <memory>

#include "../core/ulcfg.h"
#include "../core/gamefile.h"

class QComboBox;
class QListWidget;
class QProgressBar;
class QLabel;
class QPushButton;
class QWidget;

namespace usbutils { struct OrphanGame; }

/// Result of a worker-thread job: ok + a human message (error detail or
/// success summary). "cancelled" marks user-cancellation.
struct JobOutcome {
    bool    ok = false;
    QString message;
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);

protected:
    void closeEvent(QCloseEvent* ev) override;

private slots:
    void refreshDrives();
    void openDrive();
    void refreshGames();
    void installIso();
    void exportSelected();
    void removeSelected();
    void renameSelected();
    void checkStructure();
    void recoverGames();
    void videoRipSelected();
    void dnasPatchSelected();
    void ps2LoadSelected();
    void about();

private:
    void setUiBusy(bool busy);
    QString currentDir() const;
    int selectedRow() const;
    /// Run `job` on a worker thread behind a cancelable modal progress
    /// dialog; `onDone(outcome)` runs on the GUI thread afterwards.
    void runJob(const QString& title, uint64_t expectedTotalBytes,
                const std::function<JobOutcome(const usbutils::ProgressFn&,
                                               const std::function<bool()>&)>& job,
                const std::function<void(const JobOutcome&)>& onDone);

    QComboBox*    driveCombo_ = nullptr;
    QListWidget*  gameList_   = nullptr;
    QProgressBar* progress_   = nullptr;
    QLabel*       status_     = nullptr;
    QPushButton*  btnOpen_    = nullptr;
    QWidget*      actionsPanel_ = nullptr;   ///< disabled while busy

    QString dir_;
    std::unique_ptr<usbutils::UlCfg> cfg_;
    int busyJobs_ = 0;
};
