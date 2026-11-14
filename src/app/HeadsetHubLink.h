#ifndef HEADSETHUBLINK_H
#define HEADSETHUBLINK_H

// HeadsetHubLink — the desktop's end of a backend session's headset hub
// (issue #238, backend #236), on the GUI thread's event loop.
//
// One link per backend session, shared by every window of that session
// (forSession): the headsets and the activity are the session's, not a
// window's, so two cube windows neither poll twice nor list different things.
// It lives while a window uses it (addUser) and goes with the last one.
//
// It follows two things:
// - presence, GET /v1/vr/headset every few seconds (and on refreshPresence()),
//   announced by headsetsChanged() only when what is connected changed;
// - activity, a long poll of GET /v1/vr/activity (as SharedSelectionLink
//   follows the selection hub), kept in a VrHub::ActivityLog and announced by
//   activityChanged().
// A backend without the hub (FastAPI's bare 404) stops both for good, quietly:
// the windows then offer nothing headset-related.
//
// The requests carry the session's token and id in their headers
// (BackendClient::applySessionHeaders); nothing here logs either.

#include "VrHeadsetHub.h"

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QTimer>

class QNetworkAccessManager;
class QNetworkReply;

class HeadsetHubLink : public QObject
{
    Q_OBJECT

public:
    /// The link of the session @p session on @p baseUrl, made on first use.
    /// A token given later replaces an empty or older one.
    static HeadsetHubLink *forSession(const QString &baseUrl, const QString &token,
                                      const QString &session);

    /// @p user follows the link; the link stops and goes when the last user is
    /// removed or destroyed.
    void addUser(QObject *user);
    void removeUser(QObject *user);

    QString baseUrl() const { return m_baseUrl; }
    QString token() const { return m_token; }
    QString session() const { return m_session; }
    bool running() const { return m_running; }
    /// False once the backend said it has no hub.
    bool supported() const { return !m_unsupported; }
    const QList<VrHub::Headset> &headsets() const { return m_headsets; }
    const VrHub::ActivityLog &activity() const { return m_log; }

    /// Ask for presence now (after a show, say) rather than at the next tick.
    void refreshPresence();

    /// How often presence is read (ms); the hub forgets a page after 45 s.
    static constexpr int kPresenceIntervalMs = 5000;
    /// The long poll's wait (s; the hub caps it at 25).
    static constexpr int kWaitS = 20;

signals:
    void headsetsChanged();
    /// New events were listed (the log changed).
    void activityChanged();
    /// The backend has no hub: nothing more will come.
    void stopped(const QString &why);

private:
    HeadsetHubLink(const QString &baseUrl, const QString &token, const QString &session);
    ~HeadsetHubLink() override;
    static QHash<QString, QPointer<HeadsetHubLink>> &registry();
    static QString keyOf(const QString &baseUrl, const QString &session);

    void start();
    void stop();
    void pollActivity();
    void onActivity(QNetworkReply *reply);
    void retryActivityLater();
    void onPresence(QNetworkReply *reply);
    void markUnsupported();
    QNetworkReply *get(const QUrl &url, int transferTimeoutMs);

    QNetworkAccessManager *m_nam{ nullptr };
    QString m_baseUrl, m_token, m_session;
    QSet<QObject *> m_users;
    bool m_running{ false };
    bool m_unsupported{ false };
    quint64 m_generation{ 0 };
    // Activity
    VrHub::ActivityLog m_log;
    qint64 m_after{ 0 };
    int m_epoch{ 0 };
    QString m_instance;   ///< the backend process the seqs belong to
    int m_fails{ 0 };
    QPointer<QNetworkReply> m_activityReply;
    QTimer m_activityRetry;
    // Presence
    QList<VrHub::Headset> m_headsets;
    QString m_presenceKey;   ///< what was announced last, to tell a change
    QPointer<QNetworkReply> m_presenceReply;
    QTimer m_presenceTimer;
};

#endif // HEADSETHUBLINK_H
