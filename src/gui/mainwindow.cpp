// SPDX-License-Identifier: MIT
#include "mainwindow.h"

#include "../core/ulcfg.h"
#include "../core/installer.h"
#include "../core/gamefile.h"
#include "../core/ulid.h"
#include "../core/recovery.h"
#include "../core/usbdevice.h"

#include <QComboBox>
#include <QListWidget>
#include <QProgressBar>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QFileDialog>
#include <QMessageBox>
#include <QInputDialog>
#include <QApplication>
#include <QThread>
#include <QTimer>

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
    setWindowTitle(tr("USBUtils — PS2 USBExtreme manager (open reimplementation)"));
    resize(760, 480);

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
    root->addWidget(gameList_, 1);

    // ---- buttons -----------------------------------------------------------
    auto* row1 = new QHBoxLayout;
    auto* btnInstall = new QPushButton(tr("Create GAME from ISO..."), this);
    auto* btnExport  = new QPushButton(tr("Export to ISO..."), this);
    auto* btnRename  = new QPushButton(tr("Rename"), this);
    auto* btnRemove  = new QPushButton(tr("Remove"), this);
    connect(btnInstall, &QPushButton::clicked, this, &MainWindow::installIso);
    connect(btnExport,  &QPushButton::clicked, this, &MainWindow::exportSelected);
    connect(btnRename,  &QPushButton::clicked, this, &MainWindow::renameSelected);
    connect(btnRemove,  &QPushButton::clicked, this, &MainWindow::removeSelected);
    row1->addWidget(btnInstall);
    row1->addWidget(btnExport);
    row1->addWidget(btnRename);
    row1->addWidget(btnRemove);
    root->addLayout(row1);

    auto* row2 = new QHBoxLayout;
    auto* btnCheck   = new QPushButton(tr("Structure check"), this);
    auto* btnRecover = new QPushButton(tr("Recover games"), this);
    auto* btnAbout   = new QPushButton(tr("About"), this);
    connect(btnCheck,   &QPushButton::clicked, this, &MainWindow::checkStructure);
    connect(btnRecover, &QPushButton::clicked, this, &MainWindow::recoverGames);
    connect(btnAbout,   &QPushButton::clicked, this, &MainWindow::about);
    row2->addWidget(btnCheck);
    row2->addWidget(btnRecover);
    row2->addStretch(1);
    row2->addWidget(btnAbout);
    root->addLayout(row2);

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
    setDisabled(busy);
    if (busy) {
        progress_->setValue(0);
        progress_->show();
    }
}

QString MainWindow::currentDir() const {
    const QString t = driveCombo_->currentText();
    return t.section('|', 0, 0).trimmed();
}

void MainWindow::refreshDrives() {
    driveCombo_->clear();
    for (const UsbDrive& d : listDrives()) {
        QString label = QString::fromStdString(d.root);
        if (!d.label.empty()) label += " | " + QString::fromStdString(d.label);
        if (d.hasUlCfg)       label += tr("  [ul.cfg]");
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
        status_->setText(tr("No ul.cfg on %1 (new game drive).").arg(dir_));
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
    status_->setText(tr("%1 game(s) on %2").arg(cfg_->records.size()).arg(dir_));
}

void MainWindow::installIso() {
    if (currentDir().isEmpty()) { QMessageBox::warning(this, tr("USBUtils"), tr("Select a USB drive first.")); return; }
    const QString iso = QFileDialog::getOpenFileName(this, tr("Select PS2 ISO image"),
                                                      QString(), tr("ISO images (*.iso *.ISO)"));
    if (iso.isEmpty()) return;

    // Pre-read serial for the default name suggestion.
    IsoInfo info = readIsoInfo(iso.toStdString());
    QString suggestion = QString::fromStdString(info.serial);

    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Game name"),
        tr("Display name for this game:"), QLineEdit::Normal, suggestion, &ok);
    if (!ok || name.isEmpty()) return;

    setUiBusy(true);
    const QString dir = currentDir();

    // Run the install on a worker thread, pump progress on the GUI thread.
    QThread* th = QThread::create([this, iso, name, dir]() {
        InstallOptions opt;
        opt.gameName = name.toStdString();
        InstallResult r = usbutils::installIso(iso.toStdString(), dir.toStdString(), opt,
            [this](uint64_t done, uint64_t total) {
                QMetaObject::invokeMethod(this, [this, done, total]() {
                    progress_->setMaximum(int(total >> 20));
                    progress_->setValue(int(done >> 20));
                }, Qt::QueuedConnection);
            });
        QMetaObject::invokeMethod(this, [this, r, dir, name]() {
            setUiBusy(false);
            if (r.ok) {
                QMessageBox::information(this, tr("USBUtils"),
                    tr("Installed \"%1\"\nid=%2  parts=%3\n%4 written.")
                        .arg(name).arg(QString::fromStdString(r.idHex))
                        .arg(r.parts).arg(humanSize(r.totalBytes)));
                dir_ = dir;
                refreshGames();
            } else {
                QMessageBox::critical(this, tr("USBUtils"),
                    tr("Install failed: %1").arg(QString::fromStdString(r.error)));
            }
        }, Qt::QueuedConnection);
    });
    connect(th, &QThread::finished, th, &QObject::deleteLater);
    th->start();
}

void MainWindow::exportSelected() {
    const int row = gameList_->currentRow();
    if (row < 0 || !cfg_) { QMessageBox::warning(this, tr("USBUtils"), tr("Select a game.")); return; }

    const QString out = QFileDialog::getSaveFileName(this, tr("Export to ISO"),
        QString::fromStdString(cfg_->records[row].nameStr()) + ".iso",
        tr("ISO images (*.iso)"));
    if (out.isEmpty()) return;

    setUiBusy(true);
    const UlCfgRecord rec = cfg_->records[row];
    const QString dir = dir_;

    QThread* th = QThread::create([this, rec, out, dir]() {
        const bool ok = usbutils::exportIso(dir.toStdString(), rec, out.toStdString(),
            [this](uint64_t d, uint64_t t) {
                QMetaObject::invokeMethod(this, [this, d, t]() {
                    progress_->setMaximum(int(t >> 20));
                    progress_->setValue(int(d >> 20));
                }, Qt::QueuedConnection);
            });
        QMetaObject::invokeMethod(this, [this, ok, out]() {
            setUiBusy(false);
            if (ok) status_->setText(tr("Exported %1").arg(out));
            else QMessageBox::critical(this, tr("USBUtils"), tr("Export failed."));
        }, Qt::QueuedConnection);
    });
    connect(th, &QThread::finished, th, &QObject::deleteLater);
    th->start();
}

void MainWindow::removeSelected() {
    const int row = gameList_->currentRow();
    if (row < 0 || !cfg_) { QMessageBox::warning(this, tr("USBUtils"), tr("Select a game.")); return; }
    if (QMessageBox::question(this, tr("USBUtils"),
            tr("Remove \"%1\" and all its chunk files?")
                .arg(QString::fromStdString(cfg_->records[row].nameStr()))) != QMessageBox::Yes)
        return;
    cfg_->removeGame(static_cast<size_t>(row));
    refreshGames();
}

void MainWindow::renameSelected() {
    const int row = gameList_->currentRow();
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

    UlCfg cfg;
    cfg.load(currentDir().toStdString());
    for (const OrphanGame& og : orphans)
        cfg.append(makeRecoveredRecord(og, og.suggestedName));
    cfg.save();
    refreshGames();
}

void MainWindow::about() {
    QMessageBox::about(this, tr("About USBUtils"),
        tr("<b>USBUtils</b><br/>"
           "Open C/C++/Qt reimplementation of <i>USBUtil v2.2 rev1.0</i> "
           "by ISEKO (2009).<br/><br/>"
           "Reverse-engineering notes and on-disk format documentation ship "
           "in the <code>docs/</code> folder of the repository.<br/><br/>"
           "MIT licensed — see LICENSE."));
}
