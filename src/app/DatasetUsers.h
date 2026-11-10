#ifndef DATASETUSERS_H
#define DATASETUSERS_H

// Which windows use a backend dataset (issue #165 follow-up).
//
// A window that opens a dataset on the backend holds it until it closes; the
// last one to close tells the backend (DELETE /v1/datasets/{id}), so that a
// catalogue's table or a VBT's memory map is not kept for a window that is
// gone. A dataset is named by its backend, session and id: ids restart per
// session (samp-client-traps), and two backends can hand out the same one.
//
// Widget-free and thread-safe, so the counting is testable (TestVbtByteOrder).

#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QString>

namespace visivo {

class DatasetUsers
{
public:
    static DatasetUsers &instance()
    {
        static DatasetUsers users;
        return users;
    }

    /// A window now uses the dataset.
    void acquire(const QString &backendUrl, const QString &sessionId, const QString &datasetId)
    {
        if (datasetId.isEmpty())
            return;
        QMutexLocker lock(&m_mutex);
        ++m_count[key(backendUrl, sessionId, datasetId)];
    }

    /// A window no longer uses it. True when it was the last: the caller
    /// closes the dataset on the backend.
    bool release(const QString &backendUrl, const QString &sessionId, const QString &datasetId)
    {
        if (datasetId.isEmpty())
            return false;
        QMutexLocker lock(&m_mutex);
        const QString k = key(backendUrl, sessionId, datasetId);
        auto it = m_count.find(k);
        if (it == m_count.end())
            return false;   // never acquired here: not ours to close
        if (--it.value() > 0)
            return false;
        m_count.erase(it);
        return true;
    }

    int users(const QString &backendUrl, const QString &sessionId, const QString &datasetId) const
    {
        QMutexLocker lock(&m_mutex);
        return m_count.value(key(backendUrl, sessionId, datasetId));
    }

private:
    static QString key(const QString &url, const QString &session, const QString &id)
    {
        return url + QLatin1Char('\n') + session + QLatin1Char('\n') + id;
    }

    mutable QMutex m_mutex;
    QHash<QString, int> m_count;
};

} // namespace visivo

#endif // DATASETUSERS_H
