// SPDX-License-Identifier: MIT
#include "isoexplorerdialog.h"

#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QStatusBar>
#include <QTimer>
#include <QTreeView>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QStandardItemModel>

#include <algorithm>
#include <functional>

using usbutils::IsoNode;

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

constexpr int kRoleNode = Qt::UserRole + 1;
} // namespace

IsoExplorerDialog::IsoExplorerDialog(QWidget* parent, const QString& initialPath)
    : QDialog(parent), doc_(std::make_unique<usbutils::IsoImageDoc>()) {
    setWindowTitle(tr("USBUtils — ISO explorer"));
    setWindowFlag(Qt::WindowMinMaxButtonsHint, true);
    resize(760, 540);

    auto* root = new QVBoxLayout(this);

    // ---- source + volume id row --------------------------------------------
    auto* topRow = new QHBoxLayout;
    sourceLine_ = new QLabel(tr("No image open."), this);
    sourceLine_->setStyleSheet("color:#666;");
    topRow->addWidget(new QLabel(tr("Volume ID:"), this));
    volIdEdit_ = new QLineEdit(this);
    volIdEdit_->setEnabled(false);
    volIdEdit_->setMaxLength(32);
    volIdEdit_->setToolTip(tr("The image's name, as the console shows it. "
                              "Editable — saved with the image."));
    connect(volIdEdit_, &QLineEdit::textEdited,
            this, &IsoExplorerDialog::onVolumeIdChanged);
    topRow->addWidget(volIdEdit_, 1);
    root->addLayout(topRow);
    root->addWidget(sourceLine_);

    // ---- tree ----------------------------------------------------------------
    model_ = new QStandardItemModel(this);
    model_->setHorizontalHeaderLabels({tr("Name"), tr("Type"), tr("Size")});
    tree_ = new QTreeView(this);
    tree_->setModel(model_);
    tree_->setAlternatingRowColors(true);
    tree_->setAnimated(true);
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    tree_->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    tree_->header()->resizeSection(0, 380);
    tree_->header()->setStretchLastSection(true);
    connect(tree_, &QTreeView::customContextMenuRequested,
            this, &IsoExplorerDialog::onTreeContextMenu);
    connect(tree_, &QTreeView::doubleClicked, this,
            [this](const QModelIndex&) { extractSelected(); });
    root->addWidget(tree_, 1);

    // ---- buttons ---------------------------------------------------------------
    auto* btnRow = new QHBoxLayout;
    auto* btnOpen = new QPushButton(tr("Open image..."), this);
    btnNewFolder_ = new QPushButton(tr("New folder"), this);
    btnAddFile_ = new QPushButton(tr("Add file..."), this);
    btnSave_ = new QPushButton(tr("Save a copy of the image..."), this);
    auto* btnClose = new QPushButton(tr("Close image"), this);
    btnNewFolder_->setToolTip(tr("Create a new folder inside the selected one."));
    btnAddFile_->setToolTip(tr("Add a file from this computer into the image."));
    btnSave_->setToolTip(tr("Write the (possibly modified) image to a new "
                            ".iso file. The original file is never touched."));
    connect(btnOpen, &QPushButton::clicked, this, &IsoExplorerDialog::openImage);
    connect(btnNewFolder_, &QPushButton::clicked, this, &IsoExplorerDialog::newFolder);
    connect(btnAddFile_, &QPushButton::clicked, this, &IsoExplorerDialog::addFile);
    connect(btnSave_, &QPushButton::clicked, this, &IsoExplorerDialog::saveImageAs);
    connect(btnClose, &QPushButton::clicked, this, &IsoExplorerDialog::closeImage);
    btnRow->addWidget(btnOpen);
    btnRow->addWidget(btnNewFolder_);
    btnRow->addWidget(btnAddFile_);
    btnRow->addStretch(1);
    btnRow->addWidget(btnSave_);
    btnRow->addWidget(btnClose);
    root->addLayout(btnRow);

    infoLine_ = new QLabel(QString(), this);
    infoLine_->setStyleSheet("color:#666;");
    root->addWidget(infoLine_);

    if (!initialPath.isEmpty())
        loadImage(initialPath);
    else
        QTimer::singleShot(0, this, [this] { updateInfoLine(); });
}

IsoExplorerDialog::~IsoExplorerDialog() = default;

// ---------------------------------------------------------------------------
// loading / closing
// ---------------------------------------------------------------------------

bool IsoExplorerDialog::loadImage(const QString& path) {
    if (dirty_) {
        const auto ret = QMessageBox::question(this, tr("USBUtils"),
            tr("The current image has unsaved changes. Discard them and open "
               "another image?"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (ret != QMessageBox::Yes) return false;
    }

    if (!doc_->open(path.toStdString())) {
        QMessageBox::warning(this, tr("USBUtils"),
            tr("Could not open \"%1\" as an ISO9660 image.")
                .arg(QFileInfo(path).fileName()));
        return false;
    }

    volIdEdit_->blockSignals(true);
    volIdEdit_->setText(QString::fromStdString(doc_->volumeId()));
    volIdEdit_->blockSignals(true);
    volIdEdit_->setEnabled(true);
    sourceLine_->setText(tr("Image: %1 (%2)")
        .arg(QDir::toNativeSeparators(path))
        .arg(humanSize(doc_->originalSize())));
    setDirty(false);
    rebuildTree();
    tree_->expandAll();
    return true;
}

void IsoExplorerDialog::openImage() {
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Open an ISO image"), QString(),
        tr("ISO images (*.iso *.ISO);;All files (*)"));
    if (!path.isEmpty()) loadImage(path);
}

void IsoExplorerDialog::closeImage() {
    if (dirty_) {
        const auto ret = QMessageBox::question(this, tr("USBUtils"),
            tr("The image has unsaved changes. Discard them?"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (ret != QMessageBox::Yes) return;
    }
    doc_->close();
    model_->clear();
    model_->setHorizontalHeaderLabels({tr("Name"), tr("Type"), tr("Size")});
    volIdEdit_->clear();
    volIdEdit_->setEnabled(false);
    sourceLine_->setText(tr("No image open."));
    setDirty(false);
    updateInfoLine();
}

// ---------------------------------------------------------------------------
// tree <-> model
// ---------------------------------------------------------------------------

void IsoExplorerDialog::rebuildTree() {
    model_->clear();
    model_->setHorizontalHeaderLabels({tr("Name"), tr("Type"), tr("Size")});
    if (!doc_->isOpen()) { updateInfoLine(); return; }

    // recursive item builder
    std::function<void(const IsoNode*, QStandardItem*)> build =
        [&](const IsoNode* node, QStandardItem* parentItem) {
            for (const auto& c : node->children) {
                auto* nameItem = new QStandardItem();
                nameItem->setText(QString::fromStdString(c->name));
                if (c->isDir)
                    nameItem->setIcon(QIcon::fromTheme("folder",
                        QIcon(":/res/icons/usbutils_64.png")));
                nameItem->setEditable(false);
                nameItem->setData(
                    QVariant::fromValue(reinterpret_cast<quintptr>(c.get())),
                    kRoleNode);
                if (c->isExternal())
                    nameItem->setToolTip(tr("Added or replaced — not part of "
                                            "the original image."));
                auto* typeItem = new QStandardItem(
                    c->isDir ? tr("Folder") : tr("File"));
                typeItem->setEditable(false);
                auto* sizeItem = new QStandardItem(
                    c->isDir ? tr("%n item(s)", "", int(c->children.size()))
                             : humanSize(c->size()));
                sizeItem->setEditable(false);
                parentItem->appendRow({nameItem, typeItem, sizeItem});
                if (c->isDir) build(c.get(), nameItem);
            }
        };

    auto* rootItem = new QStandardItem(QString::fromStdString(doc_->volumeId()));
    rootItem->setEditable(false);
    rootItem->setData(QVariant::fromValue(
        reinterpret_cast<quintptr>(doc_->root())), kRoleNode);
    model_->appendRow(rootItem);
    build(doc_->root(), rootItem);
    updateInfoLine();
}

const IsoNode* IsoExplorerDialog::nodeForIndex(const QModelIndex& index) const {
    if (!index.isValid()) return nullptr;
    const QVariant v = model_->data(index.siblingAtColumn(0), kRoleNode);
    if (!v.isValid()) return nullptr;
    return reinterpret_cast<const IsoNode*>(v.value<quintptr>());
}

QStandardItem* IsoExplorerDialog::itemForNode(const IsoNode* node) const {
    // walk the model looking for the node pointer
    std::function<QStandardItem*(QStandardItem*)> find =
        [&](QStandardItem* it) -> QStandardItem* {
        if (!it) return nullptr;
        if (reinterpret_cast<const IsoNode*>(
                it->data(kRoleNode).value<quintptr>()) == node)
            return it;
        for (int r = 0; r < it->rowCount(); ++r) {
            if (auto* hit = find(it->child(r))) return hit;
        }
        return nullptr;
    };
    for (int r = 0; r < model_->rowCount(); ++r)
        if (auto* hit = find(model_->item(r))) return hit;
    return nullptr;
}

void IsoExplorerDialog::updateInfoLine() {
    if (!doc_->isOpen()) {
        infoLine_->setText(tr("Open an image to explore or modify it. "
                              "Double-click a file to save it out."));
        btnNewFolder_->setEnabled(false);
        btnAddFile_->setEnabled(false);
        btnSave_->setEnabled(false);
        return;
    }
    size_t files = 0, dirs = 0;
    doc_->forEachNode([&](const IsoNode* n, int) {
        if (n == doc_->root()) return;
        if (n->isDir) ++dirs; else ++files;
    });
    infoLine_->setText(tr("%1 folder(s), %2 file(s) — changes are only "
                          "written when you save a copy.")
                          .arg(dirs).arg(files));
    btnNewFolder_->setEnabled(true);
    btnAddFile_->setEnabled(true);
    btnSave_->setEnabled(true);
}

void IsoExplorerDialog::setDirty(bool dirty) {
    dirty_ = dirty;
    setWindowTitle(dirty_ ? tr("USBUtils — ISO explorer *")
                          : tr("USBUtils — ISO explorer"));
}

// ---------------------------------------------------------------------------
// modifications
// ---------------------------------------------------------------------------

void IsoExplorerDialog::onVolumeIdChanged(const QString& text) {
    if (!doc_->isOpen()) return;
    doc_->setVolumeId(text.toStdString());
    setDirty(true);
    // refresh the root item label
    if (model_->rowCount() > 0)
        model_->item(0)->setText(text);
}

void IsoExplorerDialog::newFolder() {
    if (!doc_->isOpen()) return;
    const QModelIndex idx = tree_->currentIndex();
    const IsoNode* target = nodeForIndex(idx);
    if (!target || !target->isDir) {
        QMessageBox::information(this, tr("USBUtils"),
            tr("Select a folder first (the top entry is the image root)."));
        return;
    }
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("New folder"),
        tr("Folder name:"), QLineEdit::Normal, tr("NEW_FOLDER"), &ok)
        .trimmed();
    if (!ok || name.isEmpty()) return;

    IsoNode* created = doc_->addDir(const_cast<IsoNode*>(target),
                                    name.toStdString());
    if (!created) return;
    setDirty(true);
    rebuildTree();
    // reselect the new folder
    if (auto* it = itemForNode(created)) {
        tree_->setCurrentIndex(it->index());
        tree_->expand(it->index().parent());
    }
}

void IsoExplorerDialog::addFile() {
    if (!doc_->isOpen()) return;
    const QModelIndex idx = tree_->currentIndex();
    const IsoNode* target = nodeForIndex(idx);
    if (!target || !target->isDir) {
        QMessageBox::information(this, tr("USBUtils"),
            tr("Select a folder first (the top entry is the image root)."));
        return;
    }
    const QString file = QFileDialog::getOpenFileName(this, tr("Add a file"),
        QString(), tr("All files (*)"));
    if (file.isEmpty()) return;

    IsoNode* added = doc_->addFile(const_cast<IsoNode*>(target),
                                   file.toStdString());
    if (!added) {
        QMessageBox::warning(this, tr("USBUtils"),
            tr("Could not read \"%1\".").arg(file));
        return;
    }
    setDirty(true);
    rebuildTree();
    if (auto* it = itemForNode(added)) {
        tree_->scrollTo(it->index());
        tree_->setCurrentIndex(it->index());
    }
}

void IsoExplorerDialog::replaceContent() {
    if (!doc_->isOpen()) return;
    const QModelIndex idx = tree_->currentIndex();
    const IsoNode* target = nodeForIndex(idx);
    if (!target || target->isDir) return;

    const QString file = QFileDialog::getOpenFileName(this,
        tr("Replace the content of \"%1\"").arg(
            QString::fromStdString(target->name)),
        QString(), tr("All files (*)"));
    if (file.isEmpty()) return;

    if (doc_->replaceFile(const_cast<IsoNode*>(target), file.toStdString())) {
        setDirty(true);
        rebuildTree();
    } else {
        QMessageBox::warning(this, tr("USBUtils"),
            tr("Could not read \"%1\".").arg(file));
    }
}

void IsoExplorerDialog::renameSelected() {
    if (!doc_->isOpen()) return;
    const QModelIndex idx = tree_->currentIndex();
    const IsoNode* target = nodeForIndex(idx);
    if (!target || target == doc_->root()) return;

    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Rename"),
        tr("New name:"), QLineEdit::Normal,
        QString::fromStdString(target->name), &ok).trimmed();
    if (!ok || name.isEmpty()) return;

    if (doc_->renameNode(const_cast<IsoNode*>(target), name.toStdString())) {
        setDirty(true);
        rebuildTree();
    }
}

void IsoExplorerDialog::deleteSelected() {
    if (!doc_->isOpen()) return;
    const QModelIndex idx = tree_->currentIndex();
    const IsoNode* target = nodeForIndex(idx);
    if (!target || target == doc_->root()) return;

    // find parent
    const IsoNode* parent = doc_->root();
    std::function<const IsoNode*(const IsoNode*, const IsoNode*)> findParent =
        [&](const IsoNode* p, const IsoNode* n) -> const IsoNode* {
        for (const auto& c : p->children) {
            if (c.get() == n) return p;
            if (c->isDir) {
                if (auto* hit = findParent(c.get(), n)) return hit;
            }
        }
        return nullptr;
    };
    parent = findParent(doc_->root(), target);
    if (!parent) return;

    if (target->isDir && !target->children.empty()) {
        QMessageBox::information(this, tr("USBUtils"),
            tr("The folder is not empty. Delete its contents first."));
        return;
    }
    const auto ret = QMessageBox::question(this, tr("USBUtils"),
        tr("Delete \"%1\" from the image?")
            .arg(QString::fromStdString(target->name)),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (ret != QMessageBox::Yes) return;

    if (doc_->removeChild(const_cast<IsoNode*>(parent),
                          const_cast<IsoNode*>(target))) {
        setDirty(true);
        rebuildTree();
    }
}

void IsoExplorerDialog::extractSelected() {
    if (!doc_->isOpen()) return;
    const QModelIndex idx = tree_->currentIndex();
    const IsoNode* target = nodeForIndex(idx);
    if (!target || target->isDir) {
        QMessageBox::information(this, tr("USBUtils"),
            tr("Select a file to save out."));
        return;
    }
    const QString out = QFileDialog::getSaveFileName(this,
        tr("Save \"%1\" as...").arg(QString::fromStdString(target->name)),
        QString::fromStdString(target->name), tr("All files (*)"));
    if (out.isEmpty()) return;

    if (!doc_->extractTo(target, out.toStdString())) {
        QMessageBox::warning(this, tr("USBUtils"),
            tr("Could not save the file out."));
    } else {
        QMessageBox::information(this, tr("USBUtils"),
            tr("Saved %1 (%2).")
                .arg(QDir::toNativeSeparators(out))
                .arg(humanSize(target->size())));
    }
}

// ---------------------------------------------------------------------------
// save
// ---------------------------------------------------------------------------

void IsoExplorerDialog::saveImageAs() {
    if (!doc_->isOpen()) return;
    QString suggested = QFileInfo(QString::fromStdString(doc_->sourcePath()))
                            .absoluteDir().filePath("modified.iso");
    const QString out = QFileDialog::getSaveFileName(this,
        tr("Save a copy of the image"), suggested,
        tr("ISO images (*.iso);;All files (*)"));
    if (out.isEmpty()) return;

    // a long write — show the busy cursor at least (images are DVD-sized)
    setEnabled(false);
    QApplication::setOverrideCursor(Qt::WaitCursor);
    std::string err;
    const bool ok = doc_->saveAs(out.toStdString(), nullptr, &err);
    QApplication::restoreOverrideCursor();
    setEnabled(true);

    if (!ok) {
        QMessageBox::critical(this, tr("USBUtils"),
            tr("Saving failed: %1").arg(QString::fromStdString(err)));
        return;
    }
    setDirty(false);
    QMessageBox::information(this, tr("USBUtils"),
        tr("Saved the image to %1.").arg(QDir::toNativeSeparators(out)));
}

// ---------------------------------------------------------------------------
// context menu
// ---------------------------------------------------------------------------

void IsoExplorerDialog::onTreeContextMenu(const QPoint& pos) {
    if (!doc_->isOpen()) return;
    const QModelIndex idx = tree_->indexAt(pos);
    if (!idx.isValid()) return;
    tree_->setCurrentIndex(idx);
    const IsoNode* node = nodeForIndex(idx);

    QMenu m(this);
    if (node && node->isDir) {
        m.addAction(tr("New folder..."), this, &IsoExplorerDialog::newFolder);
        m.addAction(tr("Add file..."), this, &IsoExplorerDialog::addFile);
    } else {
        m.addAction(tr("Save this file out..."), this,
                    &IsoExplorerDialog::extractSelected);
        m.addAction(tr("Replace content with my file..."), this,
                    &IsoExplorerDialog::replaceContent);
    }
    if (node && node != doc_->root()) {
        m.addSeparator();
        m.addAction(tr("Rename..."), this, &IsoExplorerDialog::renameSelected);
        m.addAction(tr("Delete"), this, &IsoExplorerDialog::deleteSelected);
    }
    m.exec(tree_->viewport()->mapToGlobal(pos));
}

void IsoExplorerDialog::closeEvent(QCloseEvent* ev) {
    if (dirty_) {
        const auto ret = QMessageBox::question(this, tr("USBUtils"),
            tr("The image has unsaved changes. Close anyway?"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (ret != QMessageBox::Yes) { ev->ignore(); return; }
    }
    ev->accept();
}
