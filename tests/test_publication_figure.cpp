// test_publication_figure.cpp
// See TestPublicationFigure.h.

#include "TestPublicationFigure.h"

#include "app/BackendClient.h"
#include "PublicationFigureSpec.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>

#include <cmath>

using namespace Qt::StringLiterals;
namespace vf = visivo::figure;

namespace {

/// A backend that answers POST /v1/products/publication_figure with @p body and
/// keeps what it was sent.
class FigureServer : public QObject
{
public:
    explicit FigureServer(QByteArray body) : body(std::move(body))
    {
        server.listen(QHostAddress::LocalHost);
        QObject::connect(&server, &QTcpServer::newConnection, this, [this]() {
            while (QTcpSocket *s = server.nextPendingConnection()) {
                auto *buffer = new QByteArray;
                QObject::connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
                QObject::connect(s, &QObject::destroyed, s, [buffer]() { delete buffer; });
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
                    const bool route = line.startsWith("POST /v1/products/publication_figure ");
                    if (route)
                        requestBody = buffer->mid(end + 4, length);
                    buffer->clear();
                    const QByteArray payload =
                            route ? this->body : QByteArray(R"({"detail":"Not Found"})");
                    s->write(QByteArray("HTTP/1.1 ") + (route ? "200" : "404") + " X\r\n"
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
    QByteArray body;
    QByteArray requestBody;
};

QJsonObject body(const vf::Options &o, const vf::Overlays &ov = {}, bool preview = false)
{
    return vf::requestBody(u"ds1"_s, o, ov, preview, u"field_figure"_s);
}

visivo::contours::LineSource square(double level, const QString &colour, bool negativeDash)
{
    // One closed line of four segments: 0-1-2-3-0, in source pixels.
    visivo::contours::LineSource s;
    s.points = { { 10, 10 }, { 20, 10 }, { 20, 20 }, { 10, 20 } };
    s.level = { level, level, level, level };
    s.segments = { 0, 1, 1, 2, 2, 3, 3, 0 };
    s.levels = { level };
    s.colours = { colour };
    s.dashNegative = negativeDash;
    s.lineWidth = 2.5;
    return s;
}

}   // namespace

void TestPublicationFigure::defaultsAreTheOneClickFigure()
{
    const QJsonObject b = body({});
    QCOMPARE(b.value(u"dataset_id"_s).toString(), u"ds1"_s);
    QCOMPARE(b.value(u"cmap"_s).toString(), u"Inferno"_s);
    QCOMPARE(b.value(u"stretch"_s).toString(), u"linear"_s);
    QCOMPARE(b.value(u"percent"_s).toDouble(), 99.5);
    QCOMPARE(b.value(u"dpi"_s).toInt(), 200);
    QCOMPARE(b.value(u"width_in"_s).toDouble(), 8.0);
    QCOMPARE(b.value(u"height_in"_s).toDouble(), 6.5);
    QVERIFY(b.value(u"add_grid"_s).toBool());
    QVERIFY(b.value(u"add_colorbar"_s).toBool());
    QVERIFY(b.value(u"crop"_s).toBool());
    QCOMPARE(b.value(u"title"_s).toString(), QString());
    QCOMPARE(b.value(u"frame"_s).toString(), u"native"_s);
    // Nothing is added to the figure the tool always made.
    QVERIFY(!b.value(u"beam"_s).toBool());
    QVERIFY(!b.value(u"scale_bar"_s).toBool());
    QVERIFY(!b.value(u"compass"_s).toBool());
    QVERIFY(b.value(u"regions"_s).toArray().isEmpty());
    QVERIFY(!b.contains(u"vmin"_s));
    QCOMPARE(b.value(u"output_basename"_s).toString(), u"field_figure.png"_s);
}

void TestPublicationFigure::everyOptionReachesTheBody()
{
    vf::Options o;
    o.cmap = u"Blues_r"_s;
    o.stretch = u"power"_s;
    o.gamma = 2.5;
    o.percent = 98.0;
    o.title = u"NGC 5135"_s;
    o.xlabel = u"RA"_s;
    o.ylabel = u"Dec"_s;
    o.fontFamily = u"serif"_s;
    o.titleSize = 18;
    o.labelSize = 14;
    o.tickSize = 9;
    o.colorbarLabelSize = 12;
    o.grid = false;
    o.frame = u"galactic"_s;
    o.tickFormat = u"degrees"_s;
    o.colorbar = true;
    o.colorbarPosition = u"bottom"_s;
    o.colorbarLabel = u"Jy/beam"_s;
    o.beam = true;
    o.beamPosition = u"top-left"_s;
    o.scaleBar = true;
    o.scaleBarLength = 10.0;
    o.scaleBarUnit = u"arcmin"_s;
    o.scaleBarPosition = u"top-right"_s;
    o.compass = true;
    o.compassPosition = u"bottom-right"_s;
    o.widthIn = 5.5;
    o.heightIn = 4.0;
    o.dpi = 300;
    o.crop = false;
    o.format = u"pdf"_s;
    const QJsonObject b = body(o);
    QCOMPARE(b.value(u"cmap"_s).toString(), u"Blues_r"_s);
    QCOMPARE(b.value(u"stretch"_s).toString(), u"power"_s);
    QCOMPARE(b.value(u"gamma"_s).toDouble(), 2.5);
    QCOMPARE(b.value(u"percent"_s).toDouble(), 98.0);
    QCOMPARE(b.value(u"title"_s).toString(), u"NGC 5135"_s);
    QCOMPARE(b.value(u"xlabel"_s).toString(), u"RA"_s);
    QCOMPARE(b.value(u"ylabel"_s).toString(), u"Dec"_s);
    QCOMPARE(b.value(u"font_family"_s).toString(), u"serif"_s);
    QCOMPARE(b.value(u"title_size"_s).toDouble(), 18.0);
    QCOMPARE(b.value(u"label_size"_s).toDouble(), 14.0);
    QCOMPARE(b.value(u"tick_size"_s).toDouble(), 9.0);
    QCOMPARE(b.value(u"colorbar_label_size"_s).toDouble(), 12.0);
    QVERIFY(!b.value(u"add_grid"_s).toBool());
    QCOMPARE(b.value(u"frame"_s).toString(), u"galactic"_s);
    QCOMPARE(b.value(u"tick_format"_s).toString(), u"degrees"_s);
    QCOMPARE(b.value(u"colorbar_position"_s).toString(), u"bottom"_s);
    QCOMPARE(b.value(u"colorbar_label"_s).toString(), u"Jy/beam"_s);
    QVERIFY(b.value(u"beam"_s).toBool());
    QCOMPARE(b.value(u"beam_position"_s).toString(), u"top-left"_s);
    QVERIFY(b.value(u"scale_bar"_s).toBool());
    QCOMPARE(b.value(u"scale_bar_length"_s).toDouble(), 10.0);
    QCOMPARE(b.value(u"scale_bar_unit"_s).toString(), u"arcmin"_s);
    QCOMPARE(b.value(u"scale_bar_position"_s).toString(), u"top-right"_s);
    QVERIFY(b.value(u"compass"_s).toBool());
    QCOMPARE(b.value(u"compass_position"_s).toString(), u"bottom-right"_s);
    QCOMPARE(b.value(u"width_in"_s).toDouble(), 5.5);
    QCOMPARE(b.value(u"height_in"_s).toDouble(), 4.0);
    QCOMPARE(b.value(u"dpi"_s).toInt(), 300);
    QVERIFY(!b.value(u"crop"_s).toBool());
    QCOMPARE(b.value(u"output_basename"_s).toString(), u"field_figure.pdf"_s);
}

void TestPublicationFigure::theWindowsRangeIsSentOnlyWhenUsed()
{
    vf::Options o;
    o.vmin = -0.1;
    o.vmax = 2.0;
    QVERIFY(!body(o).contains(u"vmin"_s));   // not asked for
    o.useRange = true;
    QCOMPARE(body(o).value(u"vmin"_s).toDouble(), -0.1);
    QCOMPARE(body(o).value(u"vmax"_s).toDouble(), 2.0);
    o.vmax = -1.0;                            // an inverted range is the backend's to refuse, not sent
    QVERIFY(!body(o).contains(u"vmax"_s));
}

void TestPublicationFigure::aPreviewHasNoFileAndAFigureHasItsFormat()
{
    vf::Options o;
    o.format = u"svg"_s;
    const QJsonObject preview = vf::requestBody(u"ds1"_s, o, {}, true, u"x"_s, 640);
    QVERIFY(preview.value(u"preview"_s).toBool());
    QCOMPARE(preview.value(u"preview_px"_s).toInt(), 640);
    QVERIFY(!preview.contains(u"output_basename"_s));
    const QJsonObject figure = vf::requestBody(u"ds1"_s, o, {}, false, u"x"_s);
    QVERIFY(!figure.value(u"preview"_s).toBool());
    QCOMPARE(figure.value(u"output_basename"_s).toString(), u"x.svg"_s);
    QCOMPARE(vf::outputName(QString(), QString()), u"figure.png"_s);
}

void TestPublicationFigure::overlaysAreSentOnlyWhenChosen()
{
    vf::Overlays ov;
    ov.contourSources.push_back(square(1.0, u"#ff0000"_s, true));
    ov.regions = QJsonArray{ QJsonObject{ { "shape", "circle" } } };
    ov.annotations = QJsonArray{ QJsonObject{ { "shape", "text" } }, QJsonObject{ { "shape", "vector" } } };
    vf::Options o;
    QCOMPARE(body(o, ov).value(u"regions"_s).toArray().size(), 0);
    o.annotations = true;
    QCOMPARE(body(o, ov).value(u"regions"_s).toArray().size(), 2);
    o.regions = true;
    QCOMPARE(body(o, ov).value(u"regions"_s).toArray().size(), 3);
    o.contours = true;
    QCOMPARE(body(o, ov).value(u"regions"_s).toArray().size(), 4);   // one closed contour line
}

void TestPublicationFigure::annotationsAreCanonicalTextAndVectors()
{
    const QJsonArray a = vf::noteRegions({ { false, u"Nucleus"_s, 9, 19, 0, 0 },
                                           { true, u"jet"_s, 10, 20, 13, 24 },
                                           { true, QString(), 5, 5, 5, 5 } });   // a zero-length arrow: nothing
    QCOMPARE(a.size(), 3);   // the text, the arrow and its text
    const QJsonObject text = a.at(0).toObject();
    QCOMPARE(text.value(u"shape"_s).toString(), u"text"_s);
    QCOMPARE(text.value(u"frame"_s).toString(), u"image"_s);
    QCOMPARE(text.value(u"center"_s).toArray().at(0).toDouble(), 10.0);   // 0-based window pixels, 1-based file pixels
    QCOMPARE(text.value(u"center"_s).toArray().at(1).toDouble(), 20.0);
    QCOMPARE(text.value(u"text"_s).toString(), u"Nucleus"_s);
    const QJsonObject arrow = a.at(1).toObject();
    QCOMPARE(arrow.value(u"shape"_s).toString(), u"vector"_s);
    QCOMPARE(arrow.value(u"length"_s).toDouble(), 5.0);                   // from the label to the tip
    QVERIFY(std::abs(arrow.value(u"angle_deg"_s).toDouble() - std::atan2(4.0, 3.0) * 180 / M_PI) < 1e-9);
    QCOMPARE(a.at(2).toObject().value(u"text"_s).toString(), u"jet"_s);
}

void TestPublicationFigure::hiddenRegionsStayOut()
{
    visivo::regions::Region shown, hidden;
    shown.shape = hidden.shape = visivo::regions::Shape::Circle;
    shown.center = { 4, 5 };
    shown.radius = 3;
    shown.color = u"#00ff00"_s;
    hidden.visible = false;
    const QJsonArray r = vf::regionRegions({ shown, hidden });
    QCOMPARE(r.size(), 1);
    const QJsonObject c = r.at(0).toObject();
    QCOMPARE(c.value(u"shape"_s).toString(), u"circle"_s);
    QCOMPARE(c.value(u"center"_s).toArray().at(0).toDouble(), 5.0);   // 1-based
    QCOMPARE(c.value(u"props"_s).toObject().value(u"color"_s).toString(), u"#00ff00"_s);
}

void TestPublicationFigure::contourLinesKeepTheirColourWidthAndDash()
{
    const QJsonArray lines = vf::contourRegions({ square(-1.0, u"#0000ff"_s, true),
                                                  square(2.0, u"#ff0000"_s, true) });
    QCOMPARE(lines.size(), 2);
    const QJsonObject neg = lines.at(0).toObject();
    QCOMPARE(neg.value(u"shape"_s).toString(), u"polygon"_s);
    QCOMPARE(neg.value(u"points"_s).toArray().size(), 4);
    QCOMPARE(neg.value(u"points"_s).toArray().at(0).toArray().size(), 2);
    const QJsonObject np = neg.value(u"props"_s).toObject();
    QCOMPARE(np.value(u"color"_s).toString(), u"#0000ff"_s);
    QCOMPARE(np.value(u"width"_s).toDouble(), 2.5);
    QVERIFY(np.value(u"dash"_s).toBool());                            // a negative level is dashed
    QVERIFY(!lines.at(1).toObject().value(u"props"_s).toObject().contains(u"dash"_s));
}

void TestPublicationFigure::contourLinesAreJoinedOnceAndKept()
{
    vf::Overlays ov;
    ov.contourSources.push_back(square(1.0, u"#ff0000"_s, false));
    const QJsonArray first = ov.contours();
    ov.contourSources.clear();   // the cache, not the sources, answers from here
    QCOMPARE(ov.contours(), first);
    QVERIFY(!first.isEmpty());
}

void TestPublicationFigure::colourMapsOfTheWindowMapToTheBackendsOrInferno()
{
    // What the backend answered for the viewer's registry: the dialog's list.
    const QStringList available = vf::orderedColourMaps(
            { u"Purples"_s, u"Glow"_s, u"Blues_r"_s, u"Viridis"_s, u"CoolWarm"_s, u"Inferno"_s });
    // The uniform maps first, the rest alphabetically.
    QCOMPARE(available, (QStringList{ u"Inferno"_s, u"Viridis"_s, u"CoolWarm"_s, u"Blues_r"_s,
                                      u"Glow"_s, u"Purples"_s }));
    QCOMPARE(vf::colourMapFor(u"CoolWarm"_s, available), u"CoolWarm"_s);
    QCOMPARE(vf::colourMapFor(u"coolwarm"_s, available), u"CoolWarm"_s);
    QCOMPARE(vf::colourMapFor(u"Glow"_s, available), u"Glow"_s);      // the backend's stand-in
    QCOMPARE(vf::colourMapFor(u"Purples"_s, available), u"Purples"_s);
    QCOMPARE(vf::colourMapFor(u"Rainbow"_s, available), u"Inferno"_s);   // not offered: Inferno
    QCOMPARE(vf::colourMapFor(u"MyLut"_s, available), u"Inferno"_s);
    QCOMPARE(vf::colourMapFor(QString(), available), u"Inferno"_s);
    QVERIFY(vf::orderedColourMaps({}).isEmpty());
}

void TestPublicationFigure::theWindowsFrameIsTheFiguresFrame()
{
    QCOMPARE(vf::frameForWindow(false, false), u"native"_s);
    QCOMPARE(vf::frameForWindow(true, false), u"galactic"_s);
    QCOMPARE(vf::frameForWindow(false, true), u"ecliptic"_s);
    // The body carries it and the default scale bar unit is the backend's automatic one.
    vf::Options o;
    o.frame = vf::frameForWindow(false, true);
    QCOMPARE(body(o).value(u"frame"_s).toString(), u"ecliptic"_s);
    QCOMPARE(body(vf::Options{}).value(u"scale_bar_unit"_s).toString(), u"auto"_s);
}

void TestPublicationFigure::formatsAreThoseTheBackendWrites()
{
    QStringList keys;
    for (const auto &f : vf::formats())
        keys << QString::fromLatin1(f.key);
    QCOMPARE(keys, (QStringList{ u"png"_s, u"pdf"_s, u"svg"_s, u"eps"_s }));
}

void TestPublicationFigure::theClientPostsTheBodyAndReadsThePreview()
{
    const QByteArray png = QByteArray("\x89PNG\r\n\x1a\nxyz", 11);
    FigureServer backend(QJsonDocument(QJsonObject{
            { "valid", true }, { "has_wcs", true }, { "width", 200 }, { "height", 160 },
            { "format", "png" }, { "regions_drawn", 4 },
            { "notes", QJsonArray{ u"Beam: the header has no BMAJ/BMIN."_s } },
            { "png_base64", QString::fromLatin1(png.toBase64()) } }).toJson());
    QVERIFY(backend.server.isListening());
    const BackendClient client(backend.url(), u"tok"_s);
    vf::Options o;
    o.beam = true;
    const BackendPublicationFigureResult r = client.publicationFigure(body(o, {}, true));
    QVERIFY2(r.valid, qPrintable(r.error));
    QCOMPARE(r.png, png);
    QCOMPARE(r.regionsDrawn, 4);
    QCOMPARE(r.notes.size(), 1);
    QCOMPARE(r.width, 200);
    const QJsonObject sent = QJsonDocument::fromJson(backend.requestBody).object();
    QVERIFY(sent.value(u"preview"_s).toBool());
    QVERIFY(sent.value(u"beam"_s).toBool());
    QCOMPARE(sent.value(u"dataset_id"_s).toString(), u"ds1"_s);
}

void TestPublicationFigure::theClientReportsARefusal()
{
    FigureServer backend(R"({"valid": false, "error": "Font 'Nope' is not installed on the backend."})");
    const BackendClient client(backend.url(), u"tok"_s);
    const BackendPublicationFigureResult r = client.publicationFigure(body({}));
    QVERIFY(!r.valid);
    QVERIFY(r.error.contains(u"not installed"_s));
    QVERIFY(r.png.isEmpty());
}
