// SPDX-License-Identifier: MIT
#include "mainwindow.h"

#include "videoripdialog.h"
#include "dnasdialog.h"

#include "../core/ulcfg.h"
#include "../core/installer.h"
#include "../core/gamefile.h"
#include "../core/ulid.h"
#include "../core/recovery.h"
#include "../core/usbdevice.h"
#include "../core/videorip.h"
#include "../core/port.h"

#include <QComboBox>
#include <QListWidget>
#include <QProgressBar>
#include <QLabel>
#include <QPushButton>
#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFileDialog>
#include <QMessageBox>
#include <QInputDialog>
#include <QLineEdit>
#include <QApplication>
#include <QThread>
#include <QTimer>
#include <QProgressDialog>
#include <QCloseEvent>
#include <QMenu>

#ifndef USBUTILS_VERSION
#define USBUTILS_VERSION "0.0.1"
#endif

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
    resize(780, 500);

    auto* central = new QWidget(this);
    auto* root    = new QVBoxLayout(central);

    // ---- drive row -------------------------------------------------------
    auto* driveRow = new QHBoxLayout;
    driveCombo_ = new QComboBox(this);
    driveCombo_->setMinimumWidth(340);
    auto* btnRefresh = new QPushButton(tr("Refresh drives"), this);
    btnOpen_ = new QPushButton(tr("Open games list"), this);
    connect(btnRefresh, &QPushButton::clicked, this, &MainWindow::refreshDrives);
    connect(btnOpen_, &QPushButton::clicked, this, &MainWindow::openDrive);
    driveRow->addWidget(new QLabel(tr("USB drive:"), this));
    driveRow->addWidget(driveCombo_, 1);
    driveRow->addWidget(btnRefresh);
    driveRow->addWidget(btnOpen_);
    root->addLayout(driveRow);

    // ---- game list --------------------------------------------------------
    gameList_ = new QListWidget(this);
    gameList_->setAlternatingRowColors(true);
    gameList_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(gameList_, &QListWidget::customContextMenuRequested,
            this, [this](const QPoint& pos) {
        const int row = selectedRow();
        if (row < 0 || !cfg_ || row >= (int)cfg_->records.size()) return;
        QMenu m(this);
        QAction* aExport = m.addAction(tr("Export to ISO..."));
        QAction* aRename = m.addAction(tr("Rename"));
        QAction* aVideos = m.addAction(tr("Videos (turn off / restore)..."));
        QAction* aDnas   = m.addAction(tr("DNAS patch..."));
        m.addSeparator();
        QAction* aRemove = m.addAction(tr("Remove"));
        QAction* picked  = m.exec(gameList_->mapToGlobal(pos));
        if (picked == aExport)  exportSelected();
        else if (picked == aRename) renameSelected();
        else if (picked == aVideos) videoRipSelected();
        else if (picked == aDnas)   dnasPatchSelected();
        else if (picked == aRemove) removeSelected();
    });
    root->addWidget(gameList_, 1);

    // ---- buttons (kept in one panel so only they get disabled) -----------
    actionsPanel_ = new QWidget(this);
    auto* panelL  = new QVBoxLayout(actionsPanel_);
    panelL->setContentsMargins(0, 0, 0, 0);

    auto* row1 = new QHBoxLayout;
    auto* btnInstall = new QPushButton(tr("Create GAME from ISO..."), this);
    auto* btnExport  = new QPushButton(tr("Export to ISO..."), this);
    auto* btnRename  = new QPushButton(tr("Rename"), this);
    auto* btnRemove  = new QPushButton(tr("Remove"), this);
    connect(btnInstall, &QPushButton::clicked, this, &MainWindow::installIso);
    connect(btnExport,  &QPushButton::clicked, this, &MainWindow::exportSelected);
    connect(btnRename,  &QPushButton::clicked, this, &MainWindow::renameSelected);
    connect(btnRemove,  &QPushButton::clicked, this, &MainWindow::removeSelected);
    btnRename->setShortcut(tr("F2"));
    btnRemove->setShortcut(tr("Del"));
    row1->addWidget(btnInstall);
    row1->addWidget(btnExport);
    row1->addWidget(btnRename);
    row1->addWidget(btnRemove);
    panelL->addLayout(row1);

    auto* row2 = new QHBoxLayout;
    auto* btnVideos  = new QPushButton(tr("Videos..."), this);
    auto* btnDnas    = new QPushButton(tr("DNAS patch..."), this);
    auto* btnCheck   = new QPushButton(tr("Structure check"), this);
    auto* btnRecover = new QPushButton(tr("Recover games"), this);
    auto* btnAbout   = new QPushButton(tr("About"), this);
    connect(btnVideos,  &QPushButton::clicked, this, &MainWindow::videoRipSelected);
    connect(btnDnas,    &QPushButton::clicked, this, &MainWindow::dnasPatchSelected);
    connect(btnCheck,   &QPushButton::clicked, this, &MainWindow::checkStructure);
    connect(btnRecover, &QPushButton::clicked, this, &MainWindow::recoverGames);
    connect(btnAbout,   &QPushButton::clicked, this, &MainWindow::about);
    row2->addWidget(btnVideos);
    row2->addWidget(btnDnas);
    row2->addWidget(btnCheck);
    row2->addWidget(btnRecover);
    row2->addStretch(1);
    row2->addWidget(btnAbout);
    panelL->addLayout(row2);
    root->addWidget(actionsPanel_);

    // ---- progress + status --------------------------------------------------
    progress_ = new QProgressBar(this);
    progress_->hide();
    status_ = new QLabel(tr("Ready — select a USB drive and open its games list."), this);
    root->addWidget(progress_);
    root->addWidget(status_);

    setCentralWidget(central);

    QTimer::singleShot(0, this, &MainWindow::refreshDrives);
}

void MainWindow::setUiBusy(bool busy) {
    // Only the interactive panel is disabled: the progress bar and status
    // label stay alive so the user sees live feedback.
    actionsPanel_->setDisabled(busy);
    gameList_->setDisabled(busy);
    driveCombo_->setDisabled(busy);
    btnOpen_->setDisabled(busy);
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

    QThread* th = QThread::create([this, job, onDone, cancelled, dlg]() {
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
    connect(th, &QThread::finished, th, &QObject::deleteLater);
    th->start();
}

void MainWindow::refreshDrives() {
    driveCombo_->clear();
    for (const UsbDrive& d : listDrives()) {
        QString label = QString::fromStdString(d.root);
        if (!d.label.empty()) label += " | " + QString::fromStdString(d.label);
        if (d.hasUlCfg)       label += tr("  [has games]");
        driveCombo_->addItem(label);
    }
    if (driveCombo_->count() == 0)
        status_->setText(tr("No removable drives found."));
}

void MainWindow::openDrive() {
    dir_ = currentDir();
    if (dir_.isEmpty()) return;
    refreshGames();
}

void MainWindow::refreshGames() {
    gameList_->clear();
    cfg_ = std::make_unique<UlCfg>();
    if (!cfg_->load(dir_.toStdString())) {
        status_->setText(tr("No games on %1 yet (new game drive).").arg(dir_));
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
    QString msg = tr("%1 game(s) on %2").arg(cfg_->records.size()).arg(dir_);
    uint64_t freeB = 0, totalB = 0;
    if (fsSpace(dir_.toStdString(), freeB, totalB))
        msg += tr("  —  %1 free of %2").arg(humanSize(freeB)).arg(humanSize(totalB));
    status_->setText(msg);
}

void MainWindow::installIso() {
    if (currentDir().isEmpty()) { QMessageBox::warning(this, tr("USBUtils"), tr("Select a USB drive first.")); return; }
    const QString iso = QFileDialog::getOpenFileName(this, tr("Select PS2 ISO image"),
                                                      QString(), tr("ISO images (*.iso *.ISO)"));
    if (iso.isEmpty()) return;

    // Pre-read serial/volume id for the default name suggestion.
    IsoInfo info = readIsoInfo(iso.toStdString());
    QString suggestion = QString::fromStdString(info.serial);
    if (static_cast<qsizetype>(info.volumeId.size()) > suggestion.size())
        suggestion = QString::fromStdString(info.volumeId);

    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Game name"),
        tr("Display name for this game:"), QLineEdit::Normal, suggestion, &ok);
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

void MainWindow::exportSelected() {
    const int row = selectedRow();
    if (row < 0 || !cfg_) { QMessageBox::warning(this, tr("USBUtils"), tr("Select a game.")); return; }

    const QString out = QFileDialog::getSaveFileName(this, tr("Export to ISO"),
        QString::fromStdString(cfg_->records[row].nameStr()) + ".iso",
        tr("ISO images (*.iso)"));
    if (out.isEmpty()) return;

    const UlCfgRecord rec = cfg_->records[row];
    const QString dir = dir_;
    const std::string id = gameIdHex(rec.nameStr());
    const uint64_t expected =
        gameTotalSize(dir.toStdString(), id, rec.serialStr(), rec.parts);

    runJob(tr("Exporting \"%1\"...")
               .arg(QString::fromStdString(rec.nameStr())),
           expected,
        [rec, out, dir](const ProgressFn& prog,
                        const std::function<bool()>& cancel) -> JobOutcome {
            const bool ok = usbutils::exportIso(dir.toStdString(), rec,
                                                out.toStdString(), prog, cancel);
            return {ok, ok ? out : QString()};
        },
        [this, out](const JobOutcome& r) {
            if (r.ok)
                status_->setText(tr("Exported %1").arg(out));
            else if (r.message == QLatin1String("cancelled"))
                status_->setText(tr("Export cancelled."));
            else
                QMessageBox::critical(this, tr("USBUtils"),
                    tr("Export failed (or was cancelled)."));
        });
}

void MainWindow::removeSelected() {
    const int row = selectedRow();
    if (row < 0 || !cfg_) { QMessageBox::warning(this, tr("USBUtils"), tr("Select a game.")); return; }
    if (QMessageBox::question(this, tr("USBUtils"),
            tr("Remove \"%1\" and all its chunk files?")
                .arg(QString::fromStdString(cfg_->records[row].nameStr()))) != QMessageBox::Yes)
        return;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const int n = cfg_->removeGame(static_cast<size_t>(row));
    QApplication::restoreOverrideCursor();
    status_->setText(tr("Removed (%1 file(s) deleted).").arg(n));
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

void MainWindow::checkStructure() {
    if (!cfg_) return;
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
    if (bad == 0) report = tr("All %1 games are structurally OK.").arg(cfg_->records.size());
    QMessageBox::information(this, tr("Structure check"), report);
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
