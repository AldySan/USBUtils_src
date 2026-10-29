// SPDX-License-Identifier: MIT
#include "gamefilesdialog.h"

#include "../core/gameops.h"
#include "../core/port.h"
#include "workerthread.h"

#include <QTableWidget>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QFileDialog>
#include <QProgressDialog>
#include <QThread>
#include <QCloseEvent>

#include <atomic>
#include <memory>
#include <functional>
#include <cstdio>

using namespace usbutils;

static QString humanSize(uint64_t b) {
    if (b >= 1024ull * 1024 * 1024)
        return QString::number(double(b) / (1024.0 * 1024 * 1024), 'f', 2) + " GiB";
    if (b >= 1024 * 1024)
        return QString::number(double(b) / (1024.0 * 1024), 'f', 2) + " MiB";
    return QString::number(double(b) / 1024.0, 'f', 2) + " KiB";
}

GameFilesDialog::GameFilesDialog(const QString& dir, const QString& gameName,
                                 const QString& idHex, const QString& serial,
                                 uint32_t parts, QWidget* parent)
    : QDialog(parent), dir_(dir), idHex_(idHex), serial_(serial),
      gameName_(gameName), parts_(parts) {
    setWindowTitle(tr("Files inside \"%1\"").arg(gameName_));
    setModal(true);
    resize(720, 480);
    buildUi();
    loadFiles();
}

void GameFilesDialog::buildUi() {
    auto* root = new QVBoxLayout(this);

    auto* intro = new QLabel(tr(
        "These are the files stored inside this game. You can save a copy "
        "of any file to your computer, or replace a file with your own "
        "version (it must be exactly the same size as the original)."),
        this);
    intro->setWordWrap(true);
    root->addWidget(intro);

    table_ = new QTableWidget(this);
    table_->setColumnCount(3);
    table_->setHorizontalHeaderLabels({tr("File"), tr("Size"), tr("Position")});
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table_->verticalHeader()->hide();
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    root->addWidget(table_, 1);

    info_ = new QLabel(this);
    info_->setWordWrap(true);
    root->addWidget(info_);

    auto* row = new QHBoxLayout;
    btnExtract_ = new QPushButton(tr("Save a copy to my computer..."), this);
    btnReplace_ = new QPushButton(tr("Replace with my own file..."), this);
    auto* btnClose = new QPushButton(tr("Close"), this);
    btnExtract_->setEnabled(false);
    btnReplace_->setEnabled(false);
    connect(btnExtract_, &QPushButton::clicked, this,
            &GameFilesDialog::extractSelected);
    connect(btnReplace_, &QPushButton::clicked, this,
            &GameFilesDialog::replaceFile);
    connect(btnClose, &QPushButton::clicked, this, &QDialog::reject);
    connect(table_, &QTableWidget::itemSelectionChanged, this, [this]() {
        const bool has = table_->currentRow() >= 0;
        btnExtract_->setEnabled(has);
        btnReplace_->setEnabled(has);
    });
    row->addWidget(btnExtract_);
    row->addWidget(btnReplace_);
    row->addStretch(1);
    row->addWidget(btnClose);
    root->addLayout(row);
}

void GameFilesDialog::loadFiles() {
    setEnabled(false);
    auto* dlg = new QProgressDialog(
        tr("Reading the file list of \"%1\"...").arg(gameName_),
        QString(), 0, 0, this);
    dlg->setWindowModality(Qt::WindowModal);
    dlg->show();

    const QString dir = dir_, idHex = idHex_, serial = serial_;
    const uint32_t parts = parts_;

    QThread* th = new usbutils::WorkerThread([this, dir, idHex, serial, parts, dlg]() {
        std::string err;
        const auto files = listGameFiles(dir.toStdString(),
                                         idHex.toStdString(),
                                         serial.toStdString(), parts, &err);
        QMetaObject::invokeMethod(this, [this, files, err, dlg]() {
            files_ = files;
            table_->clearContents();
            table_->setRowCount(static_cast<int>(files_.size()));
            for (size_t i = 0; i < files_.size(); ++i) {
                const int r = static_cast<int>(i);
                table_->setItem(r, 0, new QTableWidgetItem(
                    QString::fromStdString(files_[i].path)));
                table_->setItem(r, 1, new QTableWidgetItem(
                    humanSize(files_[i].size)));
                table_->setItem(r, 2, new QTableWidgetItem(
                    QString::number(files_[i].lba)));
            }
            info_->setText(tr("%1 file(s) inside this game.")
                               .arg(static_cast<int>(files_.size())));
            dlg->hide();
            dlg->deleteLater();
            setEnabled(true);
            if (!err.empty())
                QMessageBox::warning(this, windowTitle(),
                    tr("Could not read this game: %1")
                        .arg(QString::fromStdString(err)));
        }, Qt::QueuedConnection);
    });
    th->start();
}

const IsoEntry* GameFilesDialog::selectedEntry() const {
    const int row = table_->currentRow();
    if (row < 0 || row >= static_cast<int>(files_.size())) return nullptr;
    return &files_[static_cast<size_t>(row)];
}

void GameFilesDialog::extractSelected() {
    const IsoEntry* e = selectedEntry();
    if (!e) return;

    const QString name = QString::fromStdString(e->name);
    const QString out = QFileDialog::getSaveFileName(this,
        tr("Save a copy of %1").arg(name), name);
    if (out.isEmpty()) return;

    const QString dir = dir_, idHex = idHex_, serial = serial_;
    const uint32_t parts = parts_;
    const IsoEntry entry = *e;

    setEnabled(false);
    auto* dlg = new QProgressDialog(
        tr("Copying %1 out of the game...").arg(name), tr("Cancel"),
        0, 100, this);
    dlg->setWindowModality(Qt::WindowModal);
    dlg->show();
    auto cancelled = std::make_shared<std::atomic_bool>(false);
    connect(dlg, &QProgressDialog::canceled, this, [cancelled]() {
        cancelled->store(true);
    });

    QThread* th = new usbutils::WorkerThread(
        [this, dir, idHex, serial, parts, entry, out, dlg, cancelled]() {
        const bool ok = extractGameFile(
            dir.toStdString(), idHex.toStdString(), serial.toStdString(),
            parts, entry, out.toStdString(),
            [dlg](uint64_t done, uint64_t total) {
                QMetaObject::invokeMethod(dlg, [dlg, done, total]() {
                    dlg->setMaximum(static_cast<int>(std::min<qint64>(
                        1 << 30, qint64(total >> 10))));
                    dlg->setValue(static_cast<int>(std::min<qint64>(
                        1 << 30, qint64(done >> 10))));
                }, Qt::QueuedConnection);
            },
            [cancelled]() { return cancelled->load(); });
        QMetaObject::invokeMethod(this, [this, ok, out, dlg]() {
            dlg->hide();
            dlg->deleteLater();
            setEnabled(true);
            if (ok)
                QMessageBox::information(this, windowTitle(),
                    tr("Saved a copy to:\n%1").arg(out));
            else
                QMessageBox::critical(this, windowTitle(),
                    tr("Could not copy the file out of the game."));
        }, Qt::QueuedConnection);
    });
    th->start();
}

void GameFilesDialog::replaceFile() {
    const IsoEntry* e = selectedEntry();
    if (!e) return;

    const QString path = QString::fromStdString(e->path);
    const QString local = QFileDialog::getOpenFileName(this,
        tr("Choose the replacement for %1").arg(path), QString(),
        tr("All files (*)"));
    if (local.isEmpty()) return;

    // size pre-check for a friendlier message than the raw engine error
    {
        FILE* f = std::fopen(local.toStdString().c_str(), "rb");
        if (f) {
            usbutils::seek64(f, 0, SEEK_END);
            const int64_t sz = usbutils::tell64(f);
            std::fclose(f);
            if (sz >= 0 && static_cast<uint64_t>(sz) != e->size) {
                QMessageBox::warning(this, windowTitle(),
                    tr("The replacement file is %1 bytes, but the in-game "
                       "file is %2 bytes.\n\nOnly a file of exactly the same "
                       "size can be put in, otherwise the game's layout "
                       "would break.")
                        .arg(static_cast<qulonglong>(sz))
                        .arg(static_cast<qulonglong>(e->size)));
                return;
            }
        }
    }

    if (QMessageBox::question(this, windowTitle(),
            tr("Replace %1 inside \"%2\" with the contents of:\n%3 ?")
                .arg(path).arg(gameName_).arg(local)) != QMessageBox::Yes)
        return;

    const QString dir = dir_, idHex = idHex_, serial = serial_;
    const uint32_t parts = parts_;

    setEnabled(false);
    auto* dlg = new QProgressDialog(
        tr("Putting the new file into the game..."), tr("Cancel"),
        0, 100, this);
    dlg->setWindowModality(Qt::WindowModal);
    dlg->show();
    auto cancelled = std::make_shared<std::atomic_bool>(false);
    connect(dlg, &QProgressDialog::canceled, this, [cancelled]() {
        cancelled->store(true);
    });

    QThread* th = new usbutils::WorkerThread(
        [this, dir, idHex, serial, parts, path, local, dlg, cancelled]() {
        const InjectResult r = injectGameFile(
            dir.toStdString(), idHex.toStdString(), serial.toStdString(),
            parts, path.toStdString(), local.toStdString(),
            [dlg](uint64_t done, uint64_t total) {
                QMetaObject::invokeMethod(dlg, [dlg, done, total]() {
                    dlg->setMaximum(static_cast<int>(std::min<qint64>(
                        1 << 30, qint64(total >> 10))));
                    dlg->setValue(static_cast<int>(std::min<qint64>(
                        1 << 30, qint64(done >> 10))));
                }, Qt::QueuedConnection);
            });
        QMetaObject::invokeMethod(this, [this, r, dlg]() {
            dlg->hide();
            dlg->deleteLater();
            setEnabled(true);
            if (r.ok)
                QMessageBox::information(this, windowTitle(),
                    tr("File replaced (%1).")
                        .arg(humanSize(r.bytes)));
            else
                QMessageBox::critical(this, windowTitle(),
                    tr("Could not replace the file:\n%1")
                        .arg(QString::fromStdString(r.error)));
        }, Qt::QueuedConnection);
    });
    th->start();
}
