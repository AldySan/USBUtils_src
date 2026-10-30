// SPDX-License-Identifier: MIT
/*
 * GameFilesDialog — per-game file browser (the original's
 * "FILE: Explore/Configure" + "EXTRACT..." + "INJECT and match"):
 *   - list every file inside one installed game
 *   - save a copy of any file to the computer
 *   - replace a file in place (the replacement must be exactly the same
 *     size, so the game's layout stays valid)
 */
#pragma once

#include <QDialog>
#include <QString>
#include <QCloseEvent>

#include "../core/iso9660.h"

#include <vector>

class QTableWidget;
class QLabel;
class QPushButton;

class GameFilesDialog : public QDialog {
    Q_OBJECT
public:
    GameFilesDialog(const QString& dir, const QString& gameName,
                    const QString& idHex, const QString& serial,
                    uint32_t parts, QWidget* parent = nullptr);

protected:
    // the dialog runs on the stack of its caller: closing it while a
    // worker thread still posts events to it is a use-after-free, so both
    // the window-manager close and reject() (Esc / Close button) respect
    // the busy counter
    void closeEvent(QCloseEvent* ev) override;
    void reject() override;

private slots:
    void extractSelected();
    void replaceFile();

private:
    void buildUi();
    void loadFiles();
    const usbutils::IsoEntry* selectedEntry() const;

    QString dir_, idHex_, serial_, gameName_;
    uint32_t parts_ = 0;

    std::vector<usbutils::IsoEntry> files_;
    QTableWidget* table_ = nullptr;
    QLabel*       info_  = nullptr;
    QPushButton*  btnExtract_ = nullptr;
    QPushButton*  btnReplace_ = nullptr;
    int busyJobs_ = 0;
};
