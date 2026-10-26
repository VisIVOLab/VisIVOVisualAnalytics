#ifndef UPDATE_NOTIFIER_H
#define UPDATE_NOTIFIER_H
// UpdateNotifier.h
// Tells the user that a newer release is out. One asynchronous request to the
// GitHub Releases API, at most once a day at startup (Settings ▸ "Check for new
// versions at startup") or on demand from Help ▸ Check for Updates….
//
// The automatic check is quiet by design: no network, a rate limit or an
// unparseable reply leaves no trace, and a release the user chose to skip is
// not offered again. The manual check always answers, including "you are up to
// date" and the reason a check failed.

#include <QObject>
#include <QPointer>

class QNetworkAccessManager;
class QNetworkReply;
class QWidget;
class Settings;

class UpdateNotifier : public QObject
{
    Q_OBJECT

public:
    /// @param window the main window the notices are shown over. Never null.
    UpdateNotifier(Settings *settings, QWidget *window);

    /// Schedule the startup check if it is enabled and a day has passed since
    /// the last one. Call once, after the main window is shown.
    void scheduleStartupCheck();

    /// Ask GitHub now. @p manual makes the outcome visible whatever it is.
    void check(bool manual);

private:
    void onFinished(QNetworkReply *reply, bool manual);
    void showNewVersion(const QString &latest, const QString &current, const QUrl &page,
                        bool manual);

    QPointer<Settings> m_settings;
    QPointer<QWidget> m_window;
    QNetworkAccessManager *m_network{ nullptr };
    QPointer<QNetworkReply> m_pending;   ///< the request in flight, if any
    bool m_pendingManual{ false };       ///< a manual request joined it
};

#endif   // UPDATE_NOTIFIER_H
