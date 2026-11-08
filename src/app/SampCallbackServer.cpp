// SampCallbackServer.cpp – loopback HTTP endpoint for SAMP hub callbacks

#include "SampCallbackServer.h"

#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include <algorithm>

SampCallbackServer::SampCallbackServer(QObject *parent)
    : QObject(parent)
    , m_server(new QTcpServer(this))
{
    connect(m_server, &QTcpServer::newConnection, this, &SampCallbackServer::onNewConnection);
}

SampCallbackServer::~SampCallbackServer()
{
    close();
}

bool SampCallbackServer::listen(QString *error)
{
    if (m_server->isListening())
        return true;
    // Loopback only: the hub runs on this machine (Standard Profile), and
    // nothing else has any business calling a SAMP client.
    if (!m_server->listen(QHostAddress::LocalHost, 0)) {
        if (error)
            *error = m_server->errorString();
        return false;
    }
    return true;
}

void SampCallbackServer::close()
{
    if (m_server->isListening())
        m_server->close();
    // Sockets are children of the server; dropping them closes them.
    const auto sockets = m_buffers.keys();
    m_buffers.clear();
    m_bufferedBytes = 0;
    m_continueSent.clear();
    for (QTcpSocket *socket : sockets) {
        socket->disconnect(this);
        socket->abort();
        socket->deleteLater();
    }
}

bool SampCallbackServer::isListening() const
{
    return m_server->isListening();
}

QString SampCallbackServer::url() const
{
    if (!m_server->isListening())
        return {};
    return QStringLiteral("http://127.0.0.1:%1/xmlrpc").arg(m_server->serverPort());
}

void SampCallbackServer::onNewConnection()
{
    while (QTcpSocket *socket = m_server->nextPendingConnection()) {
        if (m_buffers.size() >= kMaxConnections) {
            socket->abort();
            socket->deleteLater();
            continue;
        }
        // The kernel keeps what is not read yet; Qt reads no more than this
        // ahead of the parser.
        socket->setReadBufferSize(256 * 1024);
        m_buffers.insert(socket, QByteArray());
        connect(socket, &QTcpSocket::readyRead, this, [this, socket]() { onReadyRead(socket); });
        connect(socket, &QTcpSocket::disconnected, this, [this, socket]() {
            m_bufferedBytes -= m_buffers.value(socket).size();
            m_buffers.remove(socket);
            m_continueSent.remove(socket);
            socket->deleteLater();
        });
        // A peer that opens a connection and never finishes its request must
        // not hold the socket for ever. The timer belongs to the socket, so it
        // dies with it.
        auto *idle = new QTimer(socket);
        idle->setSingleShot(true);
        connect(idle, &QTimer::timeout, socket, [socket]() { socket->abort(); });
        idle->start(kIdleTimeoutMs);
    }
}

void SampCallbackServer::onReadyRead(QTcpSocket *socket)
{
    auto it = m_buffers.find(socket);
    if (it == m_buffers.end())
        return;   // already answered
    // Read no more than one request may hold, and no more than all the
    // connections together may: past either, the request is refused unread.
    const qsizetype allowance = std::min(kMaxBodyBytes + SampHttp::kMaxHeaderBytes - it->size(),
                                         kMaxBufferedBytes - m_bufferedBytes);
    if (socket->bytesAvailable() > allowance) {
        reply(socket, SampHttp::response(413, "Payload Too Large", "Payload Too Large\n",
                                         "text/plain"));
        return;
    }
    const QByteArray chunk = socket->readAll();
    it->append(chunk);
    m_bufferedBytes += chunk.size();

    const SampHttp::Request request = SampHttp::parseRequest(*it, kMaxBodyBytes);
    switch (request.state) {
    case SampHttp::Request::State::Incomplete:
        if (request.headersComplete && request.expectContinue
            && !m_continueSent.value(socket, false)) {
            socket->write("HTTP/1.1 100 Continue\r\n\r\n");
            m_continueSent.insert(socket, true);
        }
        return;
    case SampHttp::Request::State::Bad:
        reply(socket, SampHttp::response(request.status, request.reason, request.reason + '\n',
                                         "text/plain"));
        return;
    case SampHttp::Request::State::Complete:
        break;
    }

    const SampXmlRpc::Call call = SampXmlRpc::parseMethodCall(request.body);
    QByteArray body;
    if (!call.isValid())
        body = SampXmlRpc::faultResponse(1, QStringLiteral("Malformed XML-RPC call: ") + call.error);
    else if (m_handler)
        body = m_handler(call);
    else
        body = SampXmlRpc::faultResponse(1, QStringLiteral("Client not ready"));
    reply(socket, SampHttp::response(200, "OK", body));
}

void SampCallbackServer::reply(QTcpSocket *socket, const QByteArray &httpResponse)
{
    // One request per connection: stop reading, answer, close once written.
    m_bufferedBytes -= m_buffers.value(socket).size();
    m_buffers.remove(socket);
    m_continueSent.remove(socket);
    socket->write(httpResponse);
    socket->disconnectFromHost();
}
