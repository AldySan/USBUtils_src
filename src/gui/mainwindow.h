// SPDX-License-Identifier: MIT
/*
 * USBUtils GUI — Qt Widgets reimplementation of USBUtil v2.2's main form.
 * The layout mirrors the original TForm1 (see docs/dfm/TFORM1.dfm.txt):
 * drive selector, game list, action buttons, progress bar, status bar —
 * plus a menu bar, a welcome state and an automatic update check so the
 * app stays friendly for non-technical users.
 */
#pragma once

#include <QMainWindow>
#include <QString>
#include <QList>
#include <QPointer>
#include <functional>
#include <memory>

#include "../core/ulcfg.h"
#include "../core/gamefile.h"

class QComboBox;
class QListWidget;
class QProgressBar;
class QLabel;
class QPushButton;
class QWidget;
class QThread;
class QGroupBox;

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
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent* ev) override;

private slots:
    void refreshDrives();
    void openDrive();
    void refreshGames();
    void openDriveFolder();
    void installIso();
    void installFromDisc();        ///< original "Create GAME from CD/DVD"
    void openIsoExplorer();        ///< original "Open ISO to [Explore/Modify]"
    void closeUlCfg();             ///< original "Close ul.cfg"
    void viewConfig();             ///< original "View current config"
    void exportSelected();
    void removeSelected();
    void renameSelected();
    void copySelected();            ///< copy to another drive
    void moveSelected();            ///< move to another drive
    void checkStructure();          ///< structure check + broken serials
    void fixBrokenSerials();        ///< "Correct code 666.66"
    void recoverGames();
    void sortGamesList();           ///< original "Sorting" button
    void cleanDriveSpace();         ///< original "Restore space" (Rest. Spc.)
    void findFile();
    void gameFilesSelected();
    void driveReport();
    void videoRipSelected();
    void dnasPatchSelected();
    void ps2LoadSelected();
    void selectAllGames();          ///< original "Select" (mark all games)
    void invertSelection();         ///< original "Inverse selection"
    void checkForUpdates();         ///< manual check with full feedback
    void autoUpdateCheck();         ///< silent check at startup
    void showQuickHelp();
    void about();

private:
    void setUiBusy(bool busy);
    QString currentDir() const;
    int selectedRow() const;
    QList<int> selectedRows() const;   ///< all marked games (Select system)
    void copyMoveCommon(bool move);
    void buildMenus();
    void buildSupportPanel();          ///< "Useful acquired" status strip
    void updateSupportPanel();
    /// Run `job` on a worker thread behind a cancelable modal progress
    /// dialog; `onDone(outcome)` runs on the GUI thread afterwards.
    void runJob(const QString& title, uint64_t expectedTotalBytes,
                const std::function<JobOutcome(const usbutils::ProgressFn&,
                                               const std::function<bool()>&)>& job,
                const std::function<void(const JobOutcome&)>& onDone);
    void handleUpdateResult(bool manual);

    QComboBox*    driveCombo_ = nullptr;
    QListWidget*  gameList_   = nullptr;
    QProgressBar* progress_   = nullptr;
    QLabel*       status_     = nullptr;
    QLabel*       welcome_    = nullptr;     ///< shown while no drive is open
    QPushButton*  btnOpen_    = nullptr;
    QPushButton*  btnSelectAll_  = nullptr;  ///< "Select" (mark all games)
    QPushButton*  btnInvert_     = nullptr;  ///< "Inverse selection"
    QGroupBox*    supportPanel_  = nullptr;  ///< "Useful acquired" strip
    QWidget*      actionsPanel_ = nullptr;   ///< disabled while busy

    QString dir_;
    std::unique_ptr<usbutils::UlCfg> cfg_;
    int busyJobs_ = 0;
    bool updatePending_ = false;             ///< an update dialog is showing
    QPointer<QThread> updateThread_;          ///< in-flight startup update check (self-deleting; QPointer nulls on deletion)
    QString lastUpdateCheck_;                ///< for View current config
};
