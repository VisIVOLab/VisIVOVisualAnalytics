#pragma once

// RenderSessionCalls.h
// The requests that make and give back render sessions and queue places, sent
// through a network manager the application owns rather than a viewer: a
// viewer closed meanwhile would take its manager and the request with it, and
// a session (or a slot reserved for a place) would stay held on the backend
// until its timeout. Shared by the cube viewer and RemotePointView.

#include <QString>

class QByteArray;
class QNetworkAccessManager;
class QNetworkReply;

namespace visivo {

/// The application's manager for render-session requests.
QNetworkAccessManager *renderSessionManager();

/// POST /v1/render/create_session with @p body; the caller connects finished.
QNetworkReply *postCreateRenderSession(const QString &backendUrl, const QString &token,
                                       const QString &sessionId, const QByteArray &body);

/// DELETE /v1/render/{renderSessionId}, fire and forget.
void deleteRenderSession(const QString &backendUrl, const QString &token,
                         const QString &sessionId, const QString &renderSessionId);

/// DELETE /v1/render/queue/{queueId}, fire and forget: gives up a place.
void cancelRenderQueuePlace(const QString &backendUrl, const QString &token,
                            const QString &sessionId, const QString &queueId);

} // namespace visivo
