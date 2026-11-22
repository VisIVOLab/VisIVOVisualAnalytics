#include "RemotePointView.h"

#include "RemoteCameraPreset.h"
#include "RemoteRenderWidget.h"

#include <QDebug>
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
      m_nam(new QNetworkAccessManager(this)),
      m_reconnectTimer(new QTimer(this))
{
    m_reconnectTimer->setSingleShot(true);
    QObject::connect(m_reconnectTimer, &QTimer::timeout, this, &RemotePointView::onReconnectTimer);
    if (m_widget) {
        QObject::connect(m_widget, &RemoteRenderWidget::viewTurnedByUser, this,
                         &RemotePointView::scheduleCameraSnapshot);
        QObject::connect(m_widget, &RemoteRenderWidget::connectionStateChanged, this,
                         [this](const QString &state) {
                             const bool live = state == QLatin1String("Live");
                             // A reconnection's stream that ended before its
                             // first frame: an attempt that failed.
                             if (m_attemptConnecting && !live
                                 && visivo::StreamCloseRecorder::isTerminal(state)) {
                                 m_attemptConnecting = false;
                                 m_reconnectTimer->stop();
                                 m_reconnectWhy = u"the stream could not be opened again ("_s
                                         + state + u")"_s;
                                 runStep(m_reconnect.onAttemptFailed());
                                 return;
                             }
                             if (live == m_live)
                                 return;
                             const bool dropped = m_live && !live && m_widget
                                     && visivo::StreamCloseRecorder::isTerminal(state);
                             m_live = live;
                             if (dropped) {
                                 visivo::RemoteDrop drop;
                                 drop.showingRemote = isActive();
                                 drop.wasLive = m_widget->wasLiveAtClose();
                                 drop.closeReason = m_widget->lastCloseReason();
                                 drop.liveMs = m_widget->liveMsAtClose();
                                 const visivo::ReconnectStep step = m_reconnect.onDrop(drop);
                                 if (step != visivo::ReconnectStep::Ignore) {
                                     qDebug().noquote()
                                             << "[point-render] stream of session"
                                             << m_renderSessionId << "dropped; reconnecting";
                                     m_reconnectWhy = visivo::sessionEnded(drop)
                                             ? u"its render session ended"_s
                                             : u"the connection to the backend was lost"_s;
                                     emit liveChanged(false);
                                     runStep(step);
                                     return;
                                 }
                             }
                             if (live) {
                                 m_reconnect.onLive();
                                 m_attemptConnecting = false;
                                 m_reconnectTimer->stop();
                                 // A new stream knows nothing of what was sent to
                                 // the session before; the session does, but a
                                 // reconnect may be to a new one: send it all.
                                 m_sent.clear();
                                 flush();
                                 // After the positions: a new session that
                                 // replaces a lost one is turned to the view the
                                 // user had, not left at the default camera.
                                 if (m_cameraRestore) {
                                     m_cameraRestore = false;
                                     const QVariantMap camera =
                                             visivo::remote::setCameraCommand(m_camera);
                                     if (!camera.isEmpty())
                                         m_widget->sendStateCommand(camera);
                                 }
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
        if (m_reconnect.active()) {
            // A new session for a lost one could not be made (the backend out
            // of reach, a tunnel being reopened): asked again after a wait.
            m_reconnectWhy = visivo::replacementFailure(answer.httpStatus, answer.message);
            runStep(m_reconnect.onAttemptFailed());
            return;
        }
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
    if (m_reconnect.active()) {
        // A replacement for a lost session: its stream has a while to start.
        m_attemptConnecting = true;
        m_reconnectTimer->start(visivo::kReconnectAttemptTimeoutMs);
    }
    emit started();
    const QJsonValue lo = doc.value(u"data_min"_s), hi = doc.value(u"data_max"_s);
    if (lo.isDouble() && hi.isDouble())
        emit colourRange(lo.toDouble(), hi.toDouble());
}

void RemotePointView::stop()
{
    // Asked for (the window went Local, or closes): nothing comes back by itself.
    m_reconnect.cancel();
    m_attemptConnecting = false;
    ++m_checkSerial;
    m_reconnectTimer->stop();
    release();
}

void RemotePointView::release()
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
    if (m_live && m_widget) {
        m_widget->sendStateCommand(command);
        scheduleCameraSnapshot();   // a camera reset, say
    }
}

void RemotePointView::runStep(visivo::ReconnectStep step)
{
    using Step = visivo::ReconnectStep;
    m_reconnectTimer->stop();
    switch (step) {
    case Step::Ignore:
        return;
    case Step::CheckSession:
        say(visivo::reconnectStatus(m_reconnect.attempt(), visivo::RemoteReconnect::maxAttempts(), 0));
        if (m_renderSessionId.isEmpty()) {
            // A new session that could not be made: none to ask about.
            runStep(m_reconnect.onCheck(404));
            return;
        }
        checkSession();
        return;
    case Step::SameSession:
        qDebug().noquote() << "[point-render] session" << m_renderSessionId
                           << "is still there; connecting its stream again";
        if (!m_widget)
            return;
        m_widget->setTransport(RemoteRenderWidget::Transport::JpegWS);
        m_widget->connectToSession(m_backendUrl, m_renderSessionId, m_token);
        // After the call, as in created(): its own teardown says Disconnected
        // first, which is not this attempt failing.
        m_attemptConnecting = true;
        m_reconnectTimer->start(visivo::kReconnectAttemptTimeoutMs);
        return;
    case Step::NewSession:
        qDebug().noquote() << "[point-render] session" << m_renderSessionId
                           << "is gone on the backend; asking for a new one";
        say(visivo::reconnectStatus(m_reconnect.attempt(), visivo::RemoteReconnect::maxAttempts(), 0));
        // The state kept (m_state) goes to the new session when it is live,
        // the camera the user had with it.
        m_cameraRestore = !m_camera.isEmpty();
        release();
        start();
        return;
    case Step::Wait:
        say(visivo::reconnectStatus(m_reconnect.attempt(), visivo::RemoteReconnect::maxAttempts(),
                                    m_reconnect.delayMs()));
        m_reconnectTimer->start(m_reconnect.delayMs());
        return;
    case Step::GiveUp:
        qDebug().noquote() << "[point-render] reconnection given up:" << m_reconnectWhy;
        m_attemptConnecting = false;
        emit failed(m_reconnectWhy);
        return;
    }
}

void RemotePointView::onReconnectTimer()
{
    if (m_attemptConnecting) {
        // The stream did not come back in time.
        m_attemptConnecting = false;
        if (m_widget)
            m_widget->disconnectFromSession();
        m_reconnectWhy = u"the stream did not come back"_s;
        runStep(m_reconnect.onAttemptFailed());
        return;
    }
    runStep(m_reconnect.onWaited());
}

void RemotePointView::checkSession()
{
    QUrl url(m_backendUrl);
    url.setPath(u"/v1/render/%1/levels"_s.arg(m_renderSessionId));
    QNetworkRequest req(url);
    // A backend out of reach (a dead tunnel) may not refuse, only stay silent.
    req.setTransferTimeout(5000);
    BackendClient::applySessionHeaders(req, m_backendUrl, m_token, m_sessionId);
    QNetworkReply *reply = m_nam->get(req);
    const quint64 serial = ++m_checkSerial;
    const QString sid = m_renderSessionId;
    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply, serial, sid]() {
        reply->deleteLater();
        if (serial != m_checkSerial || !m_reconnect.active())
            return;   // stopped, or another check under way
        if (sid != m_renderSessionId) {
            // Another session by now: this reconnection has nothing to do.
            m_reconnect.cancel();
            return;
        }
        const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (code == 0 || code >= 500)
            m_reconnectWhy = visivo::replacementFailure(code, QString());
        else if (code == 401 || code == 403)
            m_reconnectWhy = u"the backend refused this client's token (HTTP %1)"_s.arg(code);
        runStep(m_reconnect.onCheck(code));
    });
}

void RemotePointView::say(const QString &status)
{
    if (m_widget)
        m_widget->showStatus(status);
    emit reconnecting(status);
}

void RemotePointView::scheduleCameraSnapshot()
{
    if (!m_cameraTimer) {
        m_cameraTimer = new QTimer(this);
        m_cameraTimer->setSingleShot(true);
        m_cameraTimer->setInterval(700);
        QObject::connect(m_cameraTimer, &QTimer::timeout, this, [this]() {
            if (!m_live || m_renderSessionId.isEmpty())
                return;
            QUrl url(m_backendUrl);
            url.setPath(u"/v1/render/%1/camera"_s.arg(m_renderSessionId));
            QNetworkRequest req(url);
            req.setTransferTimeout(5000);
            BackendClient::applySessionHeaders(req, m_backendUrl, m_token, m_sessionId);
            QNetworkReply *reply = m_nam->get(req);
            const QString sid = m_renderSessionId;
            QObject::connect(reply, &QNetworkReply::finished, this, [this, reply, sid]() {
                reply->deleteLater();
                if (reply->error() != QNetworkReply::NoError || sid != m_renderSessionId)
                    return;
                const QJsonObject camera = QJsonDocument::fromJson(reply->readAll()).object();
                if (!visivo::remote::setCameraCommand(camera).isEmpty())
                    m_camera = camera;
            });
        });
    }
    m_cameraTimer->start();
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
        // The backend fits its camera to new positions: the one a replacement
        // session would get is read again.
        if (type == u"set_point_fields"_s || type == u"set_sky_positions"_s)
            scheduleCameraSnapshot();
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
