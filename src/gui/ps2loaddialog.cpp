// SPDX-License-Identifier: MIT
#include "ps2loaddialog.h"

#include "../core/gamefile.h"

#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QSettings>
#include <QThread>
#include <QCoreApplication>
#include <QDir>

#include <memory>

using namespace usbutils;

namespace {

/// Look for PS2LOAD.ELF: the persisted choice first, then next to the
/// executable, then the working directory.
QString defaultElfPath() {
    QSettings s;
    const QString remembered = s.value("ps2load/elf").toString();
    if (!remembered.isEmpty() && QFileInfo::exists(remembered))
        return remembered;

    const QStringList bases = {
        QCoreApplication::applicationDirPath(),
        QDir::currentPath(),
    };
    for (const QString& base : bases)
        for (const QString& rel : {QStringLiteral("PS2LOAD.ELF"),
                                   QStringLiteral("data/PS2LOAD.ELF")}) {
            const QString cand = QDir(base).filePath(rel);
            if (QFileInfo::exists(cand)) return cand;
        }
    return QString();
}

/// Run `fn` on a worker thread behind a wait cursor (the operations are
/// fast — one ELF copy — so a busy dialog is not needed).
template <typename F>
auto runOnThread(F&& fn) -> decltype(fn()) {
    decltype(fn()) result;
    QThread* th = QThread::create([&result, &fn]() { result = fn(); });
    th->start();
    while (th->isRunning())     // process events so the dialog stays alive
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    th->deleteLater();
    return result;
}

} // namespace

Ps2LoadDialog::Ps2LoadDialog(const QString& dir, const QString& gameName,
                             const QString& idHex, const QString& serial,
                             uint32_t parts, QWidget* parent)
    : QDialog(parent), dir_(dir), idHex_(idHex), serial_(serial),
      gameName_(gameName), parts_(parts) {
    setWindowTitle(tr("PS2Load — %1").arg(gameName_));
    setModal(true);
    setMinimumWidth(480);

    auto* root = new QVBoxLayout(this);

    auto* intro = new QLabel(tr(
        "Some games only start from USB when their boot program is swapped "
        "for PS2Load. Injecting it keeps every game file at its original "
        "position on the drive — the original boot settings are backed up "
        "inside the game, so the change can be undone here at any time."),
        this);
    intro->setWordWrap(true);
    root->addWidget(intro);

    auto* form = new QFormLayout;
    elfEdit_ = new QLineEdit(defaultElfPath(), this);
    auto* browse = new QPushButton(tr("Browse..."), this);
    auto* elfRow = new QWidget(this);
    auto* elfLay = new QHBoxLayout(elfRow);
    elfLay->setContentsMargins(0, 0, 0, 0);
    elfLay->addWidget(elfEdit_, 1);
    elfLay->addWidget(browse);
    form->addRow(tr("PS2LOAD.ELF:"), elfRow);
    root->addLayout(form);

    status_ = new QLabel(this);
    status_->setWordWrap(true);
    status_->setStyleSheet("padding:6px;");
    root->addWidget(status_);

    auto* row = new QHBoxLayout;
    btnInject_ = new QPushButton(tr("Inject"), this);
    btnRemove_ = new QPushButton(tr("Remove"), this);
    auto* btnClose = new QPushButton(tr("Close"), this);
    row->addWidget(btnInject_);
    row->addWidget(btnRemove_);
    row->addStretch(1);
    row->addWidget(btnClose);
    root->addLayout(row);

    connect(browse, &QPushButton::clicked, this, &Ps2LoadDialog::browseElf);
    connect(btnInject_, &QPushButton::clicked, this, &Ps2LoadDialog::inject);
    connect(btnRemove_, &QPushButton::clicked, this, &Ps2LoadDialog::remove);
    connect(btnClose, &QPushButton::clicked, this, &QDialog::reject);

    refresh();
}

void Ps2LoadDialog::browseElf() {
    const QString p = QFileDialog::getOpenFileName(
        this, tr("Choose PS2LOAD.ELF"), elfEdit_->text(),
        tr("PS2 loader (PS2LOAD.ELF *.ELF);;All files (*)"));
    if (!p.isEmpty()) {
        elfEdit_->setText(p);
        QSettings().setValue("ps2load/elf", p);
    }
}

void Ps2LoadDialog::refresh() {
    btnInject_->setEnabled(false);
    btnRemove_->setEnabled(false);

    const Ps2LoadInfo info = queryPs2Load(
        dir_.toStdString(), idHex_.toStdString(), serial_.toStdString(),
        parts_);
    if (!info.error.empty()) {
        status_->setText(tr("ERROR: %1").arg(QString::fromStdString(info.error)));
        return;
    }
    switch (info.state) {
    case Ps2LoadState::NotInjected:
        status_->setText(tr(
            "This game is not using PS2Load.\n"
            "Boot file: %1  →  after injecting: %2")
            .arg(QString::fromStdString(info.bootElf),
                 QString::fromStdString(info.ps2lName)));
        btnInject_->setEnabled(true);
        break;
    case Ps2LoadState::InjectedByTool:
        status_->setText(tr(
            "PS2Load is active in this game (boot file %1).\n"
            "It was injected by this tool and can be safely removed.")
            .arg(QString::fromStdString(info.ps2lName)));
        btnRemove_->setEnabled(true);
        break;
    case Ps2LoadState::InjectedManually:
        status_->setText(tr(
            "A PS2Load was added to this game manually (with another "
            "tool such as UltraISO). This tool will not remove it — use "
            "the tool that created it."));
        break;
    }
}

void Ps2LoadDialog::inject() {
    const QString elf = elfEdit_->text().trimmed();
    if (elf.isEmpty() || !QFileInfo::exists(elf)) {
        QMessageBox::warning(this, tr("PS2Load"),
            tr("PS2LOAD.ELF was not found. Pick the file shipped with "
               "USBUtils (data/PS2LOAD.ELF)."));
        return;
    }
    QSettings().setValue("ps2load/elf", elf);

    setEnabled(false);
    const Ps2LoadResult res = runOnThread([&]() {
        return injectPs2Load(dir_.toStdString(), idHex_.toStdString(),
                             serial_.toStdString(), parts_,
                             elf.toStdString());
    });
    setEnabled(true);

    if (!res.ok) {
        QMessageBox::critical(this, tr("PS2Load"),
            tr("Injection failed:\n%1").arg(QString::fromStdString(res.error)));
    } else if (!res.error.empty()) {
        QMessageBox::information(this, tr("PS2Load"),
            tr("Injected as '%1;1'.\n\nNote: %2")
                .arg(QString::fromStdString(res.ps2lName),
                     QString::fromStdString(res.error)));
    }
    refresh();
}

void Ps2LoadDialog::remove() {
    const auto answer = QMessageBox::question(this, tr("PS2Load"),
        tr("Remove PS2Load from this game and restore the original boot "
           "settings?"));
    if (answer != QMessageBox::Yes) return;

    setEnabled(false);
    const Ps2LoadResult res = runOnThread([&]() {
        return removePs2Load(dir_.toStdString(), idHex_.toStdString(),
                             serial_.toStdString(), parts_);
    });
    setEnabled(true);

    if (!res.ok) {
        QMessageBox::critical(this, tr("PS2Load"),
            tr("Removal failed:\n%1").arg(QString::fromStdString(res.error)));
    }
    refresh();
}
