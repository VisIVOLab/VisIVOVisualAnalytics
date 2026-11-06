#include "RenderSessionCalls.h"

#include "app/BackendClient.h"

#include <QCoreApplication>
#include <QDebug>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

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

void deleteAndForget(const QNetworkRequest &req)
{
    qDebug().noquote() << "[remote-render] DELETE" << req.url().toString();
    QNetworkReply *reply = renderSessionManager()->deleteResource(req);
    QObject::connect(reply, &QNetworkReply::finished, reply, &QNetworkReply::deleteLater);
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
    return renderSessionManager()->post(req, body);
}

void deleteRenderSession(const QString &backendUrl, const QString &token,
                         const QString &sessionId, const QString &renderSessionId)
{
    if (renderSessionId.isEmpty())
        return;
    deleteAndForget(renderRequest(backendUrl, token, sessionId,
                                  u"/v1/render/"_s + renderSessionId));
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
