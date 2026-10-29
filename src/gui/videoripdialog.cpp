// SPDX-License-Identifier: MIT
#include "videoripdialog.h"

#include "../core/gamefile.h"
#include "../core/ulid.h"
#include "workerthread.h"

#include <QTableWidget>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QProgressDialog>
#include <QThread>
#include <QCloseEvent>

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

VideoRipDialog::VideoRipDialog(const QString& dir, const QString& gameName,
                               const QString& idHex, const QString& serial,
                               uint32_t parts, QWidget* parent)
    : QDialog(parent), dir_(dir), idHex_(idHex), serial_(serial),
      gameName_(gameName), parts_(parts) {
    setWindowTitle(tr("Videos — %1").arg(gameName_));
    setModal(true);
    resize(620, 420);
    buildUi();
    rescan();
}

void VideoRipDialog::buildUi() {
    auto* root = new QVBoxLayout(this);

    auto* intro = new QLabel(tr(
        "Videos stored on the disc make some games stutter when played from a "
        "USB drive. Turning a video OFF keeps its space on the drive (the game "
        "just skips it) and keeps a backup so you can turn it back ON later."),
        this);
    intro->setWordWrap(true);
    root->addWidget(intro);

    table_ = new QTableWidget(this);
    table_->setColumnCount(4);
    table_->setHorizontalHeaderLabels({tr("Off"), tr("File"), tr("Size"), tr("State")});
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    table_->verticalHeader()->hide();
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    root->addWidget(table_, 1);

    info_ = new QLabel(this);
    info_->setWordWrap(true);
    root->addWidget(info_);

    auto* row = new QHBoxLayout;
    auto* btnAll = new QPushButton(tr("Select all"), this);
    auto* btnNone = new QPushButton(tr("Select none"), this);
    btnRip_ = new QPushButton(tr("Turn OFF selected videos"), this);
    btnRestore_ = new QPushButton(tr("Turn all videos back ON"), this);
    btnClose_ = new QPushButton(tr("Close"), this);
    btnRip_->setEnabled(false);
    btnRestore_->setEnabled(false);
    connect(btnAll, &QPushButton::clicked, this, [this]() {
        for (int r = 0; r < table_->rowCount(); ++r)
            table_->item(r, 0)->setCheckState(Qt::Checked);
    });
    connect(btnNone, &QPushButton::clicked, this, [this]() {
        for (int r = 0; r < table_->rowCount(); ++r)
            table_->item(r, 0)->setCheckState(Qt::Unchecked);
    });
    connect(btnRip_, &QPushButton::clicked, this, &VideoRipDialog::ripSelected);
    connect(btnRestore_, &QPushButton::clicked, this, &VideoRipDialog::restoreAll);
    connect(btnClose_, &QPushButton::clicked, this, &VideoRipDialog::reject);
    row->addWidget(btnAll);
    row->addWidget(btnNone);
    row->addStretch(1);
    row->addWidget(btnRestore_);
    row->addWidget(btnRip_);
    row->addWidget(btnClose_);
    root->addLayout(row);
}

void VideoRipDialog::rescan() {
    std::string err;
    videos_ = scanVideos(dir_.toStdString(), idHex_.toStdString(),
                         serial_.toStdString(), parts_, &err);
    fillTable();
    updateInfo();

    if (!err.empty()) {
        QMessageBox::warning(this, tr("Videos"),
            tr("Could not read this game: %1")
                .arg(QString::fromStdString(err)));
    }
}

void VideoRipDialog::fillTable() {
    table_->clearContents();
    table_->setRowCount(static_cast<int>(videos_.size()));
    for (size_t i = 0; i < videos_.size(); ++i) {
        const VideoEntry& v = videos_[i];
        const int row = static_cast<int>(i);

        auto* chk = new QTableWidgetItem;
        chk->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable |
                      Qt::ItemIsSelectable);
        chk->setCheckState(Qt::Unchecked);
        table_->setItem(row, 0, chk);

        auto* name = new QTableWidgetItem(QString::fromStdString(v.path));
        name->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        table_->setItem(row, 1, name);

        auto* size = new QTableWidgetItem(humanSize(v.size));
        size->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        size->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        table_->setItem(row, 2, size);

        const QString state = v.blanked
            ? (v.hasBackup ? tr("Turned off") : tr("Turned off (no backup)"))
            : tr("Original");
        auto* st = new QTableWidgetItem(state);
        st->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        table_->setItem(row, 3, st);
    }
    btnRestore_->setEnabled(videoBackupSize(dir_.toStdString(),
                                            idHex_.toStdString(),
                                            serial_.toStdString()) > 0);
}

void VideoRipDialog::updateInfo() {
    uint64_t total = 0, off = 0;
    for (const VideoEntry& v : videos_) {
        total += v.size;
        if (v.blanked) off += v.size;
    }
    const uint64_t backup = videoBackupSize(dir_.toStdString(),
                                            idHex_.toStdString(),
                                            serial_.toStdString());
    QString text = tr("%1 video(s), %2 total. %3 currently turned off.")
                       .arg(videos_.size())
                       .arg(humanSize(total))
                       .arg(humanSize(off));
    if (backup > 0)
        text += tr(" Backup on drive: %1.").arg(humanSize(backup));
    else if (off > 0)
        text += tr(" (No backup exists for those — they cannot be turned back on.)");
    info_->setText(text);
    btnRip_->setEnabled(!videos_.empty());
}

std::vector<VideoEntry> VideoRipDialog::collectChecked() const {
    std::vector<VideoEntry> out;
    for (int r = 0; r < table_->rowCount(); ++r)
        if (table_->item(r, 0)->checkState() == Qt::Checked)
            out.push_back(videos_[static_cast<size_t>(r)]);
    return out;
}

/// Run a rip/restore job on a worker thread behind a cancelable,
/// window-modal progress dialog. `onDone` runs on the GUI thread.
void VideoRipDialog::runJob(const QString& runningText,
                            const std::function<RipResult(
                                const ProgressFn&,
                                const std::function<bool()>&)>& job,
                            const std::function<void(const RipResult&)>& onDone) {
    auto* dlg = new QProgressDialog(runningText, tr("Cancel"), 0, 100, this);
    dlg->setWindowTitle(tr("Videos"));
    dlg->setWindowModality(Qt::WindowModal);
    dlg->setAutoClose(false);
    dlg->setAutoReset(false);
    dlg->show();

    auto cancelled = std::make_shared<std::atomic_bool>(false);
    connect(dlg, &QProgressDialog::canceled, this, [cancelled]() {
        cancelled->store(true);
    });
    ++busyJobs_;
    btnRip_->setEnabled(false);
    btnRestore_->setEnabled(false);

    QThread* th = new usbutils::WorkerThread([this, job, onDone, cancelled, dlg]() {
        const RipResult r = job(
            [this, dlg](uint64_t done, uint64_t total) {
                QMetaObject::invokeMethod(this, [dlg, done, total]() {
                    dlg->setMaximum(static_cast<int>(
                        std::min<qint64>(1 << 30, std::max<qint64>(1, qint64(total >> 20)))));
                    dlg->setValue(static_cast<int>(
                        std::min<qint64>(1 << 30, qint64(done >> 20))));
                }, Qt::QueuedConnection);
            },
            [cancelled]() { return cancelled->load(); });
        QMetaObject::invokeMethod(this, [this, r, onDone, dlg]() {
            --busyJobs_;
            dlg->hide();
            dlg->deleteLater();
            onDone(r);
            rescan();
        }, Qt::QueuedConnection);
    });
    connect(th, &QThread::finished, th, &QObject::deleteLater);
    th->start();
}

void VideoRipDialog::ripSelected() {
    const std::vector<VideoEntry> selection = collectChecked();
    if (selection.empty()) {
        QMessageBox::information(this, tr("Videos"),
                                 tr("Tick at least one video first."));
        return;
    }
    uint64_t bytes = 0;
    for (const VideoEntry& v : selection) bytes += v.size;
    if (QMessageBox::question(this, tr("Turn videos off"),
            tr("Turn OFF %1 video(s) (%2)?\n\n"
               "A backup is written next to the game (same size as the "
               "videos), so you can undo this later.")
                .arg(selection.size()).arg(humanSize(bytes)))
            != QMessageBox::Yes)
        return;

    runJob(tr("Turning videos off..."),
        [this, selection](const ProgressFn& prog,
                          const std::function<bool()>& cancel) {
            return ripVideos(dir_.toStdString(), idHex_.toStdString(),
                             serial_.toStdString(), parts_, selection,
                             /*makeBackup*/ true, prog, cancel);
        },
        [this](const RipResult& r) {
            if (r.ok)
                QMessageBox::information(this, tr("Videos"),
                    tr("Turned off %1 video(s) (%2).")
                        .arg(r.rippedCount)
                        .arg(humanSize(r.blankedBytes)));
            else if (r.error != "cancelled")
                QMessageBox::critical(this, tr("Videos"),
                    tr("Could not turn videos off: %1")
                        .arg(QString::fromStdString(r.error)));
        });
}

void VideoRipDialog::restoreAll() {
    if (QMessageBox::question(this, tr("Turn videos back on"),
            tr("Restore every backed-up video of this game?"))
            != QMessageBox::Yes)
        return;

    runJob(tr("Turning videos back on..."),
        [this](const ProgressFn& prog,
               const std::function<bool()>& cancel) {
            return restoreVideos(dir_.toStdString(), idHex_.toStdString(),
                                 serial_.toStdString(), parts_, prog, cancel);
        },
        [this](const RipResult& r) {
            if (r.ok)
                QMessageBox::information(this, tr("Videos"),
                    tr("Restored %1 video(s) (%2). The backup was removed.")
                        .arg(r.rippedCount)
                        .arg(humanSize(r.blankedBytes)));
            else if (r.error != "cancelled")
                QMessageBox::critical(this, tr("Videos"),
                    tr("Could not restore videos: %1")
                        .arg(QString::fromStdString(r.error)));
        });
}

void VideoRipDialog::closeEvent(QCloseEvent* ev) {
    if (busyJobs_ > 0) {
        QMessageBox::information(this, tr("Videos"),
            tr("Please wait — an operation is still running (or cancel it)."));
        ev->ignore();
        return;
    }
    QDialog::closeEvent(ev);
}
