// SPDX-License-Identifier: MIT
/*
 * ShrinkDialog — rebuild one installed game so it takes less space.
 *
 * The media rip turns files off but keeps their space; this dialog does
 * the "real" ripping people did with 2000s rebuild tools: duplicate files
 * are relinked onto one copy, all-zero files collapse onto a single
 * shared extent, unused padding is cut and the chunk files shrink (the
 * engine re-hashes everything afterwards — see core/isoshrink.h).
 */
#pragma once

#include <QDialog>
#include <QString>

#include "../core/isoshrink.h"

class QLabel;
class QPushButton;
class QCheckBox;

class ShrinkDialog : public QDialog {
    Q_OBJECT
public:
    ShrinkDialog(const QString& dir, const QString& gameName,
                 const QString& idHex, const QString& serial,
                 uint32_t parts, QWidget* parent = nullptr);

protected:
    void closeEvent(QCloseEvent* ev) override;
    void reject() override;   // Esc / Close button must respect the busy guard

private slots:
    void analyze();
    void apply();

private:
    void buildUi();
    void fillReport();
    void updateApplyEnabled();

    QString dir_, idHex_, serial_, gameName_;
    uint32_t parts_ = 0;

    usbutils::ShrinkInfo info_;
    QLabel*      report_     = nullptr;
    QLabel*      backupNote_ = nullptr;
    QCheckBox*   discard_    = nullptr;
    QPushButton* btnApply_   = nullptr;
    QPushButton* btnClose_   = nullptr;
    int busyJobs_ = 0;
};
