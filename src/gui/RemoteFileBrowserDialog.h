#ifndef RemoteFileBrowserDialog_h
#define RemoteFileBrowserDialog_h

#include "app/BackendClient.h"

#include <QDialog>
#include <QFutureWatcher>
#include <QStringList>

#include <memory>
#include <vector>

class QCheckBox;
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

    // Optional: restrict visible files to those whose names end with one of
    // the given suffixes (case-insensitive).  Pass an empty list to clear.
    // Also updates the filter checkbox label.
    void setExtensionFilter(const QStringList &suffixes,
                            const QString &label = QString());

private:
    void loadPath(const QString &path);
    void populateEntries();
    void requestHeaderPreview(const QString &path);
    void clearDetails();
    void updateDetails(const BackendFileEntry *entry);
    void updateSelectionState();
    QString humanReadableSize(qint64 size) const;
    void refreshEntriesView();

    std::unique_ptr<BackendClient> client;
    QLineEdit *pathEdit;
    QTreeWidget *entriesTree;
    QPushButton *openButton;
    QCheckBox *fitsOnlyCheck;
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
    QStringList m_extensionFilter;  // empty = use isFits; non-empty = filter by suffix
};

#endif
