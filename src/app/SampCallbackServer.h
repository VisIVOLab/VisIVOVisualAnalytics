// SampCallbackServer.h – the XML-RPC endpoint a SAMP hub calls VisIVO back on
//
// A SAMP Standard Profile client becomes "callable" by giving the hub a URL
// (samp.hub.setXmlrpcCallback); the hub then POSTs samp.client.* calls to it.
// This is that endpoint: a minimal HTTP/1.1 server on the loopback interface
// and an ephemeral port, built on QTcpServer so it needs nothing beyond
// QtNetwork. It answers each request with whatever the handler returns and
// closes the connection; it never blocks, and it never runs viewer code (the
// handler only decodes and queues).
//
// The HTTP request parsing is a pure function (SampHttp::parseRequest) so the
// unit tests can feed it partial and malformed input.

#ifndef SAMPCALLBACKSERVER_H
#define SAMPCALLBACKSERVER_H

#include "SampXmlRpc.h"

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>

#include <functional>

class QTcpServer;
class QTcpSocket;

namespace SampHttp {

/// Headers larger than this are refused (431).
inline constexpr qsizetype kMaxHeaderBytes = 64 * 1024;

struct Request
{
    enum class State { Incomplete, Complete, Bad };
    State state{ State::Incomplete };
    QByteArray method;
    QByteArray path;
    QByteArray body;
    qsizetype consumed{ 0 };          ///< bytes of the buffer this request used
    bool headersComplete{ false };
    bool expectContinue{ false };     ///< "Expect: 100-continue" was sent
    int status{ 400 };                ///< for State::Bad
    QByteArray reason;                ///< for State::Bad
};

/// Parse one HTTP request from the start of @p buffer. A POST with a
/// Content-Length is all a SAMP hub sends; anything else is refused with the
/// status a client would expect.
inline Request parseRequest(const QByteArray &buffer, qsizetype maxBodyBytes)
{
    Request r;
    const qsizetype headerEnd = buffer.indexOf("\r\n\r\n");
    if (headerEnd < 0) {
        if (buffer.size() > kMaxHeaderBytes) {
            r.state = Request::State::Bad;
            r.status = 431;
            r.reason = "Request Header Fields Too Large";
        }
        return r;
    }
    if (headerEnd > kMaxHeaderBytes) {
        r.state = Request::State::Bad;
        r.status = 431;
        r.reason = "Request Header Fields Too Large";
        return r;
    }
    r.headersComplete = true;
    const QList<QByteArray> lines = buffer.left(headerEnd).split('\n');
    const QList<QByteArray> requestLine = lines.value(0).trimmed().split(' ');
    if (requestLine.size() != 3 || !requestLine.at(2).startsWith("HTTP/1.")) {
        r.state = Request::State::Bad;
        r.reason = "Bad Request";
        return r;
    }
    r.method = requestLine.at(0);
    r.path = requestLine.at(1);
    qsizetype contentLength = -1;
    for (qsizetype i = 1; i < lines.size(); ++i) {
        const QByteArray line = lines.at(i).trimmed();
        const qsizetype colon = line.indexOf(':');
        if (colon <= 0)
            continue;
        const QByteArray name = line.left(colon).trimmed().toLower();
        const QByteArray value = line.mid(colon + 1).trimmed();
        if (name == "content-length") {
            bool ok = false;
            contentLength = value.toLongLong(&ok);
            if (!ok || contentLength < 0) {
                r.state = Request::State::Bad;
                r.reason = "Bad Content-Length";
                return r;
            }
        } else if (name == "transfer-encoding" && value.toLower() != "identity") {
            r.state = Request::State::Bad;
            r.status = 501;
            r.reason = "Not Implemented";
            return r;
        } else if (name == "expect" && value.toLower() == "100-continue") {
            r.expectContinue = true;
        }
    }
    if (r.method != "POST") {
        r.state = Request::State::Bad;
        r.status = 405;
        r.reason = "Method Not Allowed";
        return r;
    }
    if (contentLength < 0) {
        r.state = Request::State::Bad;
        r.status = 411;
        r.reason = "Length Required";
        return r;
    }
    if (contentLength > maxBodyBytes) {
        r.state = Request::State::Bad;
        r.status = 413;
        r.reason = "Payload Too Large";
        return r;
    }
    const qsizetype bodyStart = headerEnd + 4;
    if (buffer.size() - bodyStart < contentLength)
        return r;   // headers parsed, body still arriving
    r.body = buffer.mid(bodyStart, contentLength);
    r.consumed = bodyStart + contentLength;
    r.state = Request::State::Complete;
    return r;
}

/// A complete HTTP/1.1 response that closes the connection.
inline QByteArray response(int status, const QByteArray &reason, const QByteArray &body,
                           const QByteArray &contentType = "text/xml")
{
    QByteArray out;
    out += "HTTP/1.1 " + QByteArray::number(status) + ' ' + reason + "\r\n";
    out += "Content-Type: " + contentType + "\r\n";
    out += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    out += "Connection: close\r\n\r\n";
    out += body;
    return out;
}

}  // namespace SampHttp

/// The callback endpoint. Lives on whatever thread created it (the SAMP
/// worker thread in the application).
class SampCallbackServer : public QObject
{
    Q_OBJECT

public:
    /// Turns a decoded XML-RPC call into the XML-RPC response body.
    using Handler = std::function<QByteArray(const SampXmlRpc::Call &)>;

    /// The largest request accepted. A table.select.rowList of a large
    /// selection is the biggest thing a hub sends: about 40 bytes of XML per row.
    static constexpr qsizetype kMaxBodyBytes = 64 * 1024 * 1024;
    /// A connection that has not sent a whole request by then is dropped.
    static constexpr int kIdleTimeoutMs = 15000;
    /// Requests are read before the private key in them can be checked, so
    /// what unauthenticated peers can make the server hold is bounded: this
    /// many connections at once, this many bytes buffered across them.
    static constexpr int kMaxConnections = 16;
    static constexpr qsizetype kMaxBufferedBytes = 2 * kMaxBodyBytes;

    explicit SampCallbackServer(QObject *parent = nullptr);
    ~SampCallbackServer() override;

    void setHandler(Handler handler) { m_handler = std::move(handler); }

    /// Listen on 127.0.0.1 and a port the system picks. False (and @p error)
    /// when the socket cannot be opened.
    bool listen(QString *error = nullptr);
    void close();
    bool isListening() const;

    /// The URL to give samp.hub.setXmlrpcCallback (empty when not listening).
    QString url() const;

private:
    void onNewConnection();
    void onReadyRead(QTcpSocket *socket);
    void reply(QTcpSocket *socket, const QByteArray &httpResponse);

    QTcpServer *m_server{ nullptr };
    Handler m_handler;
    QHash<QTcpSocket *, QByteArray> m_buffers;
    QHash<QTcpSocket *, bool> m_continueSent;
    qsizetype m_bufferedBytes{ 0 };   ///< sum of m_buffers' sizes
};

#endif  // SAMPCALLBACKSERVER_H
