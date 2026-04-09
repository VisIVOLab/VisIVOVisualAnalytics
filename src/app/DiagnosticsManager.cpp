#include "DiagnosticsManager.h"

#include <QCoreApplication>
#include <QThread>

namespace {

QString contextSummary(const DiagnosticEntry &entry)
{
    QStringList parts;
    if (!entry.operationTag.isEmpty()) {
        parts << entry.operationTag;
    }
    if (!entry.datasetId.isEmpty()) {
        parts << QStringLiteral("dataset=%1").arg(entry.datasetId);
    }
    if (!entry.sessionId.isEmpty()) {
        parts << QStringLiteral("session=%1").arg(entry.sessionId);
    }
    return parts.join(QStringLiteral(" | "));
}

} // namespace

DiagnosticsManager *DiagnosticsManager::instance()
{
    static DiagnosticsManager *manager = new DiagnosticsManager(qApp);
    return manager;
}

DiagnosticsManager::DiagnosticsManager(QObject *parent) : QObject(parent)
{
    if (qApp) {
        this->moveToThread(qApp->thread());
    }
}

void DiagnosticsManager::publish(const DiagnosticEntry &entry)
{
    DiagnosticEntry normalized = entry;
    if (!normalized.timestamp.isValid()) {
        normalized.timestamp = QDateTime::currentDateTime();
    }

    if (QThread::currentThread() == this->thread()) {
        this->appendEntryInternal(normalized);
        return;
    }

    QMetaObject::invokeMethod(
            this, [this, normalized]() { this->appendEntryInternal(normalized); },
            Qt::QueuedConnection);
}

void DiagnosticsManager::publish(DiagnosticsLevel level, DiagnosticsCategory category,
                                 const QString &source, const QString &message,
                                 const QString &datasetId, const QString &sessionId,
                                 const QString &operationTag)
{
    this->publish(DiagnosticEntry { QDateTime::currentDateTime(), level, category, source, message,
                                    datasetId, sessionId, operationTag });
}

void DiagnosticsManager::clear()
{
    if (QThread::currentThread() == this->thread()) {
        emit entriesAboutToBeCleared();
        this->m_entries.clear();
        emit entriesCleared();
        return;
    }

    QMetaObject::invokeMethod(
            this,
            [this]() {
                emit entriesAboutToBeCleared();
                this->m_entries.clear();
                emit entriesCleared();
            },
            Qt::QueuedConnection);
}

const QVector<DiagnosticEntry> &DiagnosticsManager::entries() const
{
    return this->m_entries;
}

QString DiagnosticsManager::levelToString(DiagnosticsLevel level)
{
    switch (level) {
    case DiagnosticsLevel::Debug:
        return QStringLiteral("Debug");
    case DiagnosticsLevel::Info:
        return QStringLiteral("Info");
    case DiagnosticsLevel::Warning:
        return QStringLiteral("Warning");
    case DiagnosticsLevel::Error:
        return QStringLiteral("Error");
    }
    return QStringLiteral("Info");
}

QString DiagnosticsManager::categoryToString(DiagnosticsCategory category)
{
    switch (category) {
    case DiagnosticsCategory::Scientific:
        return QStringLiteral("Scientific");
    case DiagnosticsCategory::Client:
        return QStringLiteral("Client");
    case DiagnosticsCategory::Backend:
        return QStringLiteral("Backend");
    case DiagnosticsCategory::Task:
        return QStringLiteral("Task");
    case DiagnosticsCategory::WCS:
        return QStringLiteral("WCS");
    case DiagnosticsCategory::Remote:
        return QStringLiteral("Remote");
    case DiagnosticsCategory::Rendering:
        return QStringLiteral("Rendering");
    case DiagnosticsCategory::Performance:
        return QStringLiteral("Performance");
    }
    return QStringLiteral("Client");
}

void DiagnosticsManager::appendEntryInternal(const DiagnosticEntry &entry)
{
    const int row = this->m_entries.size();
    emit entryAboutToBeAppended(row);
    this->m_entries.push_back(entry);
    emit entryAppended();
}

DiagnosticsModel::DiagnosticsModel(DiagnosticsManager *manager, QObject *parent)
    : QAbstractTableModel(parent), m_manager(manager)
{
    QObject::connect(this->m_manager, &DiagnosticsManager::entryAboutToBeAppended, this,
                     [this](int row) { beginInsertRows(QModelIndex(), row, row); });
    QObject::connect(this->m_manager, &DiagnosticsManager::entryAppended, this,
                     [this]() { endInsertRows(); });
    QObject::connect(this->m_manager, &DiagnosticsManager::entriesAboutToBeCleared, this,
                     [this]() { beginResetModel(); });
    QObject::connect(this->m_manager, &DiagnosticsManager::entriesCleared, this,
                     [this]() { endResetModel(); });
}

int DiagnosticsModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : this->m_manager->entries().size();
}

int DiagnosticsModel::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant DiagnosticsModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid()) {
        return {};
    }
    const DiagnosticEntry entry = this->entryAt(index.row());
    if (role == Qt::DisplayRole) {
        switch (index.column()) {
        case TimeColumn:
            return entry.timestamp.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
        case LevelColumn:
            return DiagnosticsManager::levelToString(entry.level);
        case CategoryColumn:
            return DiagnosticsManager::categoryToString(entry.category);
        case SourceColumn:
            return entry.source;
        case OperationColumn:
            return entry.operationTag;
        case ContextColumn:
            return contextSummary(entry);
        case MessageColumn:
            return entry.message;
        default:
            break;
        }
    }
    if (role == Qt::ToolTipRole) {
        return QStringLiteral("%1\n%2").arg(contextSummary(entry), entry.message);
    }
    return {};
}

QVariant DiagnosticsModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
        return QAbstractTableModel::headerData(section, orientation, role);
    }
    switch (section) {
    case TimeColumn:
        return QStringLiteral("Time");
    case LevelColumn:
        return QStringLiteral("Level");
    case CategoryColumn:
        return QStringLiteral("Category");
    case SourceColumn:
        return QStringLiteral("Source");
    case OperationColumn:
        return QStringLiteral("Operation");
    case ContextColumn:
        return QStringLiteral("Context");
    case MessageColumn:
        return QStringLiteral("Message");
    default:
        break;
    }
    return {};
}

DiagnosticEntry DiagnosticsModel::entryAt(int row) const
{
    if (row < 0 || row >= this->m_manager->entries().size()) {
        return {};
    }
    return this->m_manager->entries().at(row);
}
