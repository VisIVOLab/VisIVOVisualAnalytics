// BackendClient_VrPair.cpp
// Headset pairing transport (issue #214, contract visivo-pair/1 of #210):
// the three /v1/vr/pair routes "Open in VR Headset…" uses to pair a headset
// by code. The replies are returned raw; app/VrPair.h says what they mean.
//
// The code is a short-lived credential: it travels in the reply body and in
// the DELETE path, neither of which is logged here.

#include "BackendClient.h"

#include <QJsonDocument>
#include <QUrl>

BackendVrPairReply BackendClient::createVrPairCode(const QJsonObject &body) const
{
    BackendVrPairReply reply;
    QString error;
    QMap<QString, QString> headers;
    reply.body = performPost(QUrl(m_baseUrl + QStringLiteral("/v1/vr/pair")), QJsonDocument(body),
                             error, &headers, &reply.httpStatus);
    if (reply.httpStatus == 0)
        reply.error = error;
    reply.date = headers.value(QStringLiteral("date"));
    return reply;
}

BackendVrPairReply BackendClient::vrPairStatus() const
{
    BackendVrPairReply reply;
    QString error;
    reply.body = performGet(QUrl(m_baseUrl + QStringLiteral("/v1/vr/pair")), error, nullptr,
                            &reply.httpStatus);
    if (reply.httpStatus == 0)
        reply.error = error;
    return reply;
}

bool BackendClient::revokeVrPair(const QString &codeOrId, QString &error, int timeoutMs) const
{
    if (codeOrId.isEmpty()) {
        error = QStringLiteral("nothing to revoke");
        return false;
    }
    const QString safeSeg = QString::fromUtf8(QUrl::toPercentEncoding(codeOrId));
    QByteArray raw;
    return sendBoundedDelete(QStringLiteral("/v1/vr/pair/") + safeSeg, timeoutMs, raw, error);
}
