#ifndef DiagnosticsManager_h
#define DiagnosticsManager_h

#include <QAbstractTableModel>
#include <QDateTime>
#include <QList>
#include <QObject>
#include <QString>
#include <QVector>

enum class DiagnosticsLevel
{
    Debug = 0,
    Info,
    Warning,
    Error,
};

enum class DiagnosticsCategory
{
    Scientific = 0,
    Client,
    Backend,
    Task,
    WCS,
    Remote,
    Rendering,
    Performance,
};

struct DiagnosticEntry
{
    QDateTime timestamp;
    DiagnosticsLevel level{ DiagnosticsLevel::Info };
    DiagnosticsCategory category{ DiagnosticsCategory::Client };
    QString source;
    QString message;
    QString datasetId;
    QString sessionId;
    QString operationTag;
};

class DiagnosticsManager : public QObject
{
    Q_OBJECT

public:
    static DiagnosticsManager *instance();

    void publish(const DiagnosticEntry &entry);
    void publish(DiagnosticsLevel level, DiagnosticsCategory category, const QString &source,
                 const QString &message, const QString &datasetId = QString(),
                 const QString &sessionId = QString(), const QString &operationTag = QString());
    void clear();

    const QVector<DiagnosticEntry> &entries() const;

    static QString levelToString(DiagnosticsLevel level);
    static QString categoryToString(DiagnosticsCategory category);

signals:
    void entryAboutToBeAppended(int row);
    void entryAppended();
    void entriesAboutToBeCleared();
    void entriesCleared();

private:
    explicit DiagnosticsManager(QObject *parent = nullptr);
    void appendEntryInternal(const DiagnosticEntry &entry);

    QVector<DiagnosticEntry> m_entries;
};

class DiagnosticsModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    enum Column
    {
        TimeColumn = 0,
        LevelColumn,
        CategoryColumn,
        SourceColumn,
        OperationColumn,
        ContextColumn,
        MessageColumn,
        ColumnCount,
    };

    explicit DiagnosticsModel(DiagnosticsManager *manager, QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;

    DiagnosticEntry entryAt(int row) const;

private:
    DiagnosticsManager *m_manager;
};

#endif
