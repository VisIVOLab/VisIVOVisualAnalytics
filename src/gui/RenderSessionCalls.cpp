#include "RenderSessionCalls.h"

#include "app/BackendClient.h"
#include "app/OpenRenderSessions.h"

#include <QCoreApplication>
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

#include <functional>

using namespace Qt::Literals::StringLiterals;

namespace visivo {

QNetworkAccessManager *renderSessionManager()
{
    static QNetworkAccessManager *nam = new QNetworkAccessManager(QCoreApplication::instance());
    return nam;
}

namespace {

QNetworkRequest renderRequest(const QString &backendUrl, const QString &token,
                              const QString &sessionId, const QString &path)
{
    QUrl url(backendUrl);
    url.setPath(path);
    QNetworkRequest req(url);
    // The session header: create_session resolves the dataset in this client's
    // backend session, and every route checks the token.
    BackendClient::applySessionHeaders(req, backendUrl, token, sessionId);
    return req;
}

/// @p onAnswered, if any, runs once the backend answered (whatever it said).
void deleteAndForget(const QNetworkRequest &req, std::function<void()> onAnswered = {})
{
    qDebug().noquote() << "[remote-render] DELETE" << req.url().toString();
    QNetworkReply *reply = renderSessionManager()->deleteResource(req);
    QObject::connect(reply, &QNetworkReply::finished, reply, [reply, onAnswered]() {
        reply->deleteLater();
        // Any HTTP answer (404 too: the session was gone), except a refusal of
        // the token (401, 403), after which the session may still be there;
        // not a request that never reached the backend.
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (onAnswered && status > 0 && status != 401 && status != 403)
            onAnswered();
    });
}

} // namespace

QNetworkReply *postCreateRenderSession(const QString &backendUrl, const QString &token,
                                       const QString &sessionId, const QByteArray &body)
{
    QNetworkRequest req =
            renderRequest(backendUrl, token, sessionId, u"/v1/render/create_session"_s);
    req.setHeader(QNetworkRequest::ContentTypeHeader, u"application/json"_s);
    qDebug().noquote() << "[remote-render] POST" << req.url().toString()
                       << QString::fromUtf8(body);
    QNetworkReply *reply = renderSessionManager()->post(req, body);
    // Connected before the caller's handler, so it runs first; peek() leaves
    // the body to the caller. A session made is listed until it is given
    // back, so that quitting closes it (OpenRenderSessions.h).
    QObject::connect(reply, &QNetworkReply::finished, reply,
                     [reply, backendUrl, token, sessionId]() {
                         if (reply->error() != QNetworkReply::NoError
                             || reply->attribute(QNetworkRequest::HttpStatusCodeAttribute)
                                             .toInt()
                                     != 200)
                             return;
                         const QString sid =
                                 QJsonDocument::fromJson(reply->peek(reply->bytesAvailable()))
                                         .object()
                                         .value(u"session_id"_s)
                                         .toString();
                         OpenRenderSessions::instance().opened(backendUrl, token, sessionId, sid);
                     });
    return reply;
}

void deleteRenderSession(const QString &backendUrl, const QString &token,
                         const QString &sessionId, const QString &renderSessionId)
{
    if (renderSessionId.isEmpty())
        return;
    deleteAndForget(renderRequest(backendUrl, token, sessionId,
                                  u"/v1/render/"_s + renderSessionId),
                    [backendUrl, renderSessionId]() {
                        OpenRenderSessions::instance().closed(backendUrl, renderSessionId);
                    });
}

void cancelRenderQueuePlace(const QString &backendUrl, const QString &token,
                            const QString &sessionId, const QString &queueId)
{
    if (queueId.isEmpty())
        return;
    deleteAndForget(renderRequest(backendUrl, token, sessionId,
                                  u"/v1/render/queue/"_s + queueId));
}

} // namespace visivo
