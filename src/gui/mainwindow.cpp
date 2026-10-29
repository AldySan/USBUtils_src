// SPDX-License-Identifier: MIT
#include "mainwindow.h"

#include "videoripdialog.h"
#include "dnasdialog.h"
#include "ps2loaddialog.h"
#include "finddialog.h"
#include "gamefilesdialog.h"

#include "../core/ulcfg.h"
#include "../core/installer.h"
#include "../core/gamefile.h"
#include "../core/ulid.h"
#include "../core/recovery.h"
#include "../core/usbdevice.h"
#include "../core/videorip.h"
#include "../core/gameops.h"
#include "../core/update.h"
#include "../core/port.h"
#include "workerthread.h"

#include <QComboBox>
#include <QListWidget>
#include <QProgressBar>
#include <QLabel>
#include <QPushButton>
#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QMenuBar>
#include <QMenu>
#include <QAction>
#include <QFileDialog>
#include <QMessageBox>
#include <QInputDialog>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QApplication>
#include <QThread>
#include <QTimer>
#include <QDateTime>
#include <QLocale>
#include <QDir>
#include <QProgressDialog>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QUrl>
#include <QClipboard>
#include <QGuiApplication>
#include <QFile>
#include <QGroupBox>
#include <QToolButton>
#include <QIODevice>

#include "isoexplorerdialog.h"
#include "configdialog.h"
#include "../core/supportfiles.h"
#include "../core/usbdevice.h"
#include "../core/installer.h"

#include <atomic>
#include <algorithm>
#include <functional>
#include <memory>

using namespace usbutils;

static QString humanSize(uint64_t b) {
    if (b >= 1024ull * 1024 * 1024)
        return QString::number(double(b) / (1024.0 * 1024 * 1024), 'f', 2) + " GiB";
    if (b >= 1024 * 1024)
        return QString::number(double(b) / (1024.0 * 1024), 'f', 2) + " MiB";
    return QString::number(double(b) / 1024.0, 'f', 2) + " KiB";
}

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(tr("USBUtils %1 — PS2 USBExtreme manager")
                       .arg(QString::fromLatin1(USBUTILS_VERSION)));
    resize(860, 560);

    auto* central = new QWidget(this);
    auto* root    = new QVBoxLayout(central);

    // ---- drive row -------------------------------------------------------
    auto* driveRow = new QHBoxLayout;
    driveCombo_ = new QComboBox(this);
    driveCombo_->setMinimumWidth(360);
    driveCombo_->setToolTip(tr("Your USB drives. Pick the one that holds "
                               "your PS2 games."));
    auto* btnRefresh = new QPushButton(tr("Refresh drives"), this);
    btnRefresh->setToolTip(tr("Look again for plugged-in USB drives."));
    btnOpen_ = new QPushButton(tr("Open games list"), this);
    btnOpen_->setToolTip(tr("Show the games stored on the selected drive."));
    auto* btnFolder = new QPushButton(tr("Open folder"), this);
    btnFolder->setToolTip(tr("Open the selected drive in your file manager."));
    connect(btnRefresh, &QPushButton::clicked, this, &MainWindow::refreshDrives);
    connect(btnOpen_, &QPushButton::clicked, this, &MainWindow::openDrive);
    connect(btnFolder, &QPushButton::clicked, this, &MainWindow::openDriveFolder);
    driveRow->addWidget(new QLabel(tr("USB drive:"), this));
    driveRow->addWidget(driveCombo_, 1);
    driveRow->addWidget(btnRefresh);
    driveRow->addWidget(btnOpen_);
    driveRow->addWidget(btnFolder);
    root->addLayout(driveRow);

    // ---- welcome state (before a drive is opened) ------------------------
    welcome_ = new QLabel(tr(
        "<div style='color:#555; font-size:14pt;'>"
        "<b>Welcome to USBUtils!</b></div>"
        "<div style='color:#555;'><br/>"
        "This tool puts PlayStation 2 games on a USB drive so the console "
        "can play them without discs.<br/><br/>"
        "1. Plug in your USB drive (it should be FAT32)<br/>"
        "2. Pick the drive at the top<br/>"
        "3. Click <b>Open games list</b> — then install your first game"
        "</div>"), this);
    welcome_->setAlignment(Qt::AlignTop | Qt::AlignHCenter);
    welcome_->setContentsMargins(24, 24, 24, 24);
    root->addWidget(welcome_, 1);

    // ---- game list --------------------------------------------------------
    gameList_ = new QListWidget(this);
    gameList_->setAlternatingRowColors(true);
    gameList_->setSelectionMode(QAbstractItemView::ExtendedSelection);  // Select system
    gameList_->setContextMenuPolicy(Qt::CustomContextMenu);
    gameList_->setToolTip(tr("The games on this drive. Right-click a game "
                             "for more actions; hold Ctrl/Shift to mark "
                             "several."));
    gameList_->hide();
    connect(gameList_, &QListWidget::customContextMenuRequested,
            this, [this](const QPoint& pos) {
        const int row = selectedRow();
        if (row < 0 || !cfg_ || row >= (int)cfg_->records.size()) return;
        QMenu m(this);
        QAction* aExport = m.addAction(tr("Save a backup copy (ISO)..."));
        QAction* aFiles  = m.addAction(tr("Look inside this game's files..."));
        QAction* aRename = m.addAction(tr("Rename"));
        QAction* aVideos = m.addAction(tr("Videos (turn off / restore)..."));
        QAction* aDnas   = m.addAction(tr("DNAS patch..."));
        QAction* aPs2    = m.addAction(tr("PS2Load options..."));
        m.addSeparator();
        QAction* aCopy   = m.addAction(tr("Copy to another drive..."));
        QAction* aMove   = m.addAction(tr("Move to another drive..."));
        m.addSeparator();
        QAction* aRemove = m.addAction(tr("Remove"));
        QAction* picked  = m.exec(gameList_->mapToGlobal(pos));
        if (picked == aExport)  exportSelected();
        else if (picked == aFiles)  gameFilesSelected();
        else if (picked == aRename) renameSelected();
        else if (picked == aVideos) videoRipSelected();
        else if (picked == aDnas)   dnasPatchSelected();
        else if (picked == aPs2)    ps2LoadSelected();
        else if (picked == aCopy)   copySelected();
        else if (picked == aMove)   moveSelected();
        else if (picked == aRemove) removeSelected();
    });
    root->addWidget(gameList_, 1);

    // ---- "Useful acquired" strip (original Panel29/ListBox9): support files --
    buildSupportPanel();

    // ---- buttons -----------------------------------------------------------
    actionsPanel_ = new QWidget(this);
    auto* panelL  = new QVBoxLayout(actionsPanel_);
    panelL->setContentsMargins(0, 0, 0, 0);

    auto* gamesBox = new QGroupBox(tr("Your games"), this);
    auto* row1 = new QHBoxLayout(gamesBox);
    auto* btnInstall = new QPushButton(tr("Install a game..."), this);
    btnSelectAll_ = new QPushButton(tr("Select all"), this);
    btnInvert_ = new QPushButton(tr("Invert"), this);
    auto* btnCopy    = new QPushButton(tr("Copy to another drive..."), this);
    auto* btnRename = new QPushButton(tr("Rename"), this);
    auto* btnRemove = new QPushButton(tr("Remove game"), this);
    btnInstall->setToolTip(tr("Copy a game file (ISO) from your computer "
                              "onto the USB drive, ready for the console."));
    btnSelectAll_->setToolTip(tr("Mark every game in the list (the "
                                 "original's \"Select\" button)."));
    btnInvert_->setToolTip(tr("Flip the marked games (the original's "
                              "\"Inverse selection\")."));
    btnCopy->setToolTip(tr("Copy the selected game to a second USB drive."));
    btnRename->setToolTip(tr("Change how the game's name shows in the list."));
    btnRemove->setToolTip(tr("Delete the selected game from the drive."));
    connect(btnInstall, &QPushButton::clicked, this, &MainWindow::installIso);
    connect(btnSelectAll_, &QPushButton::clicked, this, &MainWindow::selectAllGames);
    connect(btnInvert_, &QPushButton::clicked, this, &MainWindow::invertSelection);
    connect(btnCopy,    &QPushButton::clicked, this, &MainWindow::copySelected);
    connect(btnRename,  &QPushButton::clicked, this, &MainWindow::renameSelected);
    connect(btnRemove,  &QPushButton::clicked, this, &MainWindow::removeSelected);
    btnRename->setShortcut(tr("F2"));
    btnRemove->setShortcut(tr("Del"));
    row1->addWidget(btnInstall);
    row1->addWidget(btnSelectAll_);
    row1->addWidget(btnInvert_);
    row1->addWidget(btnCopy);
    row1->addWidget(btnRename);
    row1->addWidget(btnRemove);
    panelL->addWidget(gamesBox);

    auto* toolsBox = new QGroupBox(tr("Fixes & tools"), this);
    auto* row2 = new QHBoxLayout(toolsBox);
    auto* btnFind    = new QPushButton(tr("Find a file..."), this);
    auto* btnCheck   = new QPushButton(tr("Check & repair"), this);
    auto* btnRecover = new QPushButton(tr("Recover games"), this);
    auto* btnReport  = new QPushButton(tr("Report..."), this);
    btnFind->setToolTip(tr("Search for a file inside every game on the "
                           "drive (for example all videos)."));
    btnCheck->setToolTip(tr("Verify that every game is complete and fix "
                            "broken serial codes."));
    btnRecover->setToolTip(tr("Bring back games that vanished from the "
                              "list but still have their files."));
    btnReport->setToolTip(tr("Show a summary of the drive: games, sizes, "
                             "state. Can be saved as a text file."));
    connect(btnFind,    &QPushButton::clicked, this, &MainWindow::findFile);
    connect(btnCheck,   &QPushButton::clicked, this, &MainWindow::checkStructure);
    connect(btnRecover, &QPushButton::clicked, this, &MainWindow::recoverGames);
    connect(btnReport,  &QPushButton::clicked, this, &MainWindow::driveReport);
    row2->addWidget(btnFind);
    row2->addWidget(btnCheck);
    row2->addWidget(btnRecover);
    row2->addWidget(btnReport);
    panelL->addWidget(toolsBox);

    auto* advBox = new QGroupBox(tr("Advanced (for specific games)"), this);
    auto* row3 = new QHBoxLayout(advBox);
    auto* btnFiles  = new QPushButton(tr("Game files..."), this);
    auto* btnVideos = new QPushButton(tr("Videos..."), this);
    auto* btnDnas   = new QPushButton(tr("DNAS patch..."), this);
    auto* btnPs2    = new QPushButton(tr("PS2Load..."), this);
    btnFiles->setToolTip(tr("Look at the files inside the selected game; "
                            "save copies out or put your own file in."));
    btnVideos->setToolTip(tr("Turn a game's videos off so it runs smoother "
                             "from USB; reversible."));
    btnDnas->setToolTip(tr("Fix the DNAS error that stops some games from "
                           "going online."));
    btnPs2->setToolTip(tr("Swap a game's boot program for the PS2Load "
                          "helper — helps stubborn games boot from USB."));
    connect(btnFiles,  &QPushButton::clicked, this, &MainWindow::gameFilesSelected);
    connect(btnVideos, &QPushButton::clicked, this, &MainWindow::videoRipSelected);
    connect(btnDnas,   &QPushButton::clicked, this, &MainWindow::dnasPatchSelected);
    connect(btnPs2,    &QPushButton::clicked, this, &MainWindow::ps2LoadSelected);
    row3->addWidget(btnFiles);
    row3->addWidget(btnVideos);
    row3->addWidget(btnDnas);
    row3->addWidget(btnPs2);
    panelL->addWidget(advBox);
    root->addWidget(actionsPanel_);

    // ---- progress + status --------------------------------------------------
    progress_ = new QProgressBar(this);
    progress_->hide();
    status_ = new QLabel(tr("Ready — select a USB drive and open its games list."), this);
    root->addWidget(progress_);
    root->addWidget(status_);

    setCentralWidget(central);
    buildMenus();
    updateSupportPanel();

    QTimer::singleShot(0, this, &MainWindow::refreshDrives);
    // silent update check shortly after startup
    QTimer::singleShot(2500, this, &MainWindow::autoUpdateCheck);
}

MainWindow::~MainWindow() {
    // The startup update check captures `this`; if it is still talking to
    // GitHub while the window is being destroyed, wait for it to finish
    // before ~QObject runs. 60 s covers the worst-case HTTP timeout chain
    // (two requests x four 10 s WinHTTP timeouts + DNS) with margin.
    if (updateThread_ && updateThread_->isRunning())
        updateThread_->wait(60000);
}

void MainWindow::buildMenus() {
    // ---- File --------------------------------------------------------------
    QMenu* mFile = menuBar()->addMenu(tr("&File"));
    QAction* aOpen = mFile->addAction(tr("&Open games list..."));
    aOpen->setShortcut(QKeySequence(tr("Ctrl+O")));
    connect(aOpen, &QAction::triggered, this, &MainWindow::openDrive);
    QAction* aRefresh = mFile->addAction(tr("&Refresh drives"));
    aRefresh->setShortcut(QKeySequence("F5"));
    connect(aRefresh, &QAction::triggered, this, &MainWindow::refreshDrives);
    QAction* aFolder = mFile->addAction(tr("Open drive &folder"));
    connect(aFolder, &QAction::triggered, this, &MainWindow::openDriveFolder);
    QAction* aCloseList = mFile->addAction(tr("&Close games list"));
    aCloseList->setShortcut(QKeySequence(tr("Ctrl+W")));   // original "Close ul.cfg"
    connect(aCloseList, &QAction::triggered, this, &MainWindow::closeUlCfg);
    mFile->addSeparator();
    QAction* aIsoExp = mFile->addAction(tr("Open ISO to [&Explore/Modify]..."));
    aIsoExp->setShortcut(QKeySequence(tr("Ctrl+I")));      // original shortcut Ctrl+I
    connect(aIsoExp, &QAction::triggered, this, &MainWindow::openIsoExplorer);
    mFile->addSeparator();
    QAction* aExit = mFile->addAction(tr("E&xit"));
    aExit->setShortcut(QKeySequence("Alt+F4"));
    connect(aExit, &QAction::triggered, this, &QWidget::close);

    // ---- Games ---------------------------------------------------------------
    QMenu* mGames = menuBar()->addMenu(tr("&Games"));
    QAction* aInstall = mGames->addAction(tr("&Install a game from an ISO file..."));
    connect(aInstall, &QAction::triggered, this, &MainWindow::installIso);
    QAction* aInstallDisc = mGames->addAction(
        tr("Install a game from a &CD/DVD disc..."));     // original "Create GAME from CD/DVD"
    connect(aInstallDisc, &QAction::triggered, this, &MainWindow::installFromDisc);
    mGames->addSeparator();
    QAction* aCopy = mGames->addAction(tr("&Copy to another drive..."));
    connect(aCopy, &QAction::triggered, this, &MainWindow::copySelected);
    QAction* aMove = mGames->addAction(tr("&Move to another drive..."));
    connect(aMove, &QAction::triggered, this, &MainWindow::moveSelected);
    mGames->addSeparator();
    QAction* aExport = mGames->addAction(tr("Save a &backup copy (ISO)..."));
    connect(aExport, &QAction::triggered, this, &MainWindow::exportSelected);
    QAction* aRename = mGames->addAction(tr("&Rename"));
    connect(aRename, &QAction::triggered, this, &MainWindow::renameSelected);
    QAction* aRemove = mGames->addAction(tr("Re&move"));
    connect(aRemove, &QAction::triggered, this, &MainWindow::removeSelected);

    // ---- Tools -----------------------------------------------------------------
    QMenu* mTools = menuBar()->addMenu(tr("&Tools"));
    QAction* aFind = mTools->addAction(tr("&Find a file..."));
    connect(aFind, &QAction::triggered, this, &MainWindow::findFile);
    QAction* aFiles = mTools->addAction(tr("&Game files (explore / extract / replace)..."));
    connect(aFiles, &QAction::triggered, this, &MainWindow::gameFilesSelected);
    mTools->addSeparator();
    QAction* aCheck = mTools->addAction(tr("&Check & repair"));
    connect(aCheck, &QAction::triggered, this, &MainWindow::checkStructure);
    QAction* aFix = mTools->addAction(tr("Fix &broken serial codes (666.66)"));
    connect(aFix, &QAction::triggered, this, &MainWindow::fixBrokenSerials);
    QAction* aRecover = mTools->addAction(tr("&Recover games..."));
    connect(aRecover, &QAction::triggered, this, &MainWindow::recoverGames);
    mTools->addSeparator();
    QAction* aClean = mTools->addAction(tr("&Clean up drive space..."));
    connect(aClean, &QAction::triggered, this, &MainWindow::cleanDriveSpace);
    QAction* aSort = mTools->addAction(tr("&Sort the games list (A to Z)"));
    connect(aSort, &QAction::triggered, this, &MainWindow::sortGamesList);
    mTools->addSeparator();
    QAction* aReport = mTools->addAction(tr("Drive &report..."));
    connect(aReport, &QAction::triggered, this, &MainWindow::driveReport);
    mTools->addSeparator();
    QAction* aConfig = mTools->addAction(tr("View current &configuration..."));
    aConfig->setShortcut(QKeySequence(tr("Ctrl+F6")));    // original shortcut
    connect(aConfig, &QAction::triggered, this, &MainWindow::viewConfig);

    // ---- Advanced ------------------------------------------------------------
    QMenu* mAdv = menuBar()->addMenu(tr("&Advanced"));
    QAction* aVideos = mAdv->addAction(tr("&Videos (turn off / restore)..."));
    connect(aVideos, &QAction::triggered, this, &MainWindow::videoRipSelected);
    QAction* aDnas = mAdv->addAction(tr("&DNAS patch..."));
    connect(aDnas, &QAction::triggered, this, &MainWindow::dnasPatchSelected);
    QAction* aPs2 = mAdv->addAction(tr("&PS2Load options..."));
    connect(aPs2, &QAction::triggered, this, &MainWindow::ps2LoadSelected);

    // ---- Help -----------------------------------------------------------------
    QMenu* mHelp = menuBar()->addMenu(tr("&Help"));
    QAction* aHelp = mHelp->addAction(tr("&Quick help"));
    connect(aHelp, &QAction::triggered, this, &MainWindow::showQuickHelp);
    QAction* aUpdate = mHelp->addAction(tr("Check for &updates..."));
    connect(aUpdate, &QAction::triggered, this, &MainWindow::checkForUpdates);
    mHelp->addSeparator();
    QAction* aAbout = mHelp->addAction(tr("&About USBUtils"));
    connect(aAbout, &QAction::triggered, this, &MainWindow::about);
}

void MainWindow::setUiBusy(bool busy) {
    // Only the interactive panel is disabled: the progress bar and status
    // label stay alive so the user sees live feedback.
    actionsPanel_->setDisabled(busy);
    gameList_->setDisabled(busy);
    driveCombo_->setDisabled(busy);
    btnOpen_->setDisabled(busy);
    btnSelectAll_->setDisabled(busy);
    btnInvert_->setDisabled(busy);
    if (busy) {
        progress_->setValue(0);
        progress_->show();
    } else {
        progress_->hide();
    }
}

QString MainWindow::currentDir() const {
    const QString t = driveCombo_->currentText();
    return t.section('|', 0, 0).trimmed();
}

int MainWindow::selectedRow() const {
    return gameList_->currentRow();
}

QList<int> MainWindow::selectedRows() const {
    QList<int> rows;
    if (!gameList_) return rows;
    // current row first (the "primary" selection), then the rest, sorted
    const QModelIndexList sel = gameList_->selectionModel()->selectedIndexes();
    for (const QModelIndex& idx : sel) rows.append(idx.row());
    std::sort(rows.begin(), rows.end());
    rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
    return rows;
}

void MainWindow::closeEvent(QCloseEvent* ev) {
    if (busyJobs_ > 0) {
        QMessageBox::information(this, tr("USBUtils"),
            tr("Please wait — an operation is still running (or cancel it)."));
        ev->ignore();
        return;
    }
    QMainWindow::closeEvent(ev);
}

void MainWindow::runJob(const QString& title, uint64_t expectedTotalBytes,
                        const std::function<JobOutcome(const ProgressFn&,
                                                 const std::function<bool()>&)>& job,
                        const std::function<void(const JobOutcome&)>& onDone) {
    setUiBusy(true);
    ++busyJobs_;

    auto* dlg = new QProgressDialog(title, tr("Cancel"), 0, 100, this);
    dlg->setWindowTitle(tr("USBUtils"));
    dlg->setWindowModality(Qt::WindowModal);
    dlg->setAutoClose(false);
    dlg->setAutoReset(false);
    dlg->setMaximum(static_cast<int>(
        std::min<qint64>(1 << 30, std::max<qint64>(1, qint64(expectedTotalBytes >> 20)))));
    dlg->show();

    auto cancelled = std::make_shared<std::atomic_bool>(false);
    connect(dlg, &QProgressDialog::canceled, this, [cancelled]() {
        cancelled->store(true);
    });

    QThread* th = new usbutils::WorkerThread([this, job, onDone, cancelled, dlg]() {
        const JobOutcome outcome = job(
            [this, dlg](uint64_t done, uint64_t total) {
                QMetaObject::invokeMethod(this, [dlg, done, total]() {
                    dlg->setMaximum(static_cast<int>(
                        std::min<qint64>(1 << 30, std::max<qint64>(1, qint64(total >> 20)))));
                    dlg->setValue(static_cast<int>(
                        std::min<qint64>(1 << 30, qint64(done >> 20))));
                }, Qt::QueuedConnection);
            },
            [cancelled]() { return cancelled->load(); });
        QMetaObject::invokeMethod(this, [this, outcome, onDone, dlg]() {
            --busyJobs_;
            dlg->hide();
            dlg->deleteLater();
            setUiBusy(false);
            onDone(outcome);
        }, Qt::QueuedConnection);
    });
    th->start();
}

// ---------------------------------------------------------------------------
// drive / list handling
// ---------------------------------------------------------------------------

void MainWindow::refreshDrives() {
    driveCombo_->clear();
    for (const UsbDrive& d : listDrives()) {
        QString label = QString::fromStdString(d.root);
        if (!d.label.empty()) label += " | " + QString::fromStdString(d.label);
        if (d.hasUlCfg)       label += tr("  [has games]");
        driveCombo_->addItem(label);
    }
    if (driveCombo_->count() == 0) {
        status_->setText(tr("No removable drives found. Plug in your USB "
                            "drive and click \"Refresh drives\"."));
        welcome_->setText(tr(
            "<div style='color:#555; font-size:14pt;'>"
            "<b>Welcome to USBUtils!</b></div>"
            "<div style='color:#555;'><br/>"
            "This tool puts PlayStation 2 games on a USB drive so the console "
            "can play them without discs.<br/><br/>"
            "1. Plug in your USB drive (it should be FAT32)<br/>"
            "2. Click <b>Refresh drives</b> above<br/>"
            "3. Click <b>Open games list</b> — then install your first game"
            "</div>"));
        welcome_->show();
        gameList_->hide();
    }
}

void MainWindow::openDrive() {
    dir_ = currentDir();
    if (dir_.isEmpty()) {
        QMessageBox::information(this, tr("USBUtils"),
            tr("Pick a USB drive at the top first."));
        return;
    }
    refreshGames();
}

void MainWindow::refreshGames() {
    cfg_ = std::make_unique<UlCfg>();
    gameList_->clear();
    welcome_->hide();
    gameList_->show();
    if (!cfg_->load(dir_.toStdString())) {
        status_->setText(tr("No games on %1 yet. Install your first game "
                            "with the button below.").arg(dir_));
        return;
    }
    for (const UlCfgRecord& r : cfg_->records) {
        const std::string id = gameIdHex(r.nameStr());
        const uint64_t sz = gameTotalSize(dir_.toStdString(), id, r.serialStr(), r.parts);
        QString line = QString::fromStdString(r.nameStr());
        line += QString("   [%1]  %2  %3 part(s)  %4")
                    .arg(QString::fromStdString(r.serialStr()))
                    .arg(r.media == kMediaCd ? "CD" : "DVD")
                    .arg(r.parts)
                    .arg(humanSize(sz));
        gameList_->addItem(line);
    }
    if (!cfg_->records.empty()) gameList_->setCurrentRow(0);
    QString msg = tr("%1 game(s) on %2").arg(cfg_->records.size()).arg(dir_);
    uint64_t freeB = 0, totalB = 0;
    if (fsSpace(dir_.toStdString(), freeB, totalB))
        msg += tr("  —  %1 free of %2").arg(humanSize(freeB)).arg(humanSize(totalB));
    status_->setText(msg);
}

void MainWindow::openDriveFolder() {
    const QString d = dir_.isEmpty() ? currentDir() : dir_;
    if (d.isEmpty()) return;
    QDesktopServices::openUrl(QUrl::fromLocalFile(d));
}

// ---------------------------------------------------------------------------
// install / export / rename / remove
// ---------------------------------------------------------------------------

void MainWindow::installIso() {
    if (currentDir().isEmpty()) { QMessageBox::warning(this, tr("USBUtils"), tr("Select a USB drive first.")); return; }
    const QString iso = QFileDialog::getOpenFileName(this, tr("Pick the game file to install"),
                                                      QString(), tr("Game images (*.iso *.ISO);;All files (*)"));
    if (iso.isEmpty()) return;

    // Pre-read serial/volume id for the default name suggestion.
    IsoInfo info = readIsoInfo(iso.toStdString());
    QString suggestion = QString::fromStdString(info.serial);
    if (static_cast<qsizetype>(info.volumeId.size()) > suggestion.size())
        suggestion = QString::fromStdString(info.volumeId);

    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Name the game"),
        tr("How should this game be called in the list?"), QLineEdit::Normal, suggestion, &ok);
    if (!ok || name.isEmpty()) return;

    const QString dir = currentDir();
    const uint64_t expected = [&iso]() {
        FILE* f = std::fopen(iso.toStdString().c_str(), "rb");
        if (!f) return 0ull;
        const bool sok = usbutils::seek64(f, 0, SEEK_END);
        const int64_t sz = sok ? usbutils::tell64(f) : -1;
        std::fclose(f);
        return sz < 0 ? 0ull : static_cast<uint64_t>(sz);
    }();

    runJob(tr("Installing \"%1\"...").arg(name), expected,
        [iso, name, dir](const ProgressFn& prog,
                         const std::function<bool()>& cancel) -> JobOutcome {
            InstallOptions opt;
            opt.gameName = name.toStdString();
            const InstallResult r = usbutils::installIso(
                iso.toStdString(), dir.toStdString(), opt, prog, cancel);
            if (r.ok)
                return {true, QString("Installed \"%1\" — parts: %2, %3 written.")
                            .arg(name).arg(r.parts).arg(humanSize(r.totalBytes))};
            return {false, QString::fromStdString(r.error)};
        },
        [this, dir](const JobOutcome& r) {
            if (r.ok) {
                QMessageBox::information(this, tr("USBUtils"), r.message);
                dir_ = dir;
                refreshGames();
            } else if (r.message == QLatin1String("cancelled")) {
                status_->setText(tr("Install cancelled."));
            } else {
                QMessageBox::critical(this, tr("USBUtils"),
                    tr("Install failed: %1").arg(r.message));
            }
        });
}

// ---------------------------------------------------------------------------
// Create GAME from CD/DVD — original "CrearjuegodesdeDCDVD"
// ---------------------------------------------------------------------------

void MainWindow::installFromDisc() {
    if (currentDir().isEmpty()) {
        QMessageBox::warning(this, tr("USBUtils"),
                             tr("Select a USB drive first."));
        return;
    }

    // probe optical drives (opens each device briefly)
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const std::vector<OpticalDrive> drives = listOpticalDrives();
    QApplication::restoreOverrideCursor();

    if (drives.empty()) {
        QMessageBox::information(this, tr("USBUtils"),
            tr("No CD/DVD drives found on this computer."));
        return;
    }

    QStringList items;
    for (const OpticalDrive& d : drives) {
        QString line = QString::fromStdString(d.display);
        if (d.discPresent) {
            if (d.isPs2)
                line += tr("  [PS2 game: %1]").arg(
                    QString::fromStdString(d.serial));
            else if (!d.volumeId.empty())
                line += tr("  [data disc]");
        } else {
            line += tr("  [no disc]");
        }
        items << line;
    }
    items << tr("Type a device path manually...");
    bool ok = false;
    const QString pick = QInputDialog::getItem(this,
        tr("Install from a CD/DVD disc"),
        tr("Which drive holds the game disc?"), items, 0, false, &ok);
    if (!ok || pick.isEmpty()) return;

    OpticalDrive chosen;
    if (pick == tr("Type a device path manually...")) {
        const QString typed = QInputDialog::getText(this,
            tr("Install from a CD/DVD disc"),
            tr("Device path (for example /dev/sr0 on Linux, or D: on "
               "Windows):")).trimmed();
        if (typed.isEmpty()) return;
        chosen.devicePath = typed.toStdString();
        chosen.display = typed.toStdString();
        // probe the typed path
        QApplication::setOverrideCursor(Qt::WaitCursor);
        const bool probed = probeOpticalDrive(chosen.devicePath, &chosen);
        QApplication::restoreOverrideCursor();
        (void)probed;
        if (!chosen.discPresent) {
            QMessageBox::warning(this, tr("USBUtils"),
                tr("Could not read a PS2 data disc from \"%1\".")
                    .arg(typed));
            return;
        }
    } else {
        const int idx = items.indexOf(pick);
        if (idx >= 0 && idx < int(drives.size()))
            chosen = drives[size_t(idx)];
    }

    if (!chosen.discPresent) {
        QMessageBox::warning(this, tr("USBUtils"),
            tr("There is no disc in that drive. Insert the game disc and "
               "try again."));
        return;
    }
    if (chosen.dataBytes == 0) {
        QMessageBox::warning(this, tr("USBUtils"),
            tr("The disc is not a PS2 data disc (no ISO9660 data found)."));
        return;
    }

    // default name from the volume id / serial
    QString suggestion = QString::fromStdString(chosen.volumeId);
    if (chosen.isPs2 && suggestion.isEmpty())
        suggestion = QString::fromStdString(chosen.serial);
    bool nameOk = false;
    const QString name = QInputDialog::getText(this, tr("Name the game"),
        tr("How should this game be called in the list?"),
        QLineEdit::Normal, suggestion, &nameOk);
    if (!nameOk || name.isEmpty()) return;

    const QString dir = currentDir();
    const QString dev = QString::fromStdString(chosen.devicePath);
    const uint64_t expected = chosen.dataBytes;

    runJob(tr("Installing \"%1\" from disc...").arg(name), expected,
        [dev, name, dir, expected](
            const ProgressFn& prog,
            const std::function<bool()>& cancel) -> JobOutcome {
            InstallOptions opt;
            opt.gameName = name.toStdString();
            opt.sourceSize = expected;     // raw device: PVD-derived size
            const InstallResult r = usbutils::installIso(
                dev.toStdString(), dir.toStdString(), opt, prog, cancel);
            if (r.ok)
                return {true, QString("Installed \"%1\" — parts: %2, %3 written.")
                            .arg(name).arg(r.parts).arg(humanSize(r.totalBytes))};
            return {false, QString::fromStdString(r.error)};
        },
        [this, dir](const JobOutcome& r) {
            if (r.ok) {
                QMessageBox::information(this, tr("USBUtils"), r.message);
                dir_ = dir;
                refreshGames();
            } else if (r.message == QLatin1String("cancelled")) {
                status_->setText(tr("Install cancelled."));
            } else {
                QMessageBox::critical(this, tr("USBUtils"),
                    tr("Install failed: %1").arg(r.message));
            }
        });
}

// ---------------------------------------------------------------------------
// Open ISO to [Explore/Modify] — original TISOForm
// ---------------------------------------------------------------------------

void MainWindow::openIsoExplorer() {
    const QString iso = QFileDialog::getOpenFileName(this,
        tr("Pick the ISO image to explore"), QString(),
        tr("ISO images (*.iso *.ISO);;All files (*)"));
    if (iso.isEmpty()) return;

    auto* dlg = new IsoExplorerDialog(this, iso);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowModality(Qt::NonModal);
    dlg->show();
}

// ---------------------------------------------------------------------------
// Close ul.cfg — original File menu "Close ul.cfg" (Ctrl+W)
// ---------------------------------------------------------------------------

void MainWindow::closeUlCfg() {
    if (!cfg_ && dir_.isEmpty()) return;
    if (busyJobs_ > 0) {
        QMessageBox::information(this, tr("USBUtils"),
            tr("Please wait — an operation is still running."));
        return;
    }
    cfg_.reset();
    dir_.clear();
    gameList_->clear();
    gameList_->hide();
    welcome_->show();
    status_->setText(tr("Games list closed. Open one from the File menu."));
    updateSupportPanel();
}

// ---------------------------------------------------------------------------
// View current config — original "View current config" (Ctrl+F6)
// ---------------------------------------------------------------------------

void MainWindow::viewConfig() {
    ConfigDialog dlg(this);
    ConfigDialog::Context ctx;
    ctx.currentDrive = dir_;
    ctx.ulCfgPath = dir_.isEmpty() ? QString()
        : QDir(dir_).filePath("ul.cfg");
    ctx.gameCount = cfg_ ? int(cfg_->records.size()) : 0;
    ctx.lastUpdateCheck = lastUpdateCheck_;
    dlg.setContext(ctx);
    dlg.exec();
}

// ---------------------------------------------------------------------------
// Select system — original "Select" (mark all) + "Inverse selection"
// ---------------------------------------------------------------------------

void MainWindow::selectAllGames() {
    if (!gameList_->count()) return;
    gameList_->selectAll();
    status_->setText(tr("%1 game(s) marked.").arg(gameList_->count()));
}

void MainWindow::invertSelection() {
    if (!gameList_->count()) return;
    const int n = gameList_->count();
    for (int i = 0; i < n; ++i)
        gameList_->item(i)->setSelected(!gameList_->item(i)->isSelected());
    const int marked = gameList_->selectedItems().size();
    status_->setText(tr("%1 game(s) marked.").arg(marked));
}

// ---------------------------------------------------------------------------
// "Useful acquired" support-file strip — original Panel29/ListBox9
// ---------------------------------------------------------------------------

void MainWindow::buildSupportPanel() {
    supportPanel_ = new QGroupBox(tr("Useful acquired — support files"), this);
    supportPanel_->setToolTip(tr(
        "Shows whether the files that some tools need are present. All of "
        "them ship with USBUtils in the 'data' folder next to the program."));
    auto* lay = new QHBoxLayout(supportPanel_);
    lay->setContentsMargins(8, 4, 8, 4);
    lay->addStretch(1);
    // content is (re)built by updateSupportPanel()
}

void MainWindow::updateSupportPanel() {
    if (!supportPanel_) return;
    // wipe previous content (keep the layout itself)
    while (auto* item = supportPanel_->layout()->takeAt(0)) {
        if (item->widget()) item->widget()->deleteLater();
        delete item;
    }

    const std::vector<SupportStatus> st = checkSupportFiles();
    size_t found = 0;
    for (const SupportStatus& s : st)
        if (s.found) ++found;
    const size_t total = st.size();

    auto* lay = static_cast<QHBoxLayout*>(supportPanel_->layout());
    auto* summary = new QLabel(
        tr("%1/%2 present").arg(found).arg(total), supportPanel_);
    const QString color = (found == total) ? QStringLiteral("#1a7f37")
                                            : QStringLiteral("#b45309");
    summary->setStyleSheet(QStringLiteral("color:%1; font-weight:bold;")
                               .arg(color));
    lay->addWidget(summary);
    lay->addSpacing(12);

    // compact per-file chips (required files only — the full list lives in
    // Tools → View current configuration)
    for (const SupportStatus& s : st) {
        if (!s.file.required) continue;
        auto* chip = new QLabel(QString::fromLatin1(s.file.fileName),
                                supportPanel_);
        chip->setToolTip(s.found
            ? tr("Needed for: %1 — found").arg(
                QString::fromLatin1(s.file.feature))
            : tr("Needed for: %1 — MISSING").arg(
                QString::fromLatin1(s.file.feature)));
        chip->setStyleSheet(QStringLiteral(
            "padding:1px 7px; border-radius:7px; color:%1; background:%2;")
            .arg(s.found ? QStringLiteral("#1a7f37")
                         : QStringLiteral("#b91c1c"))
            .arg(s.found ? QStringLiteral("#e6f4ea")
                         : QStringLiteral("#fde8e8")));
        lay->addWidget(chip);
        lay->addSpacing(4);
    }
    lay->addStretch(1);
}

void MainWindow::exportSelected() {
    const QList<int> rows = selectedRows();
    if (rows.isEmpty() || !cfg_) {
        QMessageBox::warning(this, tr("USBUtils"), tr("Select a game."));
        return;
    }

    // single game: pick a file. several marked games (the original's
    // "Conv. ISO" for marked games): pick a folder and export each as
    // "<game name>.iso".
    QString outFile;
    QString outDir;
    if (rows.size() == 1) {
        outFile = QFileDialog::getSaveFileName(this,
            tr("Save a backup copy of the game"),
            QString::fromStdString(cfg_->records[rows.first()].nameStr()) + ".iso",
            tr("ISO images (*.iso)"));
        if (outFile.isEmpty()) return;
    } else {
        outDir = QFileDialog::getExistingDirectory(this,
            tr("Save %1 backup copies into which folder?")
                .arg(rows.size()));
        if (outDir.isEmpty()) return;
    }

    const QString dir = dir_;
    uint64_t expectedTotal = 0;
    std::vector<UlCfgRecord> recs;
    for (int row : rows) {
        if (row < 0 || row >= int(cfg_->records.size())) continue;
        recs.push_back(cfg_->records[size_t(row)]);
        expectedTotal += gameTotalSize(dir.toStdString(),
            gameIdHex(cfg_->records[size_t(row)].nameStr()),
            cfg_->records[size_t(row)].serialStr(),
            cfg_->records[size_t(row)].parts);
    }

    runJob(rows.size() == 1
               ? tr("Saving a backup copy of \"%1\"...")
                     .arg(QString::fromStdString(recs.front().nameStr()))
               : tr("Saving %1 backup copies...").arg(recs.size()),
           expectedTotal,
        [recs, outFile, outDir, dir](const ProgressFn& prog,
                                     const std::function<bool()>& cancel) -> JobOutcome {
            int done = 0;
            for (const UlCfgRecord& rec : recs) {
                QString target = outFile;
                if (target.isEmpty())
                    target = QDir(outDir).filePath(
                        QString::fromStdString(rec.nameStr()) + ".iso");
                if (!usbutils::exportIso(dir.toStdString(), rec,
                                         target.toStdString(), prog, cancel))
                    return {false, QString::fromStdString(rec.nameStr())};
                ++done;
            }
            return {true, QString::number(done)};
        },
        [this, outFile, outDir, rows](const JobOutcome& r) {
            if (r.ok) {
                status_->setText(rows.size() == 1
                    ? tr("Backup saved: %1").arg(outFile)
                    : tr("%1 backups saved into %2").arg(r.message).arg(outDir));
            } else if (r.message == QLatin1String("cancelled")) {
                status_->setText(tr("Backup cancelled."));
            } else {
                QMessageBox::critical(this, tr("USBUtils"),
                    tr("Backup of \"%1\" failed (or was cancelled).").arg(r.message));
            }
        });
}

void MainWindow::removeSelected() {
    const QList<int> rows = selectedRows();
    if (rows.isEmpty() || !cfg_) {
        QMessageBox::warning(this, tr("USBUtils"), tr("Select a game."));
        return;
    }
    QString question;
    if (rows.size() == 1) {
        question = tr("Remove \"%1\" from the drive?\n\nIts files will be "
                      "deleted. Keep a backup copy first if you might want "
                      "it back.")
            .arg(QString::fromStdString(cfg_->records[rows.first()].nameStr()));
    } else {
        question = tr("Remove these %1 games from the drive?\n\nTheir files "
                      "will be deleted. Keep a backup copy first if you "
                      "might want them back.").arg(rows.size());
    }
    if (QMessageBox::question(this, tr("USBUtils"), question)
            != QMessageBox::Yes)
        return;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    int files = 0, games = 0;
    // delete from the highest row down so indices stay valid
    for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
        if (*it < 0 || *it >= int(cfg_->records.size())) continue;
        files += cfg_->removeGame(size_t(*it));
        ++games;
    }
    QApplication::restoreOverrideCursor();
    status_->setText(tr("Removed %1 game(s) (%2 file(s) deleted).")
                         .arg(games).arg(files));
    refreshGames();
}

void MainWindow::renameSelected() {
    const int row = selectedRow();
    if (row < 0 || !cfg_) { QMessageBox::warning(this, tr("USBUtils"), tr("Select a game.")); return; }
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Rename game"),
        tr("New name:"), QLineEdit::Normal,
        QString::fromStdString(cfg_->records[row].nameStr()), &ok);
    if (!ok || name.isEmpty()) return;
    if (!cfg_->renameGame(static_cast<size_t>(row), name.toStdString())) {
        QMessageBox::critical(this, tr("USBUtils"), tr("Rename failed."));
        return;
    }
    refreshGames();
}

// ---------------------------------------------------------------------------
// copy / move to another drive
// ---------------------------------------------------------------------------

void MainWindow::copySelected()   { copyMoveCommon(false); }
void MainWindow::moveSelected()   { copyMoveCommon(true); }

void MainWindow::copyMoveCommon(bool move) {
    const int row = selectedRow();
    if (row < 0 || !cfg_ || row >= (int)cfg_->records.size()) {
        QMessageBox::warning(this, tr("USBUtils"), tr("Select a game."));
        return;
    }
    if (dir_.isEmpty()) return;

    // destination picker: other removable drives first, folder fallback
    QStringList choices;
    for (const UsbDrive& d : listDrives()) {
        const QString root = QString::fromStdString(d.root);
        if (root != dir_) choices << root;
    }
    choices << tr("Other folder...");
    bool okPick = false;
    const QString pick = QInputDialog::getItem(this,
        move ? tr("Move game to...") : tr("Copy game to..."),
        tr("Where should \"%1\" go?")
            .arg(QString::fromStdString(cfg_->records[row].nameStr())),
        choices, 0, false, &okPick);
    if (!okPick || pick.isEmpty()) return;

    QString dest = pick;
    if (pick == tr("Other folder...")) {
        dest = QFileDialog::getExistingDirectory(this,
            tr("Pick the destination folder"));
        if (dest.isEmpty()) return;
    }

    const UlCfgRecord rec = cfg_->records[row];
    const std::string id = gameIdHex(rec.nameStr());
    const uint64_t size =
        gameTotalSize(dir_.toStdString(), id, rec.serialStr(), rec.parts);

    // free-space sanity warning
    uint64_t freeB = 0, totalB = 0;
    if (fsSpace(dest.toStdString(), freeB, totalB) && freeB < size) {
        QMessageBox::warning(this, tr("USBUtils"),
            tr("The destination has only %1 free, but the game needs %2.")
                .arg(humanSize(freeB)).arg(humanSize(size)));
        return;
    }

    if (QMessageBox::question(this, tr("USBUtils"),
            tr("%1 \"%2\" to:\n%3 ?")
                .arg(move ? tr("Move") : tr("Copy"))
                .arg(QString::fromStdString(rec.nameStr()))
                .arg(dest)) != QMessageBox::Yes)
        return;

    const QString srcDir = dir_;
    const int idx = row;
    runJob(tr("%1 \"%2\"...")
               .arg(move ? tr("Moving") : tr("Copying"))
               .arg(QString::fromStdString(rec.nameStr())),
           size,
        [this, srcDir, idx, dest, move](const ProgressFn& prog,
                                        const std::function<bool()>& cancel) -> JobOutcome {
            // NOTE: uses the live cfg_ (record positions may have changed
            // since the dialog was opened; re-resolve by id+serial)
            const std::string id2 = gameIdHex(cfg_->records[idx].nameStr());
            const size_t at = cfg_->findByIdSerial(
                id2, cfg_->records[idx].serialStr());
            if (at == SIZE_MAX)
                return {false, tr("the game is no longer in the list")};
            const CopyResult r = copyGame(srcDir.toStdString(), *cfg_, at,
                                          dest.toStdString(), move, prog, cancel);
            if (r.ok)
                return {true, QString("%1 (%2 copied)")
                                  .arg(move ? tr("Game moved") : tr("Game copied"))
                                  .arg(humanSize(r.copiedBytes))};
            return {false, QString::fromStdString(r.error)};
        },
        [this, move](const JobOutcome& r) {
            if (r.ok) {
                status_->setText(r.message);
                refreshGames();
            } else if (r.message == QLatin1String("cancelled")) {
                status_->setText(move ? tr("Move cancelled.") : tr("Copy cancelled."));
            } else {
                QMessageBox::critical(this, tr("USBUtils"),
                    tr("Could not finish: %1").arg(r.message));
            }
        });
}

// ---------------------------------------------------------------------------
// check & repair / fix 666.66 / recover
// ---------------------------------------------------------------------------

void MainWindow::checkStructure() {
    if (!cfg_) { QMessageBox::information(this, tr("USBUtils"), tr("Open a games list first.")); return; }
    QString report;
    int bad = 0;
    for (size_t i = 0; i < cfg_->records.size(); ++i) {
        const UlCfgRecord& r = cfg_->records[i];
        const std::string id = gameIdHex(r.nameStr());
        const StructureReport rep = usbutils::checkStructure(dir_.toStdString(), id, r.serialStr(), r.parts);
        if (!rep.ok()) {
            ++bad;
            report += QString("[%1] %2 — missing parts: ").arg(i)
                        .arg(QString::fromStdString(r.nameStr()));
            for (uint32_t m : rep.missing) report += QString("%1 ").arg(m, 2, 10, QChar('0'));
            report += "\n";
        }
    }
    QString msg = bad == 0
        ? tr("All %1 games are complete and healthy.").arg(cfg_->records.size())
        : tr("%1 game(s) have missing parts:\n\n%2").arg(bad).arg(report);

    // also look for broken serial codes ("Correct code 666.66")
    const auto fixes = usbutils::fixBrokenSerials(dir_.toStdString(), *cfg_, false);
    if (!fixes.empty()) {
        QString list;
        for (const SerialFix& f : fixes)
            list += QString("%1: %2 -> %3\n")
                        .arg(QString::fromStdString(f.gameName))
                        .arg(QString::fromStdString(f.oldSerial))
                        .arg(QString::fromStdString(f.newSerial).isEmpty()
                                 ? tr("unknown (cannot fix automatically)")
                                 : QString::fromStdString(f.newSerial));
        msg += tr("\n\nBroken serial codes found:\n%1").arg(list);
        if (QMessageBox::question(this, tr("USBUtils"),
                msg + tr("\nFix the broken serial codes?")) == QMessageBox::Yes) {
            fixBrokenSerials();
            return;
        }
        return;
    }
    QMessageBox::information(this, tr("Check & repair"), msg);
}

void MainWindow::fixBrokenSerials() {
    if (!cfg_) return;
    const auto preview = usbutils::fixBrokenSerials(dir_.toStdString(), *cfg_, false);
    if (preview.empty()) {
        QMessageBox::information(this, tr("Fix broken serial codes"),
            tr("No broken serial codes (666.66) found — everything looks "
               "fine."));
        return;
    }
    QString list;
    int fixable = 0;
    for (const SerialFix& f : preview) {
        list += QString("%1: %2 -> %3\n")
                    .arg(QString::fromStdString(f.gameName))
                    .arg(QString::fromStdString(f.oldSerial))
                    .arg(QString::fromStdString(f.newSerial).isEmpty()
                             ? tr("unknown (cannot fix automatically)")
                             : QString::fromStdString(f.newSerial));
        if (!f.newSerial.empty()) ++fixable;
    }
    if (fixable == 0) {
        QMessageBox::warning(this, tr("Fix broken serial codes"),
            tr("Broken serial codes found, but the real codes could not be "
               "read from the games:\n%1").arg(list));
        return;
    }
    if (QMessageBox::question(this, tr("Fix broken serial codes"),
            tr("These games have a broken serial code (written by another "
               "tool):\n\n%1\nFix them now?").arg(list)) != QMessageBox::Yes)
        return;

    QApplication::setOverrideCursor(Qt::WaitCursor);
    const auto applied = usbutils::fixBrokenSerials(dir_.toStdString(), *cfg_, true);
    QApplication::restoreOverrideCursor();
    int failed = 0;
    for (const SerialFix& f : applied)
        if (!f.applied && !f.newSerial.empty()) ++failed;
    if (failed)
        QMessageBox::warning(this, tr("Fix broken serial codes"),
            tr("%1 fix(es) could not be applied.").arg(failed));
    else
        status_->setText(tr("Broken serial codes fixed."));
    refreshGames();
}

void MainWindow::recoverGames() {
    if (currentDir().isEmpty()) return;
    const auto orphans = findOrphans(currentDir().toStdString());
    if (orphans.empty()) {
        QMessageBox::information(this, tr("USBUtils"), tr("No orphan games found."));
        return;
    }
    QString msg = tr("Found %1 orphan game(s):\n").arg(orphans.size());
    for (const OrphanGame& og : orphans)
        msg += QString("ul.%1.%2  (%3 part(s), %4)\n")
                   .arg(QString::fromStdString(og.idHex))
                   .arg(QString::fromStdString(og.serial))
                   .arg(og.partsFound)
                   .arg(humanSize(og.totalBytes));
    if (QMessageBox::question(this, tr("Recover games"),
            msg + tr("\nRebuild ul.cfg entries?")) != QMessageBox::Yes)
        return;

    QApplication::setOverrideCursor(Qt::WaitCursor);
    UlCfg cfg;
    cfg.load(currentDir().toStdString());
    for (const OrphanGame& og : orphans)
        cfg.append(makeRecoveredRecord(og, og.suggestedName));
    const bool saved = cfg.save();
    QApplication::restoreOverrideCursor();
    if (!saved)
        QMessageBox::warning(this, tr("USBUtils"), tr("Could not write ul.cfg."));
    refreshGames();
}

// ---------------------------------------------------------------------------
// sort / clean (original "Sorting" + "Restore space")
// ---------------------------------------------------------------------------

void MainWindow::sortGamesList() {
    if (!cfg_) {
        QMessageBox::information(this, tr("Sort games"),
            tr("Open a games list first."));
        return;
    }
    if (cfg_->records.size() < 2) {
        QMessageBox::information(this, tr("Sort games"),
            tr("Nothing to sort — fewer than two games on the list."));
        return;
    }
    size_t moved = 0;
    const bool ok = usbutils::sortGames(currentDir().toStdString(), &moved);
    if (!ok) {
        QMessageBox::warning(this, tr("Sort games"),
            tr("Could not write ul.cfg — the list was not changed."));
        return;
    }
    refreshGames();
    QMessageBox::information(this, tr("Sort games"),
        moved ? tr("Done — the list is now sorted from A to Z (%1 game(s) "
                   "changed place).").arg(quint64(moved))
              : tr("The list is already sorted from A to Z."));
}

void MainWindow::cleanDriveSpace() {
    const QString dir = currentDir();
    if (dir.isEmpty()) {
        QMessageBox::information(this, tr("Clean up drive space"),
            tr("Open a games list first."));
        return;
    }
    const DriveSpaceReport rep =
        usbutils::scanDriveSpace(dir.toStdString());

    QString msg = tr(
        "Drive check:\n\n"
        "Games on the drive: %1\n"
        "Files the games use: %2 (%3)\n"
        "Left-over files no game uses: %4 (%5)\n"
        "Files with size zero: %6\n"
        "Free space right now: %7")
        .arg(quint64(rep.games))
        .arg(quint64(rep.gameFiles)).arg(humanSize(rep.gameBytes))
        .arg(quint64(rep.obsoleteFiles)).arg(humanSize(rep.obsoleteBytes))
        .arg(quint64(rep.zeroSizeFiles))
        .arg(humanSize(rep.freeBytes));

    if (rep.obsolete.empty()) {
        QMessageBox::information(this, tr("Clean up drive space"),
            msg + tr("\n\nNothing to clean — no left-over files."));
        return;
    }

    QString list;
    for (const std::string& n : rep.obsolete)
        list += QString::fromStdString(n) + '\n';
    msg += tr("\n\nLeft-over files:\n%1").arg(list);
    msg += tr("Deleting them frees up %1.")
               .arg(humanSize(rep.obsoleteBytes));

    // Same safety net as Recover: suggest recovering real games first.
    const auto orphans = findOrphans(dir.toStdString());
    if (!orphans.empty())
        msg += tr("\n\nNote: %1 game(s) look recoverable (they are not on "
                  "the list yet). Use \"Recover games...\" first if you "
                  "want to keep them — cleaning now deletes their "
                  "files.").arg(quint64(orphans.size()));

    if (QMessageBox::question(this, tr("Clean up drive space"), msg +
            tr("\n\nDelete these left-over files?")) != QMessageBox::Yes)
        return;

    QApplication::setOverrideCursor(Qt::WaitCursor);
    const int removed = usbutils::restoreSpace(dir.toStdString(), rep);
    QApplication::restoreOverrideCursor();
    if (removed == static_cast<int>(rep.obsoleteFiles))
        QMessageBox::information(this, tr("Clean up drive space"),
            tr("Done — deleted %1 file(s) and freed up %2.")
                .arg(removed).arg(humanSize(rep.obsoleteBytes)));
    else
        QMessageBox::warning(this, tr("Clean up drive space"),
            tr("Deleted %1 of %2 file(s) — the rest could not be removed "
               "(check that the drive is not write-protected).")
                .arg(removed).arg(quint64(rep.obsoleteFiles)));
    refreshGames();
}

// ---------------------------------------------------------------------------
// find / game files / report
// ---------------------------------------------------------------------------

void MainWindow::findFile() {
    if (!cfg_) { QMessageBox::information(this, tr("USBUtils"), tr("Open a games list first.")); return; }
    FindDialog dlg(dir_, *cfg_, this);
    dlg.exec();
}

void MainWindow::gameFilesSelected() {
    const int row = selectedRow();
    if (row < 0 || !cfg_ || row >= (int)cfg_->records.size()) {
        QMessageBox::warning(this, tr("USBUtils"), tr("Select a game."));
        return;
    }
    const UlCfgRecord& r = cfg_->records[row];
    const std::string id = gameIdHex(r.nameStr());
    GameFilesDialog dlg(dir_, QString::fromStdString(r.nameStr()),
                        QString::fromStdString(id),
                        QString::fromStdString(r.serialStr()), r.parts, this);
    dlg.exec();
    refreshGames();      // sizes/state may have changed after injection
}

void MainWindow::driveReport() {
    if (!cfg_) { QMessageBox::information(this, tr("USBUtils"), tr("Open a games list first.")); return; }
    const QString dir = dir_;
    UlCfg cfg = *cfg_;

    setEnabled(false);
    auto* dlg = new QProgressDialog(tr("Preparing the drive report..."),
                                    QString(), 0, 0, this);
    dlg->setWindowModality(Qt::WindowModal);
    dlg->show();

    QThread* th = new usbutils::WorkerThread([this, dir, cfg, dlg]() {
        const std::string rep = generateReport(dir.toStdString(), cfg);
        QMetaObject::invokeMethod(this, [this, rep, dlg]() {
            dlg->hide();
            dlg->deleteLater();
            setEnabled(true);

            auto* box = new QDialog(this);
            box->setWindowTitle(tr("Drive report"));
            box->setModal(true);
            box->resize(720, 480);
            auto* lay = new QVBoxLayout(box);
            auto* text = new QPlainTextEdit(box);
            text->setPlainText(QString::fromStdString(rep));
            text->setReadOnly(true);
            lay->addWidget(text, 1);
            auto* row = new QHBoxLayout;
            auto* btnSave = new QPushButton(tr("Save to file..."), box);
            auto* btnCopy = new QPushButton(tr("Copy to clipboard"), box);
            auto* btnClose = new QPushButton(tr("Close"), box);
            connect(btnSave, &QPushButton::clicked, box, [this, text]() {
                const QString out = QFileDialog::getSaveFileName(
                    this, tr("Save report"), "usbutils-report.txt");
                if (out.isEmpty()) return;
                QFile f(out);
                if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
                    f.write(text->toPlainText().toUtf8());
                    f.close();
                    status_->setText(tr("Report saved: %1").arg(out));
                }
            });
            connect(btnCopy, &QPushButton::clicked, box, [text]() {
                QGuiApplication::clipboard()->setText(text->toPlainText());
            });
            connect(btnClose, &QPushButton::clicked, box, &QDialog::accept);
            row->addWidget(btnSave);
            row->addWidget(btnCopy);
            row->addStretch(1);
            row->addWidget(btnClose);
            lay->addLayout(row);
            box->exec();
            box->deleteLater();
        }, Qt::QueuedConnection);
    });
    th->start();
}

// ---------------------------------------------------------------------------
// per-game dialogs (video rip / DNAS / PS2Load)
// ---------------------------------------------------------------------------

void MainWindow::videoRipSelected() {
    const int row = selectedRow();
    if (row < 0 || !cfg_ || row >= (int)cfg_->records.size()) {
        QMessageBox::warning(this, tr("USBUtils"), tr("Select a game."));
        return;
    }
    const UlCfgRecord& r = cfg_->records[row];
    const std::string id = gameIdHex(r.nameStr());
    VideoRipDialog dlg(dir_, QString::fromStdString(r.nameStr()),
                       QString::fromStdString(id),
                       QString::fromStdString(r.serialStr()), r.parts, this);
    dlg.exec();
    refreshGames();      // sizes/state may have changed
}

void MainWindow::ps2LoadSelected() {
    const int row = selectedRow();
    if (row < 0 || !cfg_ || row >= (int)cfg_->records.size()) {
        QMessageBox::warning(this, tr("USBUtils"), tr("Select a game."));
        return;
    }
    const UlCfgRecord& r = cfg_->records[row];
    const std::string id = gameIdHex(r.nameStr());
    Ps2LoadDialog dlg(dir_, QString::fromStdString(r.nameStr()),
                      QString::fromStdString(id),
                      QString::fromStdString(r.serialStr()), r.parts, this);
    dlg.exec();
    refreshGames();      // the last chunk may have grown
}

void MainWindow::dnasPatchSelected() {
    const int row = selectedRow();
    if (row < 0 || !cfg_ || row >= (int)cfg_->records.size()) {
        QMessageBox::warning(this, tr("USBUtils"), tr("Select a game."));
        return;
    }
    const UlCfgRecord& r = cfg_->records[row];
    const std::string id = gameIdHex(r.nameStr());
    DnasDialog dlg(dir_, QString::fromStdString(r.nameStr()),
                   QString::fromStdString(id),
                   QString::fromStdString(r.serialStr()), r.parts, this);
    dlg.exec();
}

// ---------------------------------------------------------------------------
// update check
// ---------------------------------------------------------------------------

void MainWindow::autoUpdateCheck() {
    if (updatePending_) return;
    handleUpdateResult(false);
}

void MainWindow::checkForUpdates() { handleUpdateResult(true); }

void MainWindow::handleUpdateResult(bool manual) {
    if (updatePending_) return;
    updatePending_ = true;

    if (manual) status_->setText(tr("Checking for updates..."));

    QThread* th = new usbutils::WorkerThread([this, manual]() {
        const ReleaseInfo rel = fetchLatestRelease();
        QMetaObject::invokeMethod(this, [this, rel, manual]() {
            updateThread_ = nullptr;
            updatePending_ = false;
            lastUpdateCheck_ = QLocale::system().toString(
                QDateTime::currentDateTime(), QLocale::ShortFormat);

            if (!rel.ok) {
                if (manual)
                    QMessageBox::warning(this, tr("Check for updates"),
                        tr("Could not check for updates:\n%1")
                            .arg(QString::fromStdString(rel.error)));
                return;
            }
            const QString current = QString::fromLatin1(USBUTILS_VERSION);
            if (!isVersionNewer(current.toStdString(), rel.tag)) {
                if (manual)
                    QMessageBox::information(this, tr("Check for updates"),
                        tr("You are up to date. USBUtils %1 is the latest "
                           "version.").arg(current));
                return;
            }

            // a newer release exists
            const QString tag = QString::fromStdString(rel.tag);
            const QMessageBox::StandardButton b = QMessageBox::question(
                this, tr("Update available"),
                tr("<b>A new version of USBUtils is available: %1</b>"
                   "<br/>(you have %2)<br/><br/>"
                   "Would you like to open the download page?")
                    .arg(tag, current),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
            if (b == QMessageBox::Yes)
                QDesktopServices::openUrl(QUrl(QString::fromStdString(rel.url)));
        }, Qt::QueuedConnection);
    });
    updateThread_ = th;      // cleared in the queued result handler above;
                             // event order guarantees the result event is
                             // posted before finished -> deleteLater
    th->start();
}

// ---------------------------------------------------------------------------
// help / about
// ---------------------------------------------------------------------------

void MainWindow::showQuickHelp() {
    QMessageBox::information(this, tr("Quick help"),
        tr("<h3>How to use USBUtils</h3>"
           "<ol>"
           "<li><b>Plug in a USB drive</b> (FAT32 works best) and pick it "
           "at the top.</li>"
           "<li>Click <b>Open games list</b>.</li>"
           "<li>Click <b>Install a game...</b> and pick a game file (ISO) "
           "from your computer.</li>"
           "<li>Plug the drive into your PS2 and play.</li>"
           "</ol>"
           "<p>Right-click a game for more actions (backup, videos, "
           "patches). Use <b>Check &amp; repair</b> if something looks "
           "wrong, and <b>Recover games</b> if a game vanishes from the "
           "list.</p>"
           "<p>Only use games you own.</p>"));
}

void MainWindow::about() {
    QMessageBox::about(this, tr("About USBUtils"),
        tr("<b>USBUtils %1</b><br/>"
           "Open C/C++/Qt reimplementation of <i>USBUtil v2.2 rev1.0</i> "
           "by ISEKO (2009).<br/><br/>"
           "Reverse-engineering notes and on-disk format documentation ship "
           "in the <code>docs/</code> folder of the repository.<br/><br/>"
           "MIT licensed — see LICENSE.")
            .arg(QString::fromLatin1(USBUTILS_VERSION)));
}
