#pragma once

#include <QImage>
#include <QObject>
#include <QPair>
#include <QString>
#include <QVariantMap>
#include <QVector>
#include <chrono>
#include <memory>

class QNetworkAccessManager;

// Forward-declare libdatachannel types so the header doesn't drag in
// ``rtc/rtc.hpp`` (large; includes <future>, OpenSSL stub headers etc.).
// The cpp file is where the real includes happen.
namespace rtc {
class PeerConnection;
class DataChannel;
class Track;
}  // namespace rtc

// Forward-declare FFmpeg types so the header stays libavcodec-free.
struct AVCodecContext;
struct AVPacket;
struct AVFrame;
struct SwsContext;

/**
 * @brief WebRTC transport client for VisIVO's H.265 / NVENC SSR path.
 *
 * Headless counterpart to ``RemoteRenderWidget``'s JPEG/QWebSocket path:
 * opens an aiortc-compatible peer connection against the backend's
 * ``/v1/render/{id}/sdp/offer`` endpoint, decodes incoming H.264 RTP
 * frames via FFmpeg, and emits ``QImage`` snapshots ready for the
 * widget to paint.
 *
 * Why a separate class rather than extending RemoteRenderWidget:
 *
 *   * Keeps the WebRTC plumbing isolated from Qt widget concerns —
 *     unit-testable without a display.
 *   * Lets the same widget host either transport at runtime: the
 *     widget picks JpegWS (existing path) OR creates an instance of
 *     this class for H.265-WebRTC.
 *   * libdatachannel + FFmpeg pull in non-trivial headers we don't
 *     want bleeding into every translation unit that includes
 *     RemoteRenderWidget.h.
 *
 * Protocol (must stay byte-compatible with the backend
 * ``backend/app/webrtc_session.py``):
 *
 *   1. ``GET /v1/render/diagnostics`` — caller already verified the
 *      backend advertises ``h265-webrtc``.
 *   2. Create RTCPeerConnection + a recv-only video transceiver + a
 *      DataChannel labelled "commands".
 *   3. ``POST /v1/render/{id}/sdp/offer`` with the local SDP. Backend
 *      returns the answer. The endpoint requires the ``X-Visivo-Token``
 *      auth header.
 *   4. Once ICE/DTLS settles the video track starts delivering H.264
 *      NAL units; we depacketize via libdatachannel's H264 handler,
 *      decode with libavcodec, scale YUV→RGB with libswscale, emit
 *      ``frameReady`` with a QImage.
 *   5. Camera commands (rotate / zoom / threshold / LUT / …) go over
 *      the DataChannel as the SAME JSON the JPEG path uses over WS.
 *
 * Lifetime: caller owns the instance and destroys it on transport
 * switch / window close. Calling ``disconnectFromSession()`` is
 * idempotent.
 */
class RemoteRenderWebRTCClient : public QObject {
    Q_OBJECT

public:
    explicit RemoteRenderWebRTCClient(QObject *parent = nullptr);
    ~RemoteRenderWebRTCClient() override;

    /// Start the SDP negotiation. Non-blocking — completion is
    /// reported asynchronously via ``connectionStateChanged`` and the
    /// arrival of the first frame via ``frameReady``.
    void connectToSession(const QString &baseUrl,
                          const QString &sessionId,
                          const QString &token);

    /// Tear down peer connection + decoder. Safe to call from any
    /// state; subsequent calls are no-ops.
    void disconnectFromSession();

    /// True once the data channel is open AND the first decoded frame
    /// has been delivered to the caller.
    bool isLive() const { return m_live; }

    /// Push a state-mutation command to the backend over the
    /// DataChannel. Same JSON shape as the WebSocket transport
    /// (``{"type": ..., ...}``). No-op when the channel isn't yet
    /// open; the caller should re-emit important state when
    /// ``connectionStateChanged`` reports "Live".
    Q_INVOKABLE void sendStateCommand(const QVariantMap &command);

signals:
    /// A decoded video frame, ready for QPainter::drawImage().
    /// The QImage is heap-allocated and owned by the consumer.
    void frameReady(const QImage &frame);

    /// Smoothed FPS counter (~5-frame EMA), emitted after every frame.
    void framesPerSecondChanged(double fps);

    /// Human-readable state — surfaced to the widget's status overlay.
    /// Values: "Idle", "Negotiating", "Connecting", "Live",
    /// "Disconnected", "Error: ...".
    void connectionStateChanged(const QString &state);

private:
    // ── REST signalling ──────────────────────────────────────────
    void postSdpOffer(const QString &offerSdp);
    void handleSdpAnswer(const QByteArray &responseBody);
    /// Trickle-ICE: send one local candidate to the server as soon
    /// as libdatachannel emits it. Cuts first-frame latency by
    /// avoiding the wait for the full STUN-gathering timeout.
    void postIceCandidate(const QString &candidate, const QString &mid);

    // ── Peer connection lifecycle ────────────────────────────────
    void setupPeerConnection();
    void onLocalDescriptionReady(const QString &sdp, const QString &type);
    void onTrackOpened(std::shared_ptr<rtc::Track> track);
    void onFrameBytes(const QByteArray &nalUnits);

    // ── H.264 decoder ────────────────────────────────────────────
    void openDecoder();
    void closeDecoder();
    void feedDecoder(const QByteArray &nal);
    QImage drainDecoder();

    // ── Telemetry ────────────────────────────────────────────────
    void setStatus(const QString &status);

    QString  m_baseUrl;
    QString  m_sessionId;
    QString  m_token;
    bool     m_live{ false };
    QString  m_status{ "Idle" };

    // Trickle-ICE candidate buffer. libdatachannel starts emitting
    // candidates as soon as setLocalDescription() runs — typically
    // BEFORE we've even posted the offer, let alone received the
    // answer. The backend, however, can only addIceCandidate() after
    // setRemoteDescription(). We buffer locally and flush after the
    // server has acknowledged our offer (handleSdpAnswer).
    bool     m_answerReceived{ false };
    QVector<QPair<QString, QString>> m_iceBacklog;   // (candidate, mid)

    // Pre-open data-channel command queue. PC ``Connected`` → "Live"
    // fires up to ~300 ms before SCTP datachannel negotiation finishes,
    // and vtkWindowCube's bootstrap state push reacts to "Live". Any
    // ``sendStateCommand`` call that arrives in that window goes here,
    // and the ``commands`` channel's ``onOpen`` flushes the queue in
    // FIFO order. Without this the bootstrap was silently lost and the
    // backend kept rendering with its build-time defaults.
    QVector<QByteArray> m_pendingCommands;

    QNetworkAccessManager *m_nam{ nullptr };

    // libdatachannel handles (shared_ptr — its API requires it).
    std::shared_ptr<rtc::PeerConnection> m_pc;
    std::shared_ptr<rtc::Track>          m_videoTrack;
    std::shared_ptr<rtc::DataChannel>    m_commandsChannel;

    // FFmpeg decode state.
    AVCodecContext *m_codecCtx{ nullptr };
    AVPacket       *m_pkt{ nullptr };
    AVFrame        *m_frame{ nullptr };
    SwsContext     *m_sws{ nullptr };
    int             m_swsWidth{ 0 };
    int             m_swsHeight{ 0 };

    // FPS measurement (EMA over ~5 frames).
    std::chrono::steady_clock::time_point m_lastFrameTime;
    double m_fpsEma{ 0.0 };
};
