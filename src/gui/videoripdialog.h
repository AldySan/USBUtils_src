// SPDX-License-Identifier: MIT
/*
 * VideoRipDialog — manage in-game videos of one installed game.
 * Mirrors the original's "Video rip" form (deactivate / activate videos):
 *   - list every .PSS video found in the game's ISO directory
 *   - turn videos OFF (size-preserving blank, original kept in a backup)
 *   - turn them back ON while the backup exists
 */
#pragma once

#include <QDialog>
#include <QString>

#include "../core/videorip.h"

#include <functional>
#include <vector>

class QTableWidget;
class QLabel;
class QPushButton;
class QProgressDialog;

class VideoRipDialog : public QDialog {
    Q_OBJECT
public:
    VideoRipDialog(const QString& dir, const QString& gameName,
                   const QString& idHex, const QString& serial,
                   uint32_t parts, QWidget* parent = nullptr);

protected:
    void closeEvent(QCloseEvent* ev) override;

private slots:
    void ripSelected();
    void restoreAll();
    void rescan();

private:
    void buildUi();
    void fillTable();
    void updateInfo();
    std::vector<usbutils::VideoEntry> collectChecked() const;
    void runJob(const QString& runningText,
                const std::function<usbutils::RipResult(
                    const usbutils::ProgressFn&,
                    const std::function<bool()>&)>& job,
                const std::function<void(const usbutils::RipResult&)>& onDone);

    QString dir_, idHex_, serial_, gameName_;
    uint32_t parts_ = 0;

    std::vector<usbutils::VideoEntry> videos_;
    QTableWidget*  table_     = nullptr;
    QLabel*        info_      = nullptr;
    QPushButton*   btnRip_    = nullptr;
    QPushButton*   btnRestore_ = nullptr;
    QPushButton*   btnClose_  = nullptr;
    int busyJobs_ = 0;
};
