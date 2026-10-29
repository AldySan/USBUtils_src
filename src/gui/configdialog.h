// SPDX-License-Identifier: MIT
/*
 * ConfigDialog — port of the original's "View current config"
 * (Verconfiguracinactual1, shortcut Ctrl+F6): a read-only summary of the
 * tool's live configuration — application + data locations, which of the
 * shipped support files were found ("Useful acquired" detail), the
 * currently open games list and update-check settings.
 */
#pragma once

#include <QDialog>
#include <QString>

class QTreeWidget;
class QPushButton;

class ConfigDialog : public QDialog {
    Q_OBJECT
public:
    explicit ConfigDialog(QWidget* parent = nullptr);

    /// Context injected by MainWindow (what is currently open).
    struct Context {
        QString currentDrive;      ///< opened USB drive root ("" when none)
        QString ulCfgPath;         ///< path of the loaded ul.cfg
        int     gameCount = 0;     ///< games in the list
        QString lastUpdateCheck;   ///< human text, "" when never checked
    };
    void setContext(const Context& ctx);

private slots:
    void copyToClipboard();

private:
    void rebuild();

    QTreeWidget*  tree_ = nullptr;
    QPushButton*  btnCopy_ = nullptr;
    Context       ctx_;
};
