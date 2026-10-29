// SPDX-License-Identifier: MIT
#include "configdialog.h"

#include "../core/supportfiles.h"

#include <QApplication>
#include <QClipboard>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QStyle>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>

#include <QDir>
#include <QFileInfo>

#include <functional>

using namespace usbutils;

namespace {
QString humanSize(uint64_t b) {
    if (b >= 1024ull * 1024 * 1024)
        return QString::number(double(b) / (1024.0 * 1024 * 1024), 'f', 2) + " GiB";
    if (b >= 1024 * 1024)
        return QString::number(double(b) / (1024.0 * 1024), 'f', 2) + " MiB";
    if (b >= 1024)
        return QString::number(double(b) / 1024.0, 'f', 1) + " KiB";
    return QString::number(quint64(b)) + " B";
}
} // namespace

ConfigDialog::ConfigDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("USBUtils — current configuration"));
    setModal(true);
    resize(720, 480);

    auto* root = new QVBoxLayout(this);

    tree_ = new QTreeWidget(this);
    tree_->setColumnCount(3);
    tree_->setHeaderLabels({tr("Setting"), tr("Value"), tr("Detail")});
    tree_->setAlternatingRowColors(true);
    tree_->setRootIsDecorated(true);
    tree_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    tree_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    tree_->header()->setStretchLastSection(true);
    root->addWidget(tree_, 1);

    auto* row = new QHBoxLayout;
    auto* note = new QLabel(tr(
        "This is the tool's live configuration. Everything is detected "
        "automatically — nothing to edit here."), this);
    note->setStyleSheet("color:#666;");
    note->setWordWrap(true);
    row->addWidget(note, 1);
    btnCopy_ = new QPushButton(tr("Copy as text"), this);
    btnCopy_->setToolTip(tr("Put the whole configuration on the clipboard — "
                            "useful for bug reports."));
    connect(btnCopy_, &QPushButton::clicked, this, &ConfigDialog::copyToClipboard);
    row->addWidget(btnCopy_);
    auto* btnClose = new QPushButton(tr("Close"), this);
    connect(btnClose, &QPushButton::clicked, this, &QDialog::accept);
    row->addWidget(btnClose);
    root->addLayout(row);

    rebuild();
}

void ConfigDialog::setContext(const Context& ctx) {
    ctx_ = ctx;
    rebuild();
}

void ConfigDialog::rebuild() {
    tree_->clear();

    // ---- application --------------------------------------------------------
    auto* appCat = new QTreeWidgetItem(tree_, {tr("Application")});
    appCat->setFirstColumnSpanned(true);
    new QTreeWidgetItem(appCat, {tr("Version"), USBUTILS_VERSION, QString()});
    const QString exeDir = QDir::toNativeSeparators(
        QString::fromStdString(executableDir()));
    new QTreeWidgetItem(appCat, {tr("Program folder"), exeDir, QString()});
    new QTreeWidgetItem(appCat, {tr("Settings file"),
        QFileInfo(QSettings().fileName()).absoluteFilePath(),
        tr("Remembered paths and choices")});

    // ---- games list context -------------------------------------------------
    auto* listCat = new QTreeWidgetItem(tree_, {tr("Games list")});
    listCat->setFirstColumnSpanned(true);
    if (ctx_.currentDrive.isEmpty()) {
        new QTreeWidgetItem(listCat, {tr("Games list"), tr("(not open)"),
            tr("Use File → Open games list")});
    } else {
        new QTreeWidgetItem(listCat, {tr("USB drive"),
            QDir::toNativeSeparators(ctx_.currentDrive), QString()});
        new QTreeWidgetItem(listCat, {tr("ul.cfg"),
            QDir::toNativeSeparators(ctx_.ulCfgPath), QString()});
        new QTreeWidgetItem(listCat, {tr("Games installed"),
            QString::number(ctx_.gameCount), QString()});
    }

    // ---- support files ("Useful acquired") ------------------------------------
    auto* sfCat = new QTreeWidgetItem(tree_, {tr("Support files (Useful acquired)")});
    sfCat->setFirstColumnSpanned(true);

    const std::vector<SupportStatus> st = checkSupportFiles();
    size_t foundCount = 0;
    const size_t total = st.size();
    for (const SupportStatus& s : st) {
        if (s.found) ++foundCount;
        QString value, detail;
        if (s.found) {
            value = s.verified ? tr("Found — original")
                               : tr("Found (differs from original)");
            detail = QString("%1 — %2")
                .arg(QDir::toNativeSeparators(
                    QString::fromStdString(s.foundPath)))
                .arg(humanSize(s.size));
        } else {
            value = s.file.required ? tr("MISSING (feature breaks)")
                                    : tr("missing (optional)");
            detail = s.file.required
                ? tr("Needed for: %1").arg(QString::fromLatin1(s.file.feature))
                : tr("Used by: %1 (optional)").arg(QString::fromLatin1(s.file.feature));
        }
        auto* it = new QTreeWidgetItem(sfCat, {
            QString::fromLatin1(s.file.fileName), value, detail});
        if (s.found && s.verified)
            it->setIcon(1, QApplication::style()->standardIcon(
                QStyle::SP_DialogApplyButton));
        else if (!s.found && s.file.required)
            it->setIcon(1, QApplication::style()->standardIcon(
                QStyle::SP_MessageBoxWarning));
    }
    new QTreeWidgetItem(sfCat, {tr("Summary"),
        tr("%1 of %2 found").arg(foundCount).arg(total),
        tr("Files ship with USBUtils in the 'data' folder")});

    // ---- update checks --------------------------------------------------------
    auto* upCat = new QTreeWidgetItem(tree_, {tr("Update checks")});
    upCat->setFirstColumnSpanned(true);
    new QTreeWidgetItem(upCat, {tr("Automatic check"),
        tr("at every startup"), tr("Silent — only reports newer releases")});
    new QTreeWidgetItem(upCat, {tr("Last check"),
        ctx_.lastUpdateCheck.isEmpty() ? tr("(not checked yet)")
                                       : ctx_.lastUpdateCheck, QString()});
    new QTreeWidgetItem(upCat, {tr("Update source"),
        QString::fromLatin1("github.com/AldySan/USBUtils_src/releases"),
        tr("Latest published release")});

    tree_->expandAll();
}

void ConfigDialog::copyToClipboard() {
    QString text = QStringLiteral("USBUtils %1 — configuration\n")
                       .arg(QString::fromLatin1(USBUTILS_VERSION));
    std::function<void(QTreeWidgetItem*)> walk =
        [&](QTreeWidgetItem* it) {
            for (int i = 0; i < it->childCount(); ++i) {
                QTreeWidgetItem* c = it->child(i);
                if (c->childCount() > 0) {
                    text += QStringLiteral("\n[%1]\n").arg(c->text(0));
                    walk(c);
                } else {
                    text += QStringLiteral("%1: %2").arg(c->text(0), c->text(1));
                    if (!c->text(2).isEmpty())
                        text += QStringLiteral(" — %3").arg(c->text(2));
                    text += QLatin1Char('\n');
                }
            }
        };
    for (int i = 0; i < tree_->topLevelItemCount(); ++i)
        walk(tree_->topLevelItem(i));
    QApplication::clipboard()->setText(text);
}
