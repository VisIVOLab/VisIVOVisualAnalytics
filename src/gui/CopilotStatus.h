#ifndef CopilotStatus_h
#define CopilotStatus_h

#include <QHash>
#include <QMap>
#include <QObject>
#include <QString>

/// What the desktop last heard about why the copilot cannot answer, per
/// backend (#75 follow-up).
///
/// The backend's /v1/assistant/status and /config carry a ``problem`` string
/// (a refused VISIVO_LLM_BASE_URL, a self-hosted server with no or several
/// models). Every viewer has its own copilot panel, but the problem belongs to
/// the backend session they share, and Settings ▸ Scientific Copilot shows it
/// too, without a backend of its own. So the panels report here, and both the
/// panels and the Settings dialog read from here and follow ``changed``.
class CopilotStatus : public QObject
{
    Q_OBJECT
public:
    static CopilotStatus *instance();

    /// A number to take just before asking a backend (status or config), and
    /// to hand back to report(): later requests get larger numbers.
    quint64 nextTicket();

    /// @p reporter (a copilot panel) heard @p problem from the backend at
    /// @p url, in reply to the request that took @p ticket; "" clears it.
    /// @p active names the configuration it is about, e.g. "server's
    /// configuration (off)", so Settings can say it as the panel does. A
    /// reply to a request older than one already reported for @p url is
    /// ignored. The reporter's backend is remembered until it reports another
    /// one or is destroyed, so a closed viewer's backend drops out of
    /// problems().
    void report(QObject *reporter, const QString &url, const QString &problem,
                quint64 ticket, const QString &active = QString());
    /// @p reporter no longer talks to a backend (it was disconnected).
    void forget(QObject *reporter);

    /// The problem last reported for the backend at @p url; "" if none.
    QString problem(const QString &url) const;
    /// The known problems of the backends some live panel talks to, by URL.
    QMap<QString, QString> problems() const;
    /// The configuration last reported with @p url's problem; "" if none.
    QString active(const QString &url) const;

signals:
    void changed();

private:
    CopilotStatus() = default;
    QHash<QString, QString> m_problemByUrl;
    QHash<QString, QString> m_activeByUrl;
    QHash<QObject *, QString> m_urlByReporter;
    QHash<QString, quint64> m_ticketByUrl;   ///< newest request reported per backend
    quint64 m_lastTicket{ 0 };
};

#endif  // CopilotStatus_h
