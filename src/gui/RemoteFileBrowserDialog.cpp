#include "RemoteFileBrowserDialog.h"

#include "BackendLocality.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFontDatabase>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSplitter>
#include <QStyle>
#include <QTextEdit>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>
#include <QtConcurrentRun>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace {
constexpr int PathRole = Qt::UserRole + 1;
constexpr int TypeRole = Qt::UserRole + 2;
constexpr int SizeRole = Qt::UserRole + 3;
constexpr int ModifiedRole = Qt::UserRole + 4;
constexpr int IsFitsRole = Qt::UserRole + 5;
QString g_lastVisitedRemoteDirectory;

class RemoteFileTreeItem : public QTreeWidgetItem
{
public:
    using QTreeWidgetItem::QTreeWidgetItem;

    bool operator<(const QTreeWidgetItem &other) const override
    {
        const auto thisType = this->data(0, TypeRole).toString();
        const auto otherType = other.data(0, TypeRole).toString();
        const bool thisDir = thisType == u"directory"_s;
        const bool otherDir = otherType == u"directory"_s;
        if (thisDir != otherDir) {
            return thisDir;
        }

        const int column = this->treeWidget() ? this->treeWidget()->sortColumn() : 0;
        switch (column) {
        case 1:
            return this->data(0, SizeRole).toLongLong() < other.data(0, SizeRole).toLongLong();
        case 2:
            return this->data(0, ModifiedRole).toString() < other.data(0, ModifiedRole).toString();
        case 0:
        default:
            return this->text(0).localeAwareCompare(other.text(0)) < 0;
        }
    }
};
}

RemoteFileBrowserDialog::RemoteFileBrowserDialog(const QString &backendUrl,
                                                 const QString &backendToken,
                                                 QWidget *parent)
    : QDialog(parent),
      client(std::make_unique<BackendClient>(backendUrl, backendToken)),
      backendName(visivo::backendDisplayName(backendUrl)),
      pathEdit(new QLineEdit(this)),
      entriesTree(new QTreeWidget(this)),
      openButton(new QPushButton(u"Open"_s, this)),
      typeCombo(new QComboBox(this)),   // a VIEW filter: the backend classifies by content
      sortCombo(new QComboBox(this)),
      nameValue(new QLabel(this)),
      pathValue(new QLabel(this)),
      typeValue(new QLabel(this)),
      sizeValue(new QLabel(this)),
      modifiedValue(new QLabel(this)),
      previewCaption(new QLabel(u"Preview"_s, this)),
      headerView(new QTextEdit(this))
{
    this->setPurpose(u"Open Dataset"_s);
    this->resize(980, 620);

    auto *layout = new QVBoxLayout(this);
    auto *pathRow = new QHBoxLayout;
    auto *homeButton = new QPushButton(u"Home"_s, this);
    auto *upButton = new QPushButton(u"Up"_s, this);
    auto *refreshButton = new QPushButton(u"Refresh"_s, this);
    auto *goButton = new QPushButton(u"Go"_s, this);

    pathRow->addWidget(new QLabel(u"Path"_s, this));
    pathRow->addWidget(this->pathEdit, 1);
    pathRow->addWidget(goButton);
    pathRow->addWidget(homeButton);
    pathRow->addWidget(upButton);
    pathRow->addWidget(refreshButton);
    layout->addLayout(pathRow);

    auto *optionsRow = new QHBoxLayout;
    this->sortCombo->addItem(u"Name"_s);
    this->sortCombo->addItem(u"Size"_s);
    this->sortCombo->addItem(u"Modified"_s);
    optionsRow->addStretch(1);
    optionsRow->addWidget(new QLabel(u"Sort by"_s, this));
    optionsRow->addWidget(this->sortCombo);
    layout->addLayout(optionsRow);

    auto *splitter = new QSplitter(this);
    splitter->setHandleWidth(10);
    splitter->setChildrenCollapsible(false);
    layout->addWidget(splitter, 1);

    auto *browserPane = new QWidget(splitter);
    auto *browserLayout = new QVBoxLayout(browserPane);
    browserLayout->setContentsMargins(0, 0, 6, 0);
    this->entriesTree->setColumnCount(3);
    this->entriesTree->setHeaderLabels({ u"Name"_s, u"Size"_s, u"Modified"_s });
    this->entriesTree->setAlternatingRowColors(true);
    this->entriesTree->setRootIsDecorated(false);
    this->entriesTree->setSortingEnabled(true);
    this->entriesTree->sortByColumn(0, Qt::AscendingOrder);
    this->entriesTree->header()->setStretchLastSection(false);
    this->entriesTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    this->entriesTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    this->entriesTree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    browserLayout->addWidget(this->entriesTree);

    auto *detailsPane = new QWidget(splitter);
    detailsPane->setMaximumWidth(420);
    detailsPane->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    auto *detailsLayout = new QVBoxLayout(detailsPane);
    detailsLayout->setContentsMargins(6, 0, 0, 0);
    auto *detailsTitle = new QLabel(u"Details"_s, detailsPane);
    auto *detailsForm = new QFormLayout;
    detailsForm->setContentsMargins(0, 0, 0, 0);
    detailsForm->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    detailsForm->setLabelAlignment(Qt::AlignLeft | Qt::AlignTop);
    detailsForm->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
    this->nameValue->setWordWrap(true);
    this->pathValue->setWordWrap(true);
    this->nameValue->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    this->pathValue->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    this->nameValue->setMinimumWidth(180);
    this->pathValue->setMinimumWidth(180);
    this->pathValue->setTextInteractionFlags(Qt::TextSelectableByMouse);
    this->headerView->setReadOnly(true);
    this->headerView->setLineWrapMode(QTextEdit::NoWrap);
    // Monospace, for header cards and table columns to line up. Through the
    // style sheet as well: the application's sheet would override setFont.
    this->headerView->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    this->headerView->setStyleSheet(
            u"QTextEdit { font-family: 'JetBrains Mono', 'SF Mono', Menlo, Consolas, monospace; }"_s);
    this->previewCaption->setWordWrap(true);

    detailsForm->addRow(u"Name"_s, this->nameValue);
    detailsForm->addRow(u"Path"_s, this->pathValue);
    detailsForm->addRow(u"Type"_s, this->typeValue);
    detailsForm->addRow(u"Size"_s, this->sizeValue);
    detailsForm->addRow(u"Modified"_s, this->modifiedValue);
    detailsLayout->addWidget(detailsTitle);
    detailsLayout->addLayout(detailsForm);
    detailsLayout->addWidget(this->previewCaption);
    detailsLayout->addWidget(this->headerView, 1);
    splitter->setStretchFactor(0, 5);
    splitter->setStretchFactor(1, 2);
    splitter->setSizes({ 640, 320 });

    // The type drop-down sits beside Cancel and Open, where the native open
    // dialogs put theirs: it is the last thing looked at before choosing.
    auto *bottomRow = new QHBoxLayout;
    auto *typeLabel = new QLabel(u"File type"_s, this);
    typeLabel->setBuddy(this->typeCombo);
    this->typeCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    bottomRow->addWidget(typeLabel);
    bottomRow->addWidget(this->typeCombo);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    buttons->addButton(this->openButton, QDialogButtonBox::AcceptRole);
    bottomRow->addWidget(buttons, 1);
    layout->addLayout(bottomRow);

    this->openButton->setEnabled(false);
    this->clearDetails();

    QObject::connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    QObject::connect(this->openButton, &QPushButton::clicked, this, [this]() {
        if (!this->selectedPath.isEmpty()) {
            this->accept();
        }
    });
    QObject::connect(goButton, &QPushButton::clicked, this,
                     [this]() { this->loadPath(this->pathEdit->text()); });
    QObject::connect(this->pathEdit, &QLineEdit::returnPressed, this,
                     [this]() { this->loadPath(this->pathEdit->text()); });
    QObject::connect(refreshButton, &QPushButton::clicked, this,
                     [this]() { this->loadPath(this->currentDirectory); });
    QObject::connect(homeButton, &QPushButton::clicked, this, [this]() { this->loadPath({}); });
    QObject::connect(upButton, &QPushButton::clicked, this, [this]() {
        const QDir dir(this->pathEdit->text());
        this->loadPath(dir.absoluteFilePath(u".."_s));
    });
    QObject::connect(this->typeCombo, &QComboBox::currentIndexChanged, this, [this](int) {
        // Only the user's choices land here (setFileTypes blocks the
        // combo's signals), so only those are remembered.
        if (!this->rememberKey.isEmpty()) {
            QSettings settings;
            settings.setValue(u"FileBrowser/lastType/%1"_s.arg(this->rememberKey),
                              QString(visivo::fileTypeInfo(this->currentFileType()).key));
        }
        this->refreshEntriesView();
    });
    QObject::connect(this->sortCombo, &QComboBox::currentIndexChanged, this,
                     [this](int) { this->refreshEntriesView(); });
    QObject::connect(this->entriesTree, &QTreeWidget::currentItemChanged, this,
                     [this](QTreeWidgetItem *current) {
                         this->updateSelectionState();
                         if (!current) {
                             this->clearDetails();
                             return;
                         }

                         BackendFileEntry entry;
                         entry.name = current->text(0);
                         entry.path = current->data(0, PathRole).toString();
                         entry.type = current->data(0, TypeRole).toString();
                         entry.size = current->data(0, SizeRole).toLongLong();
                         entry.modifiedTime = current->data(0, ModifiedRole).toString();
                         entry.isFits = current->data(0, IsFitsRole).toBool();
                         this->updateDetails(&entry);
                     });
    QObject::connect(this->entriesTree, &QTreeWidget::itemDoubleClicked, this,
                     [this](QTreeWidgetItem *item) {
                         if (!item) {
                             return;
                         }

                         const QString itemType = item->data(0, TypeRole).toString();
                         const QString itemPath = item->data(0, PathRole).toString();
                         if (itemType == u"directory"_s) {
                             this->loadPath(itemPath);
                             return;
                         }

                         if (this->isSelectable(item)) {
                             this->selectedPath = itemPath;
                             this->accept();
                         }
                     });
    QObject::connect(&this->previewWatcher, &QFutureWatcher<visivo::FilePreview>::finished, this,
                     [this]() {
                         this->previewInFlight = false;
                         const bool current = this->previewWatcher.property("serial").toULongLong()
                                 == this->previewSerial;
                         // The latest selection waited behind this one: its turn.
                         if (this->pendingPreview) {
                             const auto [entry, serial] = *this->pendingPreview;
                             this->pendingPreview.reset();
                             if (serial == this->previewSerial) {
                                 this->startPreview(entry, serial);
                                 return;
                             }
                         }
                         // A reply for an earlier selection: dropped.
                         if (current)
                             this->showPreview(this->previewWatcher.result());
                     });

    this->pathEdit->setClearButtonEnabled(true);
    this->pathEdit->setPlaceholderText(u"Enter a path on the backend"_s);
    this->setFileTypes(visivo::openableFileTypes(), visivo::FileType::AllSupported);
    this->loadPath(g_lastVisitedRemoteDirectory);
}

RemoteFileBrowserDialog::~RemoteFileBrowserDialog() = default;

QString RemoteFileBrowserDialog::selectedFilePath() const
{
    return this->selectedPath;
}

void RemoteFileBrowserDialog::loadPath(const QString &path)
{
    const auto result = this->client->listFiles(path);
    if (!result.valid) {
        QMessageBox::warning(this, u"Cannot list folder"_s,
                             result.error.isEmpty() ? u"Could not list the folder."_s
                                                    : result.error);
        return;
    }

    this->selectedPath.clear();
    this->currentDirectory = result.currentPath;
    g_lastVisitedRemoteDirectory = result.currentPath;
    this->currentEntries = result.entries;
    this->pathEdit->setText(result.currentPath);
    this->refreshEntriesView();
}

void RemoteFileBrowserDialog::populateEntries()
{
    this->entriesTree->clear();

    const QIcon folderIcon = this->style()->standardIcon(QStyle::SP_DirIcon);
    const QIcon fitsIcon = this->style()->standardIcon(QStyle::SP_FileIcon);
    const QIcon fileIcon = this->style()->standardIcon(QStyle::SP_FileLinkIcon);

    std::vector<BackendFileEntry> visibleEntries;
    visibleEntries.reserve(this->currentEntries.size());
    for (const auto &entry : this->currentEntries) {
        if (this->isListed(entry))
            visibleEntries.push_back(entry);
    }

    const int sortIndex = this->sortCombo->currentIndex();
    std::stable_sort(visibleEntries.begin(), visibleEntries.end(),
                     [sortIndex](const BackendFileEntry &lhs, const BackendFileEntry &rhs) {
                         const bool lhsDir = lhs.type == u"directory"_s;
                         const bool rhsDir = rhs.type == u"directory"_s;
                         if (lhsDir != rhsDir) {
                             return lhsDir;
                         }

                         switch (sortIndex) {
                         case 1:
                             if (lhs.size != rhs.size) {
                                 return lhs.size < rhs.size;
                             }
                             break;
                         case 2:
                             if (lhs.modifiedTime != rhs.modifiedTime) {
                                 return lhs.modifiedTime < rhs.modifiedTime;
                             }
                             break;
                         case 0:
                         default:
                             break;
                         }

                         return lhs.name.localeAwareCompare(rhs.name) < 0;
                     });

    for (const auto &entry : visibleEntries) {
        auto *item = new RemoteFileTreeItem(this->entriesTree);
        item->setText(0, entry.name);
        item->setText(1, entry.type == u"directory"_s ? QString() : this->humanReadableSize(entry.size));
        item->setText(2, entry.modifiedTime);
        item->setData(0, PathRole, entry.path);
        item->setData(0, TypeRole, entry.type);
        item->setData(0, SizeRole, entry.size);
        item->setData(0, ModifiedRole, entry.modifiedTime);
        item->setData(0, IsFitsRole, entry.isFits);
        item->setIcon(0, entry.type == u"directory"_s ? folderIcon : (entry.isFits ? fitsIcon : fileIcon));
    }

    this->entriesTree->sortItems(this->sortCombo->currentIndex(), Qt::AscendingOrder);
}

void RemoteFileBrowserDialog::requestPreview(const BackendFileEntry &entry)
{
    this->previewCaption->setText(u"Preview"_s);
    this->headerView->setPlainText(u"Loading preview…"_s);
    const quint64 serial = ++this->previewSerial;
    // One request out at a time: arrowing down a slow remote listing must not
    // queue a request per row ahead of the one wanted (they block a pool
    // thread each). The latest selection waits; earlier waiting ones go.
    if (this->previewInFlight) {
        this->pendingPreview = std::make_pair(entry, serial);
        return;
    }
    this->startPreview(entry, serial);
}

void RemoteFileBrowserDialog::startPreview(const BackendFileEntry &entry, quint64 serial)
{
    this->previewInFlight = true;
    this->previewWatcher.setProperty("serial", QVariant::fromValue(serial));

    // A client of its own in the worker, not the dialog's: the dialog can be
    // closed while the request is out, and the worker must not outlive what it
    // uses.
    const QString url = this->client->baseUrl();
    const QString token = this->client->token();
    const QString path = entry.path;
    const bool isFits = entry.isFits;
    const bool isFolder = entry.type == u"directory"_s;
    this->previewWatcher.setFuture(QtConcurrent::run([url, token, path, isFits, isFolder]() {
        const BackendClient worker(url, token);
        visivo::FilePreview preview = worker.filePreview(path);
        switch (visivo::previewFallback(preview.routeMissing, isFolder, isFits)) {
        case visivo::PreviewFallback::None:
            return preview;
        case visivo::PreviewFallback::Folder:
            return visivo::folderPlaceholder(path);
        case visivo::PreviewFallback::Unsupported:
            return visivo::unsupportedOnOldBackend(path);
        case visivo::PreviewFallback::FitsHeader:
            break;
        }
        // An older backend: it can still show a FITS header.
        const BackendFileHeaderResult header = worker.fileHeader(path);
        if (!header.valid) {
            visivo::FilePreview failed;
            failed.path = path;
            failed.error = header.error.isEmpty() ? u"Could not load the FITS header."_s
                                                  : header.error;
            return failed;
        }
        return visivo::previewFromHeaderCards(path, header.cards);
    }));
}

void RemoteFileBrowserDialog::showPreview(const visivo::FilePreview &preview)
{
    if (!preview.valid) {
        this->previewCaption->setText(u"Preview"_s);
        this->headerView->setPlainText(preview.error.isEmpty() ? u"No preview available."_s
                                                               : preview.error);
        return;
    }
    this->previewCaption->setText(visivo::filePreviewCaption(preview));
    this->headerView->setPlainText(visivo::filePreviewBody(preview));
}

void RemoteFileBrowserDialog::clearDetails()
{
    this->nameValue->clear();
    this->pathValue->clear();
    this->typeValue->clear();
    this->sizeValue->clear();
    this->modifiedValue->clear();
    // Whatever preview is still out describes a selection that is gone.
    ++this->previewSerial;
    this->previewCaption->setText(u"Preview"_s);
    this->headerView->setPlainText(u"Select a file to preview its details."_s);
}

void RemoteFileBrowserDialog::updateDetails(const BackendFileEntry *entry)
{
    if (!entry) {
        this->clearDetails();
        return;
    }

    this->nameValue->setText(entry->name);
    this->pathValue->setText(entry->path);
    this->typeValue->setText(entry->type == u"directory"_s ? u"Directory"_s
                                                            : (entry->isFits ? u"FITS file"_s
                                                                             : u"File"_s));
    this->sizeValue->setText(entry->type == u"directory"_s ? u"_"_s : this->humanReadableSize(entry->size));
    this->modifiedValue->setText(entry->modifiedTime);

    // Folders too: the backend lists what is in them.
    this->requestPreview(*entry);
}

bool RemoteFileBrowserDialog::isSelectable(const QTreeWidgetItem *item) const
{
    if (!item || item->data(0, TypeRole).toString() != u"file"_s)
        return false;
    // Whatever file is listed can be picked. Enabling Open for only some of
    // the files on show meant a catalogue, a .speck or a VisIVO Binary Table
    // could be seen in the list and not opened, with no explanation: the
    // button simply stayed grey. Under All files that is any file, and the
    // backend classifies it afterwards and says so when it cannot.
    return visivo::fileTypeMatches(item->text(0), this->currentFileType());
}

bool RemoteFileBrowserDialog::isListed(const BackendFileEntry &entry) const
{
    const visivo::FileType type = this->currentFileType();
    // Folders are always there to walk into; hidden ones (.git, .cache) only
    // under All files, as the native dialogs do.
    if (entry.type == u"directory"_s)
        return type == visivo::FileType::AllFiles || !visivo::isHiddenFileName(entry.name);
    return visivo::fileTypeMatches(entry.name, type);
}

visivo::FileType RemoteFileBrowserDialog::currentFileType() const
{
    const QVariant data = this->typeCombo->currentData();
    return data.isValid() ? static_cast<visivo::FileType>(data.toInt())
                          : visivo::FileType::AllFiles;
}

void RemoteFileBrowserDialog::updateSelectionState()
{
    const auto *item = this->entriesTree->currentItem();
    const bool selectable = this->isSelectable(item);
    this->selectedPath = selectable ? item->data(0, PathRole).toString() : QString();
    this->openButton->setEnabled(selectable);
}

void RemoteFileBrowserDialog::setFileTypes(const QList<visivo::FileType> &offered,
                                           visivo::FileType preferred,
                                           const QString &rememberKey)
{
    this->rememberKey = rememberKey;
    QString remembered;
    if (!rememberKey.isEmpty()) {
        QSettings settings;
        remembered = settings.value(u"FileBrowser/lastType/%1"_s.arg(rememberKey)).toString();
    }
    const QList<visivo::FileType> types = visivo::offeredFileTypes(offered);
    const visivo::FileType initial = visivo::initialFileType(types, preferred, remembered);

    {
        const QSignalBlocker blocker(this->typeCombo);
        this->typeCombo->clear();
        for (const visivo::FileType type : types) {
            this->typeCombo->addItem(visivo::fileTypeDisplayLabel(type), static_cast<int>(type));
            // The catch-all's patterns are too many for the drop-down itself.
            const QStringList suffixes = visivo::fileTypeSuffixes(type);
            if (!suffixes.isEmpty()) {
                this->typeCombo->setItemData(this->typeCombo->count() - 1,
                                             suffixes.join(u' '), Qt::ToolTipRole);
            }
        }
        this->typeCombo->setCurrentIndex(this->typeCombo->findData(static_cast<int>(initial)));
    }
    this->refreshEntriesView();
}

void RemoteFileBrowserDialog::setMainOpenFileTypes()
{
    this->setFileTypes(visivo::openableFileTypes(), visivo::FileType::AllSupported);
}

void RemoteFileBrowserDialog::setPurpose(const QString &purpose)
{
    this->setWindowTitle(u"%1 — Backend: %2"_s.arg(purpose, this->backendName));
}

void RemoteFileBrowserDialog::refreshEntriesView()
{
    this->populateEntries();
    // QTreeWidget::clear() drops the current item without currentItemChanged,
    // so the details pane would keep describing a file no longer selected,
    // and Open would stay enabled for it.
    this->clearDetails();
    this->updateSelectionState();
}

QString RemoteFileBrowserDialog::humanReadableSize(qint64 size) const
{
    static const QStringList units = { u"B"_s, u"KB"_s, u"MB"_s, u"GB"_s, u"TB"_s };
    double value = static_cast<double>(size);
    int unitIndex = 0;
    while (value >= 1024.0 && unitIndex < units.size() - 1) {
        value /= 1024.0;
        ++unitIndex;
    }

    return QStringLiteral("%1 %2").arg(value, 0, 'f', unitIndex == 0 ? 0 : 1).arg(units[unitIndex]);
}
