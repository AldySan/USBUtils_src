// SPDX-License-Identifier: MIT
/*
 * Ps2LoadDialog — inject / remove PS2Load for one installed game,
 * mirroring the original's "PS2Load options" panel (Panel27 / BitBtn1
 * "Inject" / BitBtn2 "Remove" / status label @Form+0x11F8).
 */
#pragma once

#include <QDialog>
#include <QString>

#include "../core/ps2load.h"

class QLabel;
class QLineEdit;
class QPushButton;

class Ps2LoadDialog : public QDialog {
    Q_OBJECT
public:
    Ps2LoadDialog(const QString& dir, const QString& gameName,
                  const QString& idHex, const QString& serial,
                  uint32_t parts, QWidget* parent = nullptr);

private slots:
    void browseElf();
    void inject();
    void remove();

private:
    void refresh();

    QString dir_, idHex_, serial_, gameName_;
    uint32_t parts_ = 0;

    QLineEdit*  elfEdit_ = nullptr;
    QLabel*     status_ = nullptr;
    QPushButton* btnInject_ = nullptr;
    QPushButton* btnRemove_ = nullptr;
};
