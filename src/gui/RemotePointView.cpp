#include "RemotePointView.h"

#include "RemoteRenderWidget.h"
#include "RenderSessionCalls.h"
#include "app/BackendClient.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

using namespace Qt::Literals::StringLiterals;

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
    if (isActive() || m_requesting || !m_widget)
        return;
    ++m_generation;
    m_requesting = true;
    request();
}

void RemotePointView::request()
{
    const quint64 generation = m_generation;
    const QByteArray body =
            QJsonDocument(m_place.requestBody(m_datasetId)).toJson(QJsonDocument::Compact);
    // Through the application's manager: a view deleted meanwhile must not take
    // the request with it, or the session it makes would be nobody's.
    QNetworkReply *reply =
            visivo::postCreateRenderSession(m_backendUrl, m_token, m_sessionId, body);
    const QPointer<RemotePointView> self(this);
    const QString backendUrl = m_backendUrl, token = m_token, sessionId = m_sessionId;
    QObject::connect(reply, &QNetworkReply::finished, reply, [=]() {
        reply->deleteLater();
        const QJsonObject doc = QJsonDocument::fromJson(reply->readAll()).object();
        const visivo::RenderSessionAnswer answer = visivo::parseRenderSessionAnswer(
                reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(),
                reply->error() != QNetworkReply::NoError, doc);
        if (!self || generation != self->m_generation) {
            // Stopped or deleted meanwhile: the session (or the place) the
            // backend made is nobody's.
            const visivo::RenderQueuePlace::Decision d =
                    visivo::RenderQueuePlace().decide(false, answer);
            visivo::deleteRenderSession(backendUrl, token, sessionId, d.sessionToDelete);
            visivo::cancelRenderQueuePlace(backendUrl, token, sessionId, d.placeToCancel);
            return;
        }
        self->created(answer, doc);
    });
}

void RemotePointView::created(const visivo::RenderSessionAnswer &answer, const QJsonObject &doc)
{
    using Step = visivo::RenderQueuePlace::Step;
    const visivo::RenderQueuePlace::Decision d = m_place.decide(true, answer);
    visivo::cancelRenderQueuePlace(m_backendUrl, m_token, m_sessionId, d.placeToCancel);
    const quint64 generation = m_generation;
    switch (d.step) {
    case Step::AskLater:
        emit queued(visivo::queueStatusText(answer));
        QTimer::singleShot(visivo::queuePollDelayMs(answer), this, [this, generation]() {
            if (generation == m_generation)
                request();
        });
        return;
    case Step::Preparing:
        // A point table has no pyramid; a backend that says so anyway is asked
        // again in a moment, as the cube viewer does.
        QTimer::singleShot(2000, this, [this, generation]() {
            if (generation == m_generation)
                request();
        });
        return;
    case Step::AskNow:
        // The place was dropped (a poll came too late): the back of the queue.
        request();
        return;
    case Step::StayLocal:
    case Step::Ignore:
        m_requesting = false;
        if (!answer.message.isEmpty())
            emit failed(answer.message);
        else if (answer.httpStatus >= 200 && answer.httpStatus < 300)
            emit failed(u"The backend returned no render session."_s);
        else
            emit failed(u"HTTP %1"_s.arg(answer.httpStatus));
        return;
    case Step::Connect:
        break;
    }
    m_requesting = false;
    const QString sid = answer.sessionId;
    if (!m_widget) {
        // The view it was for is gone: the session is nobody's.
        visivo::deleteRenderSession(m_backendUrl, m_token, m_sessionId, sid);
        emit failed(u"The view closed while its render session was made."_s);
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
}

void RemotePointView::stop()
{
    ++m_generation;
    m_requesting = false;
    m_live = false;
    m_sent.clear();
    if (m_widget)
        m_widget->disconnectFromSession();
    // Waiting in the queue: give the place up, or the next free slot is held
    // for nobody until the place expires.
    visivo::cancelRenderQueuePlace(m_backendUrl, m_token, m_sessionId, m_place.take());
    if (m_renderSessionId.isEmpty())
        return;
    visivo::deleteRenderSession(m_backendUrl, m_token, m_sessionId, m_renderSessionId);
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
