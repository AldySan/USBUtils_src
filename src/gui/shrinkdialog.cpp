// SPDX-License-Identifier: MIT
#include "shrinkdialog.h"

#include "../core/gamefile.h"
#include "../core/ulid.h"
#include "workerthread.h"

#include <QLabel>
#include <QPushButton>
#include <QCheckBox>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QProgressDialog>
#include <QThread>
#include <QCloseEvent>

#include <algorithm>
#include <atomic>
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

ShrinkDialog::ShrinkDialog(const QString& dir, const QString& gameName,
                           const QString& idHex, const QString& serial,
                           uint32_t parts, QWidget* parent)
    : QDialog(parent), dir_(dir), idHex_(idHex), serial_(serial),
      gameName_(gameName), parts_(parts) {
    setWindowTitle(tr("Shrink: %1").arg(gameName_));
    setModal(true);
    resize(520, 300);
    buildUi();
    analyze();
}

void ShrinkDialog::buildUi() {
    auto* root = new QVBoxLayout(this);

    auto* intro = new QLabel(
        tr("Rebuilding relinks duplicate files onto one copy, folds all-zero "
           "files (dummy padding, ripped media) into a single shared extent "
           "and cuts the unused padding — the same idea as the 2000s "
           "rebuild tools, but every file keeps its name and size and the "
           "result is verified byte for byte."), this);
    intro->setWordWrap(true);
    root->addWidget(intro);

    report_ = new QLabel(tr("Analyzing (hashes every file of this game)..."),
                         this);
    report_->setWordWrap(true);
    root->addWidget(report_, 1);

    backupNote_ = new QLabel(this);
    backupNote_->setWordWrap(true);
    backupNote_->setStyleSheet(QStringLiteral("color: #b45309;"));
    backupNote_->hide();
    root->addWidget(backupNote_);

    discard_ = new QCheckBox(
        tr("Delete the rip backups (.rip / .arip) and shrink anyway"), this);
    discard_->hide();
    root->addWidget(discard_);
    connect(discard_, &QCheckBox::toggled, this,
            &ShrinkDialog::updateApplyEnabled);

    auto* warn = new QLabel(
        tr("The rewrite runs to completion once started — keep the drive "
           "plugged in. Use \"Save a backup copy\" on the game first if you "
           "want extra safety."), this);
    warn->setWordWrap(true);
    warn->setStyleSheet(QStringLiteral("color: #6b7280;"));
    root->addWidget(warn);

    auto* row = new QHBoxLayout;
    btnApply_ = new QPushButton(tr("Shrink this game"), this);
    btnClose_ = new QPushButton(tr("Close"), this);
    btnApply_->setEnabled(false);
    connect(btnApply_, &QPushButton::clicked, this, &ShrinkDialog::apply);
    connect(btnClose_, &QPushButton::clicked, this, &ShrinkDialog::reject);
    row->addStretch(1);
    row->addWidget(btnApply_);
    row->addWidget(btnClose_);
    root->addLayout(row);
}

void ShrinkDialog::analyze() {
    auto* dlg = new QProgressDialog(
        tr("Analyzing \"%1\" (hashes every file)...").arg(gameName_),
        tr("Cancel"), 0, 0, this);
    dlg->setWindowModality(Qt::WindowModal);
    dlg->show();

    const QString dir = dir_, idHex = idHex_, serial = serial_;
    const uint32_t parts = parts_;
    auto cancelled = std::make_shared<std::atomic_bool>(false);
    connect(dlg, &QProgressDialog::canceled, this, [cancelled]() {
        cancelled->store(true);
    });

    ++busyJobs_;
    QThread* th = new usbutils::WorkerThread([this, dir, idHex, serial,
                                              parts, cancelled, dlg]() {
        const ShrinkInfo inf = analyzeShrink(
            dir.toStdString(), idHex.toStdString(), serial.toStdString(),
            parts,
            [this, dlg](uint64_t done, uint64_t total) {
                QMetaObject::invokeMethod(this, [dlg, done, total]() {
                    dlg->setMaximum(static_cast<int>(
                        std::min<qint64>(1 << 30, std::max<qint64>(1, qint64(total >> 20)))));
                    dlg->setValue(static_cast<int>(
                        std::min<qint64>(1 << 30, qint64(done >> 20))));
                }, Qt::QueuedConnection);
            },
            [cancelled]() { return cancelled->load(); });
        QMetaObject::invokeMethod(this, [this, inf, dlg]() {
            --busyJobs_;
            dlg->hide();
            dlg->deleteLater();
            info_ = inf;
            fillReport();
        }, Qt::QueuedConnection);
    });
    th->start();
}

void ShrinkDialog::fillReport() {
    updateApplyEnabled();
    if (!info_.ok) {
        report_->setText(tr("This game cannot be shrunk:\n%1")
                             .arg(QString::fromStdString(info_.error)));
        return;
    }
    const double pct = info_.currentBytes
        ? 100.0 * double(info_.savings()) / double(info_.currentBytes) : 0.0;
    QString text = tr("Size on drive now: %1")
                       .arg(humanSize(info_.currentBytes));
    if (info_.duplicateBytes)
        text += tr("\nDuplicate files: %1 (%2 extra cop%3)")
                    .arg(humanSize(info_.duplicateBytes))
                    .arg(info_.duplicateFiles)
                    .arg(info_.duplicateFiles == 1 ? "y" : "ies");
    if (info_.zeroBytes)
        text += tr("\nZero-filled files: %1 (%2 file%3)")
                    .arg(humanSize(info_.zeroBytes))
                    .arg(info_.zeroFiles)
                    .arg(info_.zeroFiles == 1 ? "" : "s");
    if (info_.gapBytes)
        text += tr("\nUnused space: %1").arg(humanSize(info_.gapBytes));
    text += tr("\nAfter the shrink: %1 (saves %2, %3%)")
                .arg(humanSize(info_.finalBytes))
                .arg(humanSize(info_.savings()))
                .arg(QString::number(pct, 'f', 1));
    if (info_.savings() == 0)
        text += tr("\n\nThis game is already compact — nothing to shrink.");
    report_->setText(text);

    if (info_.hasRipBackups) {
        backupNote_->setText(
            tr("This game has rip backups. Their stored offsets would go "
               "stale, so restoring them is impossible after shrinking. "
               "Turn the media back on first, or check the box below."));
        backupNote_->show();
        discard_->show();
    }
}

void ShrinkDialog::updateApplyEnabled() {
    btnApply_->setEnabled(info_.ok && info_.savings() > 0
                          && busyJobs_ == 0
                          && (!info_.hasRipBackups || discard_->isChecked()));
}

void ShrinkDialog::apply() {
    if (!info_.ok || info_.savings() == 0) return;
    if (info_.hasRipBackups && !discard_->isChecked()) return;

    if (QMessageBox::question(this, tr("Shrink game"),
            tr("Rewrite \"%1\" in place?\n\n%2 -> %3 (saves %4)")
                .arg(gameName_)
                .arg(humanSize(info_.currentBytes))
                .arg(humanSize(info_.finalBytes))
                .arg(humanSize(info_.savings())))
            != QMessageBox::Yes)
        return;

    // No cancel button: once the rewrite starts it must finish.
    auto* dlg = new QProgressDialog(
        tr("Shrinking \"%1\"... (do not unplug the drive)").arg(gameName_),
        QString(), 0, 100, this);
    dlg->setWindowTitle(tr("Shrink: %1").arg(gameName_));
    dlg->setWindowModality(Qt::WindowModal);
    dlg->setAutoClose(false);
    dlg->setAutoReset(false);
    dlg->show();

    const QString dir = dir_, idHex = idHex_, serial = serial_;
    const uint32_t parts = parts_;
    const bool discard = info_.hasRipBackups && discard_->isChecked();

    ++busyJobs_;
    btnApply_->setEnabled(false);
    btnClose_->setEnabled(false);
    QThread* th = new usbutils::WorkerThread([this, dir, idHex, serial,
                                              parts, discard, dlg]() {
        const ShrinkResult r = shrinkGame(
            dir.toStdString(), idHex.toStdString(), serial.toStdString(),
            parts, discard,
            [this, dlg](uint64_t done, uint64_t total) {
                QMetaObject::invokeMethod(this, [dlg, done, total]() {
                    dlg->setMaximum(static_cast<int>(
                        std::min<qint64>(1 << 30, std::max<qint64>(1, qint64(total >> 20)))));
                    dlg->setValue(static_cast<int>(
                        std::min<qint64>(1 << 30, qint64(done >> 20))));
                }, Qt::QueuedConnection);
            }, nullptr);
        QMetaObject::invokeMethod(this, [this, r, dlg]() {
            --busyJobs_;
            dlg->hide();
            dlg->deleteLater();
            btnClose_->setEnabled(true);
            if (r.ok && r.newBytes < r.oldBytes) {
                const double pct = r.oldBytes
                    ? 100.0 * double(r.oldBytes - r.newBytes)
                             / double(r.oldBytes) : 0.0;
                QString msg = tr("Shrunk %1 -> %2 (saved %3, %4%).")
                                  .arg(humanSize(r.oldBytes))
                                  .arg(humanSize(r.newBytes))
                                  .arg(humanSize(r.oldBytes - r.newBytes))
                                  .arg(QString::number(pct, 'f', 1));
                if (r.newParts != r.oldParts)
                    msg += tr("\nChunk files: %1 -> %2 (ul.cfg updated).")
                               .arg(r.oldParts).arg(r.newParts);
                msg += r.verified
                    ? tr("\nVerified: every file was re-read and matched.")
                    : tr("\nWarning: the image was rewritten but the final "
                         "check did not complete.");
                QMessageBox::information(this, tr("Shrink game"), msg);
                accept();          // closes the dialog; the list refreshes
            } else if (r.ok) {
                QMessageBox::information(this, tr("Shrink game"),
                                         tr("Nothing left to shrink."));
                analyze();
            } else {
                QMessageBox::critical(this, tr("Shrink game"),
                    tr("Could not shrink this game:\n%1")
                        .arg(QString::fromStdString(r.error)));
                analyze();
            }
        }, Qt::QueuedConnection);
    });
    th->start();
}

void ShrinkDialog::closeEvent(QCloseEvent* ev) {
    if (busyJobs_ > 0) {
        QMessageBox::information(this, tr("Shrink game"),
            tr("Please wait: an operation is still running."));
        ev->ignore();
        return;
    }
    QDialog::closeEvent(ev);
}

// Esc and the Close button call reject() directly — it does NOT go through
// closeEvent(), so without this override the dialog could be destroyed
// while a worker thread still posts events to it.
void ShrinkDialog::reject() {
    if (busyJobs_ > 0) {
        QMessageBox::information(this, tr("Shrink game"),
            tr("Please wait: an operation is still running."));
        return;
    }
    QDialog::reject();
}
