#include "CopilotStatus.h"

CopilotStatus *CopilotStatus::instance()
{
    static CopilotStatus *s_instance = new CopilotStatus();
    return s_instance;
}

quint64 CopilotStatus::nextTicket()
{
    return ++m_lastTicket;
}

void CopilotStatus::report(QObject *reporter, const QString &url, const QString &problem,
                           quint64 ticket, const QString &active)
{
    if (!reporter || url.isEmpty())
        return;
    // Every panel on a backend asks it, and their replies can come back in any
    // order: one asked earlier must not undo what a later one already said.
    // The reporter still counts as a user of that backend.
    const bool stale = ticket < m_ticketByUrl.value(url);
    if (!stale)
        m_ticketByUrl.insert(url, ticket);
    // Watch each reporter once: a closed viewer must stop counting as a user
    // of its backend, or Settings would go on showing that backend's problem.
    if (!m_urlByReporter.contains(reporter))
        connect(reporter, &QObject::destroyed, this, [this, reporter]() {
            // The reporter is half destroyed by now: it must not hear the
            // changed() its own departure emits.
            disconnect(this, nullptr, reporter, nullptr);
            forget(reporter);
        });
    const bool moved = m_urlByReporter.value(reporter) != url;
    m_urlByReporter.insert(reporter, url);
    const QString before = m_problemByUrl.value(url);
    const QString activeBefore = m_activeByUrl.value(url);
    if (!stale) {
        if (problem.isEmpty()) {
            m_problemByUrl.remove(url);
            m_activeByUrl.remove(url);
        } else {
            m_problemByUrl.insert(url, problem);
            m_activeByUrl.insert(url, active);
        }
    }
    if (moved || before != m_problemByUrl.value(url) || activeBefore != m_activeByUrl.value(url))
        emit changed();
}

void CopilotStatus::forget(QObject *reporter)
{
    if (m_urlByReporter.remove(reporter) == 0)
        return;
    // Disconnected here as well as on destruction, so a later report() from
    // the same panel connects afresh instead of twice.
    disconnect(reporter, nullptr, this, nullptr);
    emit changed();
}

QString CopilotStatus::problem(const QString &url) const
{
    return m_problemByUrl.value(url);
}

QString CopilotStatus::active(const QString &url) const
{
    return m_activeByUrl.value(url);
}

QMap<QString, QString> CopilotStatus::problems() const
{
    QMap<QString, QString> out;
    for (const QString &url : m_urlByReporter) {
        const QString p = m_problemByUrl.value(url);
        if (!p.isEmpty())
            out.insert(url, p);
    }
    return out;
}
