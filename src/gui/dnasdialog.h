// SPDX-License-Identifier: MIT
/*
 * DnasDialog — patch a game's embedded IOPRP image from a DNAS package
 * (usbutil.dni + usbutil.dna), mirroring the original's "DNAS patch"
 * tab. Paths are remembered between runs via QSettings.
 */
#pragma once

#include <QDialog>
#include <QString>

#include "../core/dnas.h"

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;

class DnasDialog : public QDialog {
    Q_OBJECT
public:
    DnasDialog(const QString& dir, const QString& gameName,
               const QString& idHex, const QString& serial,
               uint32_t parts, QWidget* parent = nullptr);

private slots:
    void browseDni();
    void browseDna();
    void loadIndex();
    void patch();

private:
    QString dir_, idHex_, serial_, gameName_;
    uint32_t parts_ = 0;

    usbutils::DnasPackage pkg_;
    QLineEdit*  dniEdit_  = nullptr;
    QLineEdit*  dnaEdit_  = nullptr;
    QComboBox*  verCombo_ = nullptr;
    QLabel*     status_   = nullptr;
    QPushButton* btnPatch_ = nullptr;
};
