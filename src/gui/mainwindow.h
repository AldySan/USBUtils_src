// SPDX-License-Identifier: MIT
/*
 * USBUtils GUI — Qt Widgets reimplementation of USBUtil v2.2's main form.
 * The layout mirrors the original TForm1 (see docs/dfm/TFORM1.dfm.txt):
 * drive selector, game list, action buttons, progress bar, status bar.
 */
#pragma once

#include <QMainWindow>
#include <QString>

#include "../core/ulcfg.h"

class QComboBox;
class QListWidget;
class QProgressBar;
class QLabel;
class QPushButton;

namespace usbutils { struct OrphanGame; }

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);

private slots:
    void refreshDrives();
    void openDrive();
    void refreshGames();
    void installIso();
    void exportSelected();
    void removeSelected();
    void renameSelected();
    void checkStructure();
    void recoverGames();
    void about();

private:
    void setUiBusy(bool busy);
    QString currentDir() const;

    QComboBox*    driveCombo_ = nullptr;
    QListWidget*  gameList_   = nullptr;
    QProgressBar* progress_   = nullptr;
    QLabel*       status_     = nullptr;
    QPushButton*  btnOpen_    = nullptr;

    QString dir_;
    std::unique_ptr<usbutils::UlCfg> cfg_;
};
