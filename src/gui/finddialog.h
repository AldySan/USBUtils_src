// SPDX-License-Identifier: MIT
/*
 * FindDialog — "Find file in games" (original Utils menu item).
 * Searches every installed game's file list for a name pattern
 * (wildcards * and ?), optionally filtered by exact size, and can
 * extract any hit straight to the computer.
 */
#pragma once

#include <QDialog>
#include <QString>
#include <QCloseEvent>

#include "../core/ulcfg.h"
#include "../core/gameops.h"

#include <memory>
#include <vector>

class QLineEdit;
class QTableWidget;
class QLabel;
class QPushButton;

class FindDialog : public QDialog {
    Q_OBJECT
public:
    FindDialog(const QString& dir, const usbutils::UlCfg& cfg, QWidget* parent = nullptr);

protected:
    // stack-allocated dialog + worker threads posting events to it: both
    // close paths must respect the busy counter or it is a use-after-free
    void closeEvent(QCloseEvent* ev) override;
    void reject() override;

private slots:
    void search();
    void extractSelected();

private:
    void buildUi();

    QString dir_;
    std::vector<usbutils::UlCfgRecord> records_;   // snapshot of the game list

    QLineEdit*    pattern_ = nullptr;
    QLineEdit*    size_    = nullptr;
    QTableWidget* table_   = nullptr;
    QLabel*       info_    = nullptr;
    QPushButton*  btnExtract_ = nullptr;

    std::vector<usbutils::FileHit> hits_;
    int busyJobs_ = 0;
};
