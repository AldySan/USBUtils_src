// SPDX-License-Identifier: MIT
#include "finddialog.h"

#include "../core/ulid.h"
#include "workerthread.h"

#include <QLineEdit>
#include <QTableWidget>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QMessageBox>
#include <QFileDialog>
#include <QProgressDialog>
#include <QThread>
#include <QCloseEvent>

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

FindDialog::FindDialog(const QString& dir, const UlCfg& cfg, QWidget* parent)
    : QDialog(parent), dir_(dir), records_(cfg.records) {
    setWindowTitle(tr("Find a file in your games"));
    setModal(true);
    resize(760, 460);
    buildUi();
}

void FindDialog::buildUi() {
    auto* root = new QVBoxLayout(this);

    auto* intro = new QLabel(tr(
        "Searches inside every game on this drive. Use * as a wildcard, "
        "for example <b>*.PSS</b> finds all videos. Leave the size empty "
        "to match any size."), this);
    intro->setWordWrap(true);
    root->addWidget(intro);

    auto* form = new QFormLayout;
    pattern_ = new QLineEdit(this);
    pattern_->setPlaceholderText(tr("e.g. *.PSS  or  IOPRP*.IMG"));
    size_ = new QLineEdit(this);
    size_->setPlaceholderText(tr("optional: exact size in bytes"));
    form->addRow(tr("File name:"), pattern_);
    form->addRow(tr("Size (bytes):"), size_);
    root->addLayout(form);

    table_ = new QTableWidget(this);
    table_->setColumnCount(3);
    table_->setHorizontalHeaderLabels({tr("Game"), tr("File"), tr("Size")});
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    table_->verticalHeader()->hide();
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    root->addWidget(table_, 1);

    info_ = new QLabel(tr("Enter what to look for, then press Search."), this);
    info_->setWordWrap(true);
    root->addWidget(info_);

    auto* row = new QHBoxLayout;
    auto* btnSearch = new QPushButton(tr("Search"), this);
    btnExtract_ = new QPushButton(tr("Save a copy of the selected file..."), this);
    auto* btnClose = new QPushButton(tr("Close"), this);
    btnExtract_->setEnabled(false);
    btnSearch->setDefault(true);
    connect(btnSearch, &QPushButton::clicked, this, &FindDialog::search);
    connect(btnExtract_, &QPushButton::clicked, this, &FindDialog::extractSelected);
    connect(btnClose, &QPushButton::clicked, this, &QDialog::reject);
    connect(pattern_, &QLineEdit::returnPressed, this, &FindDialog::search);
    row->addWidget(btnSearch);
    row->addWidget(btnExtract_);
    row->addStretch(1);
    row->addWidget(btnClose);
    root->addLayout(row);
}

void FindDialog::search() {
    const std::string pattern = pattern_->text().trimmed().toStdString();

    // busy feedback while walking the games
    setEnabled(false);
    auto* dlg = new QProgressDialog(tr("Searching your games..."), QString(),
                                    0, 0, this);
    dlg->setWindowModality(Qt::WindowModal);
    dlg->show();

    UlCfg cfg;
    cfg.dir = dir_.toStdString();
    cfg.records = records_;
    const QString dir = dir_;

    QThread* th = new usbutils::WorkerThread([this, dir, cfg, pattern, dlg]() {
        const auto hits = findFileInGames(dir.toStdString(), cfg, pattern, 0);
        QMetaObject::invokeMethod(this, [this, hits, dlg]() {
            hits_ = hits;
            table_->clearContents();
            table_->setRowCount(static_cast<int>(hits_.size()));
            for (size_t i = 0; i < hits_.size(); ++i) {
                const int r = static_cast<int>(i);
                table_->setItem(r, 0, new QTableWidgetItem(
                    QString::fromStdString(hits_[i].gameName)));
                table_->setItem(r, 1, new QTableWidgetItem(
                    QString::fromStdString(hits_[i].entry.path)));
                table_->setItem(r, 2, new QTableWidgetItem(
                    humanSize(hits_[i].entry.size)));
            }
            info_->setText(tr("%1 file(s) found.")
                               .arg(static_cast<int>(hits_.size())));
            btnExtract_->setEnabled(!hits_.empty());
            dlg->hide();
            dlg->deleteLater();
            setEnabled(true);
        }, Qt::QueuedConnection);
    });
    connect(th, &QThread::finished, th, &QObject::deleteLater);
    th->start();
}

void FindDialog::extractSelected() {
    const int row = table_->currentRow();
    if (row < 0 || row >= static_cast<int>(hits_.size())) return;
    const FileHit& hit = hits_[static_cast<size_t>(row)];
    if (hit.gameIndex >= records_.size()) return;

    const std::string idHex  = gameIdHex(records_[hit.gameIndex].nameStr());
    const std::string serial = records_[hit.gameIndex].serialStr();
    const uint32_t    parts  = records_[hit.gameIndex].parts;

    const QString name = QString::fromStdString(hit.entry.name);
    const QString out = QFileDialog::getSaveFileName(this,
        tr("Save a copy of %1").arg(name), name);
    if (out.isEmpty()) return;

    const QString dir = dir_;
    const IsoEntry entry = hit.entry;
    const QString gameName = QString::fromStdString(hit.gameName);

    setEnabled(false);
    auto* dlg = new QProgressDialog(
        tr("Copying %1 out of \"%2\"...").arg(name).arg(gameName),
        tr("Cancel"), 0, 100, this);
    dlg->setWindowModality(Qt::WindowModal);
    dlg->show();
    auto cancelled = std::make_shared<std::atomic_bool>(false);
    connect(dlg, &QProgressDialog::canceled, this, [cancelled]() {
        cancelled->store(true);
    });

    QThread* th = new usbutils::WorkerThread(
        [this, dir, idHex, serial, parts, entry, out, dlg, cancelled]() {
        const bool ok = extractGameFile(dir.toStdString(), idHex, serial,
                                        parts, entry, out.toStdString(),
                                        [dlg](uint64_t done, uint64_t total) {
            QMetaObject::invokeMethod(dlg, [dlg, done, total]() {
                dlg->setMaximum(static_cast<int>(std::min<qint64>(
                    1 << 30, qint64(total >> 10))));
                dlg->setValue(static_cast<int>(std::min<qint64>(
                    1 << 30, qint64(done >> 10))));
            }, Qt::QueuedConnection);
        }, [cancelled]() { return cancelled->load(); });
        QMetaObject::invokeMethod(this, [this, ok, out, dlg]() {
            dlg->hide();
            dlg->deleteLater();
            setEnabled(true);
            if (ok)
                QMessageBox::information(this, tr("Find a file"),
                    tr("Saved a copy to:\n%1").arg(out));
            else
                QMessageBox::critical(this, tr("Find a file"),
                    tr("Could not copy the file out of the game."));
        }, Qt::QueuedConnection);
    });
    connect(th, &QThread::finished, th, &QObject::deleteLater);
    th->start();
}
