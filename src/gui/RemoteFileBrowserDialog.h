#ifndef RemoteFileBrowserDialog_h
#define RemoteFileBrowserDialog_h

#include "app/BackendClient.h"
#include "app/SupportedFormats.h"

#include <QDialog>
#include <QFutureWatcher>
#include <QList>

#include <memory>
#include <vector>

class QComboBox;
class QLineEdit;
class QPushButton;
class QTextEdit;
class QTreeWidget;
class QTreeWidgetItem;
class QLabel;

class RemoteFileBrowserDialog : public QDialog
{
    Q_OBJECT

public:
    explicit RemoteFileBrowserDialog(const QString &backendUrl, const QString &backendToken = QString(),
                                     QWidget *parent = nullptr);
    ~RemoteFileBrowserDialog() override;

    QString selectedFilePath() const;

    /// The kinds of file the type drop-down offers (All files is always added),
    /// the one it starts on, and the name under which the user's last choice
    /// for this kind of open is remembered (empty: not remembered). Without a
    /// call the dialog offers every kind and starts on All supported files.
    void setFileTypes(const QList<visivo::FileType> &offered, visivo::FileType preferred,
                      const QString &rememberKey = QString());

    /// What the user is choosing a file for ("Open Catalogue"): the title,
    /// followed by the backend whose files are listed. "Open Dataset" by default.
    void setPurpose(const QString &purpose);

private:
    void loadPath(const QString &path);
    void populateEntries();
    void requestHeaderPreview(const QString &path);
    void clearDetails();
    void updateDetails(const BackendFileEntry *entry);
    void updateSelectionState();
    /// Can this row be opened: a file listed under the chosen type.
    bool isSelectable(const QTreeWidgetItem *item) const;
    /// Whether an entry is listed under the chosen type: folders always (bar
    /// hidden ones), files by name (SupportedFormats.h).
    bool isListed(const BackendFileEntry &entry) const;
    visivo::FileType currentFileType() const;
    QString humanReadableSize(qint64 size) const;
    void refreshEntriesView();

    std::unique_ptr<BackendClient> client;
    QString backendName;
    QString rememberKey;
    QLineEdit *pathEdit;
    QTreeWidget *entriesTree;
    QPushButton *openButton;
    QComboBox *typeCombo;
    QComboBox *sortCombo;
    QLabel *nameValue;
    QLabel *pathValue;
    QLabel *typeValue;
    QLabel *sizeValue;
    QLabel *modifiedValue;
    QTextEdit *headerView;
    QFutureWatcher<BackendFileHeaderResult> headerWatcher;
    QString selectedPath;
    QString currentDirectory;
    std::vector<BackendFileEntry> currentEntries;
};

#endif
