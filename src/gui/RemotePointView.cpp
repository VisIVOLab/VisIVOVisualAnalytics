#include "RemotePointView.h"

#include "RemoteRenderWidget.h"
#include "app/BackendClient.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

using namespace Qt::Literals::StringLiterals;

namespace {

/// Deletes a render session. Through a manager owned by the application, not
/// by the view: a view deleted right after stop() would take its manager and
/// the request with it, and the session would hold its GPU until the idle TTL.
void deleteRenderSession(const QString &backendUrl, const QString &token,
                         const QString &sessionId, const QString &renderSessionId)
{
    static QNetworkAccessManager *nam = new QNetworkAccessManager(QCoreApplication::instance());
    QUrl url(backendUrl);
    url.setPath(u"/v1/render/"_s + renderSessionId);
    QNetworkRequest req(url);
    BackendClient::applySessionHeaders(req, backendUrl, token, sessionId);
    QNetworkReply *reply = nam->deleteResource(req);
    QObject::connect(reply, &QNetworkReply::finished, reply, &QNetworkReply::deleteLater);
}

} // namespace

RemotePointView::RemotePointView(const QString &backendUrl, const QString &token,
                                 const QString &sessionId, const QString &datasetId,
                                 RemoteRenderWidget *widget, QObject *parent)
    : QObject(parent),
      m_backendUrl(backendUrl),
      m_token(token),
      m_sessionId(sessionId),
      m_datasetId(datasetId),
      m_widget(widget),
      m_nam(new QNetworkAccessManager(this))
{
    if (m_widget) {
        QObject::connect(m_widget, &RemoteRenderWidget::connectionStateChanged, this,
                         [this](const QString &state) {
                             const bool live = state == QLatin1String("Live");
                             if (live == m_live)
                                 return;
                             m_live = live;
                             if (live) {
                                 // A new stream knows nothing of what was sent to
                                 // the session before; the session does, but a
                                 // reconnect may be to a new one: send it all.
                                 m_sent.clear();
                                 flush();
                             }
                             emit liveChanged(live);
                         });
    }
}

RemotePointView::~RemotePointView()
{
    stop();
}

void RemotePointView::start()
{
    if (isActive() || !m_widget)
        return;
    const quint64 generation = ++m_generation;
    QUrl url(m_backendUrl);
    url.setPath(u"/v1/render/create_session"_s);
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, u"application/json"_s);
    // The session header: create_session resolves the dataset in this
    // client's backend session.
    BackendClient::applySessionHeaders(req, m_backendUrl, m_token, m_sessionId);
    const QByteArray body =
            QJsonDocument(QJsonObject{ { u"dataset_id"_s, m_datasetId } }).toJson(
                    QJsonDocument::Compact);
    QNetworkReply *reply = m_nam->post(req, body);
    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply, generation]() {
        reply->deleteLater();
        const QJsonObject doc = QJsonDocument::fromJson(reply->readAll()).object();
        if (generation != m_generation) {
            // Stopped meanwhile: the session the backend made is nobody's.
            const QString orphan = doc.value(u"session_id"_s).toString();
            if (!orphan.isEmpty())
                deleteRenderSession(m_backendUrl, m_token, m_sessionId, orphan);
            return;
        }
        if (reply->error() != QNetworkReply::NoError) {
            const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QString detail = doc.value(u"detail"_s).toString();
            emit failed(detail.isEmpty() ? u"HTTP %1: %2"_s.arg(code).arg(reply->errorString())
                                         : detail);
            return;
        }
        const QString sid = doc.value(u"session_id"_s).toString();
        if (sid.isEmpty() || !m_widget) {
            emit failed(u"The backend returned no render session."_s);
            return;
        }
        m_renderSessionId = sid;
        // JPEG over the WebSocket: the transport every backend has.
        m_widget->setTransport(RemoteRenderWidget::Transport::JpegWS);
        m_widget->connectToSession(m_backendUrl, sid, m_token);
        emit started();
        const QJsonValue lo = doc.value(u"data_min"_s), hi = doc.value(u"data_max"_s);
        if (lo.isDouble() && hi.isDouble())
            emit colourRange(lo.toDouble(), hi.toDouble());
    });
}

void RemotePointView::stop()
{
    ++m_generation;
    m_live = false;
    m_sent.clear();
    if (m_widget)
        m_widget->disconnectFromSession();
    if (m_renderSessionId.isEmpty())
        return;
    deleteRenderSession(m_backendUrl, m_token, m_sessionId, m_renderSessionId);
    m_renderSessionId.clear();
}

void RemotePointView::setState(const QVariantMap &command)
{
    const QString type = command.value(u"type"_s).toString();
    if (type.isEmpty())
        return;
    m_state.insert(type, command);
    flush();
}

void RemotePointView::send(const QVariantMap &command)
{
    if (m_live && m_widget)
        m_widget->sendStateCommand(command);
}

void RemotePointView::flush()
{
    if (!m_live || !m_widget)
        return;
    // Positions before colours: a field change rebuilds the points the colour
    // is then computed for.
    static const QStringList order{ u"set_sky_positions"_s, u"set_point_fields"_s,
                                    u"set_filters"_s, u"set_color"_s };
    QStringList types = order;
    for (auto it = m_state.cbegin(); it != m_state.cend(); ++it)
        if (!types.contains(it.key()))
            types << it.key();
    for (const QString &type : types) {
        const auto it = m_state.constFind(type);
        if (it == m_state.cend() || m_sent.value(type) == *it)
            continue;
        m_widget->sendStateCommand(*it);
        m_sent.insert(type, *it);
        // The new range: the backend answers the levels after the commands
        // already sent (routers/render.py), so one question, once the command
        // is on its way, is enough.
        if (type == u"set_color"_s || type == u"set_filters"_s)
            QTimer::singleShot(200, this, &RemotePointView::fetchLevels);
    }
}

void RemotePointView::fetchLevels()
{
    if (m_renderSessionId.isEmpty())
        return;
    QUrl url(m_backendUrl);
    url.setPath(u"/v1/render/%1/levels"_s.arg(m_renderSessionId));
    QNetworkRequest req(url);
    BackendClient::applySessionHeaders(req, m_backendUrl, m_token, m_sessionId);
    QNetworkReply *reply = m_nam->get(req);
    const quint64 generation = m_generation;
    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply, generation]() {
        reply->deleteLater();
        if (generation != m_generation || reply->error() != QNetworkReply::NoError)
            return;
        const QJsonObject doc = QJsonDocument::fromJson(reply->readAll()).object();
        const QJsonValue lo = doc.value(u"data_min"_s), hi = doc.value(u"data_max"_s);
        if (lo.isDouble() && hi.isDouble())
            emit colourRange(lo.toDouble(), hi.toDouble());
    });
}
