#include "TestFilePreview.h"

#include "app/BackendClient.h"
#include "app/FilePreview.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtTest>

using namespace Qt::StringLiterals;

namespace {

/// Answers POST /v1/files/preview with a canned status and body, and every
/// other path (the session mint) with FastAPI's own 404. Records the preview
/// request's body.
class PreviewServer : public QObject
{
public:
    PreviewServer(int status, QByteArray body) : status(status), body(std::move(body))
    {
        server.listen(QHostAddress::LocalHost);
        QObject::connect(&server, &QTcpServer::newConnection, this, [this]() {
            while (QTcpSocket *s = server.nextPendingConnection()) {
                auto *buffer = new QByteArray;
                QObject::connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
                QObject::connect(s, &QTcpSocket::readyRead, this, [this, s, buffer]() {
                    *buffer += s->readAll();
                    const qsizetype end = buffer->indexOf("\r\n\r\n");
                    if (end < 0)
                        return;
                    const QByteArray head = buffer->left(end).toLower();
                    qsizetype length = 0;
                    const qsizetype at = head.indexOf("content-length:");
                    if (at >= 0)
                        length = head.mid(at + 15, head.indexOf("\r\n", at) - at - 15)
                                         .trimmed()
                                         .toLongLong();
                    if (buffer->size() < end + 4 + length)
                        return;
                    const QByteArray line = buffer->left(buffer->indexOf("\r\n"));
                    const bool preview = line.startsWith("POST /v1/files/preview ");
                    if (preview)
                        requestBody = buffer->mid(end + 4, length);
                    delete buffer;
                    const int code = preview ? this->status : 404;
                    const QByteArray payload =
                            preview ? this->body : QByteArray(R"({"detail":"Not Found"})");
                    s->write("HTTP/1.1 " + QByteArray::number(code) + " X\r\n"
                             "Content-Type: application/json\r\nContent-Length: "
                             + QByteArray::number(payload.size())
                             + "\r\nConnection: close\r\n\r\n" + payload);
                    s->disconnectFromHost();
                });
            }
        });
    }
    QString url() const { return u"http://127.0.0.1:%1"_s.arg(server.serverPort()); }

    QTcpServer server;
    int status;
    QByteArray body;
    QByteArray requestBody;
};

}  // namespace

void TestFilePreview::parsesATextPreview()
{
    const visivo::FilePreview p = visivo::parseFilePreviewReply(
            R"({"path":"/d/a.csv","size_bytes":123456,"kind":"text","format":"CSV table",)"
            R"("text":"ra,dec\n1,2\n","truncated":true})");
    QVERIFY(p.valid);
    QCOMPARE(p.path, u"/d/a.csv"_s);
    QCOMPARE(p.sizeBytes, qint64(123456));
    QCOMPARE(p.kind, u"text"_s);
    QCOMPARE(p.format, u"CSV table"_s);
    QCOMPARE(p.text, u"ra,dec\n1,2\n"_s);
    QVERIFY(p.truncated);
    QVERIFY(!p.routeMissing);
}

void TestFilePreview::aReplyWithoutKindIsAnError()
{
    const visivo::FilePreview p =
            visivo::parseFilePreviewReply(R"({"error":"Path is outside the data root"})");
    QVERIFY(!p.valid);
    QCOMPARE(p.error, u"Path is outside the data root"_s);

    // No reason given: a sentence, not the raw JSON.
    const visivo::FilePreview bare = visivo::parseFilePreviewReply(R"({"path":"/x"})");
    QVERIFY(!bare.valid);
    QVERIFY(!bare.error.isEmpty());
    QVERIFY(!bare.error.startsWith(u'{'));
}

void TestFilePreview::notJsonIsAnError()
{
    const visivo::FilePreview p = visivo::parseFilePreviewReply("<html>oops</html>");
    QVERIFY(!p.valid);
    QVERIFY(!p.error.isEmpty());
}

void TestFilePreview::onlyFastApisOwn404MeansAnOldBackend()
{
    QVERIFY(visivo::isMissingRouteReply(404, R"({"detail":"Not Found"})"));
    QVERIFY(visivo::isMissingRouteReply(405, R"({"detail":"Method Not Allowed"})"));
    // The route is there and says the FILE is not: no fallback.
    QVERIFY(!visivo::isMissingRouteReply(404, R"({"detail":"File not found: /d/a.csv"})"));
    QVERIFY(!visivo::isMissingRouteReply(500, R"({"detail":"Not Found"})"));
    QVERIFY(!visivo::isMissingRouteReply(0, QByteArray()));
    // A proxy or tunnel before an old backend: HTML, or nothing at all.
    QVERIFY(visivo::isMissingRouteReply(404, "<html><body>404 Not Found</body></html>"));
    QVERIFY(visivo::isMissingRouteReply(404, QByteArray()));
    QVERIFY(visivo::isMissingRouteReply(405, R"({"message":"nope"})"));
    // A 422 is the route's own (bad arguments), never an old backend.
    QVERIFY(!visivo::isMissingRouteReply(422, QByteArray()));
}

void TestFilePreview::fallbackOnAnOldBackend()
{
    using visivo::PreviewFallback;
    QCOMPARE(visivo::previewFallback(false, true, false), PreviewFallback::None);
    QCOMPARE(visivo::previewFallback(false, false, true), PreviewFallback::None);
    QCOMPARE(visivo::previewFallback(true, true, false), PreviewFallback::Folder);
    QCOMPARE(visivo::previewFallback(true, false, true), PreviewFallback::FitsHeader);
    QCOMPARE(visivo::previewFallback(true, false, false), PreviewFallback::Unsupported);

    const visivo::FilePreview folder = visivo::folderPlaceholder(u"/d"_s);
    QVERIFY(folder.valid);
    QCOMPARE(visivo::filePreviewBody(folder), u"Folder selected."_s);
    const visivo::FilePreview other = visivo::unsupportedOnOldBackend(u"/d/a.csv"_s);
    QVERIFY(!other.valid);
    QVERIFY(other.error.contains(u"FITS headers only"_s));
}

void TestFilePreview::captionNamesTheFormatAndTruncation()
{
    visivo::FilePreview p;
    p.valid = true;
    p.kind = u"vbt"_s;
    p.format = u"VisIVO Binary Table header"_s;
    QCOMPARE(visivo::filePreviewCaption(p), u"VisIVO Binary Table header"_s);
    p.truncated = true;
    QVERIFY(visivo::filePreviewCaption(p).startsWith(u"VisIVO Binary Table header · "_s));
    QVERIFY(visivo::filePreviewCaption(p).contains(u"start of the file"_s));

    visivo::FilePreview noFormat;
    noFormat.kind = u"binary"_s;
    QCOMPARE(visivo::filePreviewCaption(noFormat), u"Binary file"_s);
}

void TestFilePreview::bodyExplainsFilesWithNoText()
{
    visivo::FilePreview p;
    p.kind = u"binary"_s;
    QVERIFY(visivo::filePreviewBody(p).contains(u"no text preview"_s));
    p.kind = u"compressed"_s;
    QVERIFY(visivo::filePreviewBody(p).startsWith(u"Compressed"_s));
    p.text = u"SIMPLE  =                    T"_s;
    QCOMPARE(visivo::filePreviewBody(p), p.text);
}

void TestFilePreview::headerCardsBecomeAFitsPreview()
{
    const visivo::FilePreview p =
            visivo::previewFromHeaderCards(u"/d/c.fits"_s, { u"SIMPLE = T"_s, u"NAXIS = 3"_s });
    QVERIFY(p.valid);
    QCOMPARE(p.kind, u"fits"_s);
    QCOMPARE(p.text, u"SIMPLE = T\nNAXIS = 3"_s);
    QVERIFY(!p.truncated);
}

void TestFilePreview::clientReadsThePreviewRoute()
{
    PreviewServer backend(200, R"({"path":"/d/a.tsv","size_bytes":10,"kind":"text",)"
                               R"("format":"TSV table","text":"a\tb","truncated":false})");
    QVERIFY(backend.server.isListening());
    const BackendClient client(backend.url(), u"tok"_s);
    const visivo::FilePreview p = client.filePreview(u"/d/a.tsv"_s, 2048, 12);
    QVERIFY2(p.valid, qPrintable(p.error));
    QCOMPARE(p.format, u"TSV table"_s);
    QCOMPARE(p.text, u"a\tb"_s);

    // The caps go out with the path, as the route expects them.
    const QJsonObject sent = QJsonDocument::fromJson(backend.requestBody).object();
    QCOMPARE(sent.value(u"path").toString(), u"/d/a.tsv"_s);
    QCOMPARE(sent.value(u"max_bytes").toInt(), 2048);
    QCOMPARE(sent.value(u"max_lines").toInt(), 12);
}

void TestFilePreview::clientTellsAMissingRouteFromAMissingFile()
{
    {
        PreviewServer oldBackend(404, R"({"detail":"Not Found"})");
        const BackendClient client(oldBackend.url(), u"tok"_s);
        const visivo::FilePreview p = client.filePreview(u"/d/a.fits"_s);
        QVERIFY(!p.valid);
        QVERIFY(p.routeMissing);
    }
    {
        PreviewServer newBackend(404, R"({"detail":"File not found: /d/gone.csv"})");
        const BackendClient client(newBackend.url(), u"tok"_s);
        const visivo::FilePreview p = client.filePreview(u"/d/gone.csv"_s);
        QVERIFY(!p.valid);
        QVERIFY(!p.routeMissing);
        // The route's own reason, not Qt's "server replied: Not Found".
        QCOMPARE(p.error, u"File not found: /d/gone.csv"_s);
    }
}
