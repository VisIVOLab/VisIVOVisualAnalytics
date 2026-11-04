#pragma once

// RemotePointView.h
// A point table drawn on the backend (issue #83): the render session's life
// (create, connect, release) and the state sent to it, for a viewer that keeps
// its own controls. The cube viewer does the same for cubes inline; point
// viewers share this.
//
// State is sent as named commands, each only when its content changes: some
// (the coordinate fields, the filters) make the backend rebuild every point of
// a table that can hold hundreds of millions.

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariantMap>

class QJsonObject;
class QNetworkAccessManager;
class QNetworkReply;
class RemoteRenderWidget;

class RemotePointView : public QObject
{
    Q_OBJECT

public:
    RemotePointView(const QString &backendUrl, const QString &token, const QString &sessionId,
                    const QString &datasetId, RemoteRenderWidget *widget,
                    QObject *parent = nullptr);
    ~RemotePointView() override;

    /// Creates the session and connects the widget; started() or failed().
    void start();
    /// Disconnects and deletes the session.
    void stop();
    bool isActive() const { return !m_renderSessionId.isEmpty(); }
    /// The backend's render session, empty before it starts.
    QString renderSessionId() const { return m_renderSessionId; }

    /// The latest value of a state command (keyed by its "type"). Sent now
    /// when the view is live and it differs from what was sent; otherwise
    /// kept, and sent when the view goes live.
    void setState(const QVariantMap &command);
    /// A one-off command (a camera reset): sent only when live.
    void send(const QVariantMap &command);

signals:
    void started();
    /// The colour field's range over the whole table, as the backend colours
    /// it (after the session starts and after each colour change).
    void colourRange(double lo, double hi);
    void failed(const QString &reason);
    void liveChanged(bool live);

private:
    /// create_session answered for the generation asked.
    void created(QNetworkReply *reply, const QJsonObject &doc);
    void flush();
    void fetchLevels();

    const QString m_backendUrl;
    const QString m_token;
    const QString m_sessionId;
    const QString m_datasetId;
    QPointer<RemoteRenderWidget> m_widget;
    QNetworkAccessManager *m_nam{ nullptr };
    QString m_renderSessionId;
    quint64 m_generation{ 0 };
    bool m_live{ false };
    QHash<QString, QVariantMap> m_state;   ///< latest per type
    QHash<QString, QVariantMap> m_sent;    ///< last sent per type
};
