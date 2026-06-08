#include "RemoteRenderWebRTCClient.h"

#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QtEndian>

#include <rtc/rtc.hpp>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <atomic>

using namespace std::chrono_literals;

namespace {
// The session bitrate aiortc reports in its SDP answer is fine — we
// don't pin it client-side. NAL aggregation buffer; 4 MiB covers a
// typical IDR for a 1080p frame with NVENC settings. Resized at
// runtime when an actual frame exceeds it.
constexpr int kNalBufferReserve = 4 * 1024 * 1024;
}  // namespace


// ─────────────────────────────────────────────────────────────────────────────
// Construction / destruction
// ─────────────────────────────────────────────────────────────────────────────


RemoteRenderWebRTCClient::RemoteRenderWebRTCClient(QObject *parent)
    : QObject(parent),
      m_lastFrameTime(std::chrono::steady_clock::now())
{
    m_nam = new QNetworkAccessManager(this);
}

RemoteRenderWebRTCClient::~RemoteRenderWebRTCClient()
{
    disconnectFromSession();
}


// ─────────────────────────────────────────────────────────────────────────────
// Session lifecycle
// ─────────────────────────────────────────────────────────────────────────────


void RemoteRenderWebRTCClient::connectToSession(const QString &baseUrl,
                                                 const QString &sessionId,
                                                 const QString &token)
{
    disconnectFromSession();

    m_baseUrl   = baseUrl;
    m_sessionId = sessionId;
    m_token     = token;

    setStatus(u"Negotiating"_qs);
    setupPeerConnection();
}

void RemoteRenderWebRTCClient::disconnectFromSession()
{
    // Order matters: data channel first, then peer connection. Calling
    // close on a peer connection cleans up the channels in flight but
    // the explicit close gives us cleaner shutdown logs.
    if (m_commandsChannel) {
        try { m_commandsChannel->close(); } catch (...) {}
        m_commandsChannel.reset();
    }
    if (m_videoTrack) {
        try { m_videoTrack->close(); } catch (...) {}
        m_videoTrack.reset();
    }
    if (m_pc) {
        try { m_pc->close(); } catch (...) {}
        m_pc.reset();
    }
    closeDecoder();

    m_live = false;
    setStatus(u"Disconnected"_qs);
}


// ─────────────────────────────────────────────────────────────────────────────
// Peer connection setup
// ─────────────────────────────────────────────────────────────────────────────


void RemoteRenderWebRTCClient::setupPeerConnection()
{
    // ICE config: STUN is REQUIRED for the typical deployment where
    // the backend (a SLURM compute node) and the client (a desktop on
    // a corporate / home LAN) sit behind different NATs. The SSH
    // tunnel forwards the HTTP/SDP layer but NOT the WebRTC media —
    // the actual UDP packets flow peer-to-peer using the candidates
    // each side gathered. Without STUN both sides only see private
    // candidates that the other can't reach.
    //
    // First-frame latency cost is ~10-30 s for STUN gather + ICE
    // checks + DTLS handshake + first IDR. The Polish phase will
    // implement trickle ICE (post the offer with only host
    // candidates immediately, push srflx candidates as they come
    // in) to cut most of that.
    rtc::Configuration cfg;
    cfg.iceServers.emplace_back("stun:stun.l.google.com:19302");

    try {
        m_pc = std::make_shared<rtc::PeerConnection>(cfg);
    } catch (const std::exception &e) {
        setStatus(QStringLiteral("Error: %1").arg(QString::fromUtf8(e.what())));
        return;
    }

    // ICE / connection state — surface to the UI for the status badge.
    m_pc->onStateChange([this](rtc::PeerConnection::State state) {
        // Marshal back to the Qt event loop. libdatachannel callbacks
        // fire on their own threads.
        QMetaObject::invokeMethod(this, [this, state]() {
            switch (state) {
                case rtc::PeerConnection::State::New:
                case rtc::PeerConnection::State::Connecting:
                    setStatus(u"Connecting"_qs);
                    break;
                case rtc::PeerConnection::State::Connected:
                    setStatus(u"Live"_qs);
                    break;
                case rtc::PeerConnection::State::Disconnected:
                case rtc::PeerConnection::State::Failed:
                case rtc::PeerConnection::State::Closed:
                    setStatus(u"Disconnected"_qs);
                    m_live = false;
                    break;
            }
        }, Qt::QueuedConnection);
    });

    // The aiortc server sends SDP only when our offer arrives, so we
    // wait until the local description is fully assembled (after ICE
    // gathering completes) before POSTing.
    m_pc->onGatheringStateChange([this](rtc::PeerConnection::GatheringState state) {
        if (state == rtc::PeerConnection::GatheringState::Complete) {
            if (auto desc = m_pc->localDescription()) {
                const QString sdp  = QString::fromStdString(std::string(*desc));
                const QString type = QString::fromStdString(desc->typeString());
                QMetaObject::invokeMethod(this, [this, sdp, type]() {
                    onLocalDescriptionReady(sdp, type);
                }, Qt::QueuedConnection);
            }
        }
    });

    // Order matters in libdatachannel 0.21.x: add the video track
    // FIRST, then the data channel. Reversed, the data-channel-only
    // configuration freezes the offer and addTrack silently no-ops —
    // the SDP comes out with only ``m=application``. The
    // media-receiver example sets up exactly this order; respecting
    // it makes addTrack append the m=video section as expected.
    rtc::Description::Video media("video", rtc::Description::Direction::RecvOnly);
    media.addH264Codec(96);     // libdatachannel's DEFAULT_H264_VIDEO_PROFILE
                                // is already the canonical aiortc fmtp set
    media.setBitrate(3000);     // 3 Mbps soft ceiling
    m_videoTrack = m_pc->addTrack(media);
    if (!m_videoTrack) {
        setStatus(u"Error: addTrack returned null"_qs);
        return;
    }
    qDebug().noquote() << QStringLiteral(
            "[webrtc] addTrack succeeded — video track created");

    // H.264 RTP depacketizer aggregates fragmented NAL units back into
    // whole frames (handles FU-A and STAP-A modes that aiortc uses).
    auto depacketizer = std::make_shared<rtc::H264RtpDepacketizer>();
    auto rtcpReceiver = std::make_shared<rtc::RtcpReceivingSession>();
    depacketizer->addToChain(rtcpReceiver);
    m_videoTrack->setMediaHandler(depacketizer);

    m_videoTrack->onFrame([this](rtc::binary frame, rtc::FrameInfo /*info*/) {
        QByteArray bytes(reinterpret_cast<const char *>(frame.data()),
                         static_cast<int>(frame.size()));
        QMetaObject::invokeMethod(this, [this, bytes]() {
            onFrameBytes(bytes);
        }, Qt::QueuedConnection);
    });

    // DataChannel "commands" — added AFTER the video track so the
    // SDP contains both BUNDLE entries (0 = video, 1 = data).
    m_commandsChannel = m_pc->createDataChannel("commands");
    m_commandsChannel->onOpen([this]() {
        QMetaObject::invokeMethod(this, [this]() {
            qDebug().noquote() << QStringLiteral(
                    "[webrtc] commands channel open (session=%1)")
                    .arg(m_sessionId);
        }, Qt::QueuedConnection);
    });
    m_commandsChannel->onMessage([](rtc::message_variant /*data*/) {
        // Server → client commands aren't part of the protocol yet.
    });

    // Generate the offer. Must be the LAST step — after the track,
    // codec, depacketizer, onFrame, and data channel are configured.
    m_pc->setLocalDescription();
}


// ─────────────────────────────────────────────────────────────────────────────
// SDP exchange via REST
// ─────────────────────────────────────────────────────────────────────────────


void RemoteRenderWebRTCClient::onLocalDescriptionReady(const QString &sdp,
                                                        const QString &type)
{
    // SDP fixup. libdatachannel emits a barebones offer that aiortc's
    // negotiator chokes on for two reasons:
    //
    //   1. The m=video section lacks an ``a=recvonly`` (or sendrecv/
    //      sendonly/inactive) direction attribute, even though we
    //      pass Direction::RecvOnly to the constructor. aiortc's
    //      ``setLocalDescription`` resolves direction via
    //      ``and_direction(t.direction, t._offerDirection)`` and
    //      crashes with ``ValueError: None is not in list`` when
    //      ``_offerDirection`` is None.
    //
    //   2. The H264 codec line is missing the canonical SDP fmtp
    //      params (``packetization-mode``, ``profile-level-id``,
    //      ``level-asymmetry-allowed``) that aiortc's codec match
    //      looks up.
    //
    // Patch both before POSTing. We log the resulting offer so the
    // first few bytes of the m=video section are visible if anything
    // still goes wrong.
    QString patched = sdp;

    // Find the m=video block: starts at "m=video" and ends at the
    // next "m=" or EOF.
    const int mIdx = patched.indexOf(QStringLiteral("m=video "));
    if (mIdx >= 0) {
        int endIdx = patched.indexOf(QStringLiteral("\nm="), mIdx + 1);
        if (endIdx < 0) endIdx = patched.size();
        QString mBlock = patched.mid(mIdx, endIdx - mIdx);

        // (1) ensure a direction attribute. Insert ``a=recvonly\r\n``
        // right after the m=video header line if no a=recvonly /
        // a=sendrecv / a=sendonly / a=inactive is present.
        const bool hasDirection =
                mBlock.contains(QStringLiteral("a=recvonly"))
                || mBlock.contains(QStringLiteral("a=sendrecv"))
                || mBlock.contains(QStringLiteral("a=sendonly"))
                || mBlock.contains(QStringLiteral("a=inactive"));
        if (!hasDirection) {
            const int hdrEnd = mBlock.indexOf(QStringLiteral("\r\n"));
            if (hdrEnd > 0) {
                mBlock.insert(hdrEnd + 2,
                              QStringLiteral("a=recvonly\r\n"));
            }
        }

        // (2) ensure the H.264 fmtp line. Insert after the
        // ``a=rtpmap:96 H264/90000`` if missing.
        const QString rtpMapTag = QStringLiteral("a=rtpmap:96 H264/90000\r\n");
        const QString fmtpLine = QStringLiteral(
                "a=fmtp:96 "
                "level-asymmetry-allowed=1;"
                "packetization-mode=1;"
                "profile-level-id=42001f\r\n");
        if (mBlock.contains(rtpMapTag)
            && !mBlock.contains(QStringLiteral("a=fmtp:96"))) {
            mBlock.replace(rtpMapTag, rtpMapTag + fmtpLine);
        }

        patched.replace(mIdx, endIdx - mIdx, mBlock);
    }

    qDebug().noquote() << QStringLiteral(
            "[webrtc] local description ready (type=%1, original=%2 bytes, "
            "patched=%3 bytes) — posting to /v1/render/%4/sdp/offer")
            .arg(type).arg(sdp.size()).arg(patched.size()).arg(m_sessionId);
    // Dump the FULL SDP. With 766-byte offers we're missing the
    // entire video m-line — addTrack() didn't take. Seeing the raw
    // payload is the fastest path to figuring out which libdatachannel
    // API call is the right one.
    qDebug().noquote() << QStringLiteral(
            "[webrtc] FULL SDP being sent (%1 bytes):\n%2")
            .arg(patched.size()).arg(patched);
    postSdpOffer(patched);
}

void RemoteRenderWebRTCClient::postSdpOffer(const QString &offerSdp)
{
    QUrl url(m_baseUrl);
    url.setPath(QStringLiteral("/v1/render/%1/sdp/offer").arg(m_sessionId));

    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, u"application/json"_qs);
    req.setRawHeader(QByteArrayLiteral("X-Visivo-Token"), m_token.toUtf8());

    QJsonObject body;
    body[u"sdp"_qs]  = offerSdp;
    body[u"type"_qs] = u"offer"_qs;
    const QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);

    QNetworkReply *reply = m_nam->post(req, payload);
    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            // On a 4xx/5xx the server typically still sends a JSON
            // body with ``detail`` (FastAPI's convention). Surface
            // that to the user instead of the generic Qt error string
            // — it's the actual Python exception that explains the
            // negotiation failure.
            const QByteArray body = reply->readAll();
            QString detail = reply->errorString();
            if (!body.isEmpty()) {
                const QJsonObject obj = QJsonDocument::fromJson(body).object();
                const QString fastApiDetail = obj.value(u"detail"_qs).toString();
                if (!fastApiDetail.isEmpty()) {
                    detail = fastApiDetail;
                } else {
                    // Body wasn't FastAPI-shaped JSON; show the first
                    // ~200 bytes verbatim so the cause isn't hidden.
                    detail = QString::fromUtf8(body.left(200));
                }
            }
            qWarning().noquote() << QStringLiteral(
                    "[webrtc] SDP offer HTTP %1 — %2").arg(code).arg(detail);
            setStatus(QStringLiteral("Error: SDP offer HTTP %1 — %2")
                              .arg(code).arg(detail));
            return;
        }
        handleSdpAnswer(reply->readAll());
    });
}

void RemoteRenderWebRTCClient::handleSdpAnswer(const QByteArray &responseBody)
{
    const QJsonDocument doc = QJsonDocument::fromJson(responseBody);
    const QJsonObject obj = doc.object();
    const QString sdp  = obj.value(u"sdp"_qs).toString();
    const QString type = obj.value(u"type"_qs).toString();
    if (sdp.isEmpty() || type.isEmpty()) {
        setStatus(u"Error: malformed SDP answer"_qs);
        return;
    }
    qDebug().noquote() << QStringLiteral(
            "[webrtc] received SDP answer (type=%1, %2 bytes) — "
            "applying remote description").arg(type).arg(sdp.size());
    try {
        m_pc->setRemoteDescription(
                rtc::Description(sdp.toStdString(), type.toStdString()));
    } catch (const std::exception &e) {
        setStatus(QStringLiteral("Error: setRemoteDescription %1")
                          .arg(QString::fromUtf8(e.what())));
    }
}


// ─────────────────────────────────────────────────────────────────────────────
// DataChannel send
// ─────────────────────────────────────────────────────────────────────────────


void RemoteRenderWebRTCClient::sendStateCommand(const QVariantMap &command)
{
    if (!m_commandsChannel || !m_commandsChannel->isOpen()) {
        // Silently drop — the caller (vtkWindowCube) re-pushes state
        // when the channel opens via the connectionStateChanged
        // "Live" transition.
        return;
    }
    const QJsonObject obj = QJsonObject::fromVariantMap(command);
    const QByteArray bytes = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    try {
        m_commandsChannel->send(std::string(bytes.constData(), bytes.size()));
    } catch (const std::exception &e) {
        qWarning().noquote() << QStringLiteral(
                "[webrtc] sendStateCommand failed: %1").arg(QString::fromUtf8(e.what()));
    }
}


// ─────────────────────────────────────────────────────────────────────────────
// H.264 decode (FFmpeg)
// ─────────────────────────────────────────────────────────────────────────────


void RemoteRenderWebRTCClient::openDecoder()
{
    if (m_codecCtx) return;

    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
        setStatus(u"Error: H.264 decoder not available in FFmpeg build"_qs);
        return;
    }
    m_codecCtx = avcodec_alloc_context3(codec);
    if (!m_codecCtx) {
        setStatus(u"Error: avcodec_alloc_context3 failed"_qs);
        return;
    }
    // Match aiortc / NVENC output: yuv420p.
    // 0 = let FFmpeg pick (typically nproc). Single-threaded decode
    // capped us at ~7 FPS on Apple Silicon; multi-thread typically
    // hits 30+ FPS for the 1024×768 frames the backend emits.
    // FF_THREAD_FRAME | FF_THREAD_SLICE lets the decoder pick the
    // mode the H.264 stream parameters allow (slice for I-frames,
    // frame parallelism for P/B chains).
    m_codecCtx->thread_count = 0;
    m_codecCtx->thread_type  = FF_THREAD_FRAME | FF_THREAD_SLICE;
    if (avcodec_open2(m_codecCtx, codec, nullptr) < 0) {
        avcodec_free_context(&m_codecCtx);
        setStatus(u"Error: avcodec_open2 failed"_qs);
        return;
    }
    m_pkt   = av_packet_alloc();
    m_frame = av_frame_alloc();
}

void RemoteRenderWebRTCClient::closeDecoder()
{
    if (m_sws) {
        sws_freeContext(m_sws);
        m_sws = nullptr;
        m_swsWidth = m_swsHeight = 0;
    }
    if (m_frame) {
        av_frame_free(&m_frame);
        m_frame = nullptr;
    }
    if (m_pkt) {
        av_packet_free(&m_pkt);
        m_pkt = nullptr;
    }
    if (m_codecCtx) {
        avcodec_free_context(&m_codecCtx);
        m_codecCtx = nullptr;
    }
}

void RemoteRenderWebRTCClient::feedDecoder(const QByteArray &nal)
{
    if (!m_codecCtx) openDecoder();
    if (!m_codecCtx || !m_pkt) return;
    av_packet_unref(m_pkt);
    m_pkt->data = reinterpret_cast<uint8_t *>(const_cast<char *>(nal.constData()));
    m_pkt->size = nal.size();
    const int rc = avcodec_send_packet(m_codecCtx, m_pkt);
    if (rc < 0 && rc != AVERROR(EAGAIN)) {
        qDebug().noquote() << QStringLiteral(
                "[webrtc] avcodec_send_packet returned %1").arg(rc);
    }
}

QImage RemoteRenderWebRTCClient::drainDecoder()
{
    if (!m_codecCtx || !m_frame) return {};
    const int rc = avcodec_receive_frame(m_codecCtx, m_frame);
    if (rc < 0) {
        // EAGAIN = need more packets, normal during start-up.
        return {};
    }

    const int w = m_frame->width;
    const int h = m_frame->height;
    if (w <= 0 || h <= 0) {
        av_frame_unref(m_frame);
        return {};
    }

    // Build / refresh the sws scaler if the frame geometry changed.
    if (!m_sws || m_swsWidth != w || m_swsHeight != h) {
        if (m_sws) sws_freeContext(m_sws);
        m_sws = sws_getContext(
                w, h, static_cast<AVPixelFormat>(m_frame->format),
                w, h, AV_PIX_FMT_RGB24,
                SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!m_sws) {
            av_frame_unref(m_frame);
            return {};
        }
        m_swsWidth  = w;
        m_swsHeight = h;
    }

    QImage out(w, h, QImage::Format_RGB888);
    uint8_t *dst[1] = { out.bits() };
    int dstStride[1] = { static_cast<int>(out.bytesPerLine()) };
    sws_scale(m_sws,
              m_frame->data, m_frame->linesize,
              0, h,
              dst, dstStride);
    av_frame_unref(m_frame);
    return out;
}

void RemoteRenderWebRTCClient::onFrameBytes(const QByteArray &nalUnits)
{
    feedDecoder(nalUnits);
    QImage img = drainDecoder();
    while (!img.isNull()) {
        // FPS smoothing.
        const auto now = std::chrono::steady_clock::now();
        const double dt = std::chrono::duration<double>(now - m_lastFrameTime).count();
        m_lastFrameTime = now;
        if (dt > 0.0 && dt < 10.0) {
            const double instant = 1.0 / dt;
            m_fpsEma = (m_fpsEma == 0.0) ? instant : (0.8 * m_fpsEma + 0.2 * instant);
            emit framesPerSecondChanged(m_fpsEma);
        }

        if (!m_live) {
            m_live = true;
            setStatus(u"Live"_qs);
        }
        emit frameReady(img);
        img = drainDecoder();  // multiple frames may be ready per packet
    }
}


// ─────────────────────────────────────────────────────────────────────────────
// Status helper
// ─────────────────────────────────────────────────────────────────────────────


void RemoteRenderWebRTCClient::setStatus(const QString &status)
{
    if (m_status == status) return;
    m_status = status;
    emit connectionStateChanged(status);
}
