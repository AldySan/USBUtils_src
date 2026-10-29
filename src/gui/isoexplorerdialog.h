// SPDX-License-Identifier: MIT
/*
 * IsoExplorerDialog — port of the original TISOForm
 * "Open ISO to [Explore/Modify]" (docs/dfm/TISOFORM.dfm.txt):
 *   - a directory tree of the image with sizes
 *   - editable Volume ID
 *   - context menu: New folder / Add file / Replace / Rename / Delete /
 *     Save file out (extract)
 *   - File menu: Open image, Save a copy of the image..., Close
 * The heavy lifting lives in core/isoedit.{h,cpp}; this dialog only maps
 * IsoNode's to QTreeView items.
 */
#pragma once

#include <QDialog>
#include <memory>

#include "../core/isoedit.h"

using usbutils::IsoNode;

class QTreeView;
class QStandardItemModel;
class QStandardItem;
class QLineEdit;
class QLabel;
class QPushButton;

class IsoExplorerDialog : public QDialog {
    Q_OBJECT
public:
    /// Open the dialog already carrying an image (empty path = ask user).
    explicit IsoExplorerDialog(QWidget* parent = nullptr,
                               const QString& initialPath = QString());
    ~IsoExplorerDialog() override;

    bool loadImage(const QString& path);
    bool hasImage() const { return doc_->isOpen(); }

private slots:
    void openImage();
    void saveImageAs();
    void closeImage();
    void newFolder();
    void addFile();
    void replaceContent();
    void renameSelected();
    void deleteSelected();
    void extractSelected();
    void onVolumeIdChanged(const QString& text);
    void onTreeContextMenu(const QPoint& pos);

private:
    void rebuildTree();
    void setDirty(bool dirty);
    QStandardItem* itemForNode(const IsoNode* node) const;
    const IsoNode* nodeForIndex(const QModelIndex& index) const;
    void updateInfoLine();
    void closeEvent(QCloseEvent* ev) override;

    std::unique_ptr<usbutils::IsoImageDoc> doc_;
    QTreeView*          tree_ = nullptr;
    QStandardItemModel* model_ = nullptr;
    QLineEdit*          volIdEdit_ = nullptr;
    QLabel*             infoLine_ = nullptr;
    QLabel*             sourceLine_ = nullptr;
    QPushButton*        btnAddFile_ = nullptr;
    QPushButton*        btnNewFolder_ = nullptr;
    QPushButton*        btnSave_ = nullptr;
    bool                dirty_ = false;
};
