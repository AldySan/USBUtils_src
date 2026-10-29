// SPDX-License-Identifier: MIT
#include "dnasdialog.h"

#include "../core/ulid.h"

#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QFileDialog>
#include <QMessageBox>
#include <QSettings>
#include <QApplication>
#include <QDir>

using namespace usbutils;

DnasDialog::DnasDialog(const QString& dir, const QString& gameName,
                       const QString& idHex, const QString& serial,
                       uint32_t parts, QWidget* parent)
    : QDialog(parent), dir_(dir), idHex_(idHex), serial_(serial),
      gameName_(gameName), parts_(parts) {
    setWindowTitle(tr("DNAS patch — %1").arg(gameName_));
    setModal(true);

    auto* root = new QVBoxLayout(this);
    auto* form = new QFormLayout;

    dniEdit_ = new QLineEdit(this);
    dnaEdit_ = new QLineEdit(this);
    auto* btnDni = new QPushButton(tr("Browse..."), this);
    auto* btnDna = new QPushButton(tr("Browse..."), this);
    auto* rowDni = new QHBoxLayout;
    auto* rowDna = new QHBoxLayout;
    rowDni->addWidget(dniEdit_, 1);
    rowDni->addWidget(btnDni);
    rowDna->addWidget(dnaEdit_, 1);
    rowDna->addWidget(btnDna);
    form->addRow(tr("Package index (usbutil.dni):"), rowDni);
    form->addRow(tr("Package data (usbutil.dna):"), rowDna);
    root->addLayout(form);

    auto* intro = new QLabel(tr(
        "Some games fail to go online with a DNAS error. This replaces the "
        "small system file inside the installed game with one from the "
        "package — nothing else on the drive is touched."), this);
    intro->setWordWrap(true);
    root->addWidget(intro);

    form = new QFormLayout;
    verCombo_ = new QComboBox(this);
    form->addRow(tr("DNAS version to apply:"), verCombo_);
    root->addLayout(form);

    status_ = new QLabel(this);
    status_->setWordWrap(true);
    root->addWidget(status_);

    auto* row = new QHBoxLayout;
    row->addStretch(1);
    btnPatch_ = new QPushButton(tr("Patch game"), this);
    auto* btnClose = new QPushButton(tr("Close"), this);
    btnPatch_->setEnabled(false);
    connect(btnDni, &QPushButton::clicked, this, &DnasDialog::browseDni);
    connect(btnDna, &QPushButton::clicked, this, &DnasDialog::browseDna);
    connect(dniEdit_, &QLineEdit::textChanged, this, &DnasDialog::loadIndex);
    connect(dnaEdit_, &QLineEdit::textChanged, this, &DnasDialog::loadIndex);
    connect(btnPatch_, &QPushButton::clicked, this, &DnasDialog::patch);
    connect(btnClose, &QPushButton::clicked, this, &DnasDialog::reject);
    row->addWidget(btnPatch_);
    row->addWidget(btnClose);
    root->addLayout(row);

    // remembered paths, defaulting to <app dir>/data/usbutil.dni|.dna
    QSettings s;
    QString dni = s.value("dnas/dni").toString();
    QString dna = s.value("dnas/dna").toString();
    if (dni.isEmpty()) dni = QDir(QApplication::applicationDirPath())
                                   .filePath("data/usbutil.dni");
    if (dna.isEmpty()) dna = QDir(QApplication::applicationDirPath())
                                   .filePath("data/usbutil.dna");
    dniEdit_->setText(dni);
    dnaEdit_->setText(dna);
    loadIndex();
}

void DnasDialog::browseDni() {
    const QString p = QFileDialog::getOpenFileName(this, tr("Select package index"),
        dniEdit_->text(), tr("DNAS index (*.dni *.DNI);;All files (*)"));
    if (!p.isEmpty()) dniEdit_->setText(p);
}

void DnasDialog::browseDna() {
    const QString p = QFileDialog::getOpenFileName(this, tr("Select package data"),
        dnaEdit_->text(), tr("DNAS data (*.dna *.DNA);;All files (*)"));
    if (!p.isEmpty()) dnaEdit_->setText(p);
}

void DnasDialog::loadIndex() {
    verCombo_->clear();
    btnPatch_->setEnabled(false);
    status_->setText(tr("Loading package..."));

    const QString dni = dniEdit_->text().trimmed();
    const QString dna = dnaEdit_->text().trimmed();
    if (dni.isEmpty() || !pkg_.loadIndex(dni.toStdString())) {
        status_->setText(tr("Package index not found or unreadable."));
        return;
    }
    if (!dna.isEmpty()) pkg_.setDataPath(dna.toStdString());

    for (const DnasEntry& e : pkg_.entries())
        verCombo_->addItem(QString::fromStdString(e.version));
    if (verCombo_->count() == 0) {
        status_->setText(tr("The index contains no versions."));
        return;
    }
    QSettings s;
    s.setValue("dnas/dni", dni);
    s.setValue("dnas/dna", dna);
    status_->setText(tr("Ready — %1 version(s) available.")
                         .arg(verCombo_->count()));
    btnPatch_->setEnabled(true);
}

void DnasDialog::patch() {
    const QString ver = verCombo_->currentText();
    if (ver.isEmpty()) return;
    if (dnaEdit_->text().trimmed().isEmpty()) {
        QMessageBox::warning(this, tr("DNAS patch"),
            tr("Select the package data file (usbutil.dna) first."));
        return;
    }
    if (QMessageBox::question(this, tr("DNAS patch"),
            tr("Apply DNAS %1 to \"%2\"?").arg(ver, gameName_))
            != QMessageBox::Yes)
        return;

    QApplication::setOverrideCursor(Qt::WaitCursor);
    const int n = pkg_.patchGame(dir_.toStdString(), idHex_.toStdString(),
                                 serial_.toStdString(), parts_,
                                 ver.toStdString());
    QApplication::restoreOverrideCursor();

    if (n < 0) {
        QMessageBox::critical(this, tr("DNAS patch"),
            tr("Patch failed — the game has no patchable system file, "
               "or the replacement is too large for it."));
        return;
    }
    QMessageBox::information(this, tr("DNAS patch"),
        tr("Game patched with DNAS %1.").arg(ver));
    status_->setText(tr("Patched with DNAS %1.").arg(ver));
}
