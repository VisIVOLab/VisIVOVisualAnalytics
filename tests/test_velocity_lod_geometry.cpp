#include "TestVelocityLodGeometry.h"

#include "gui/VelocityLodGeometry.h"
#include "app/BackendClient.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtTest>

#include <array>
#include <cmath>
#include <memory>
#include <vector>

using velocitylod::Axis;

namespace {

using Shape = std::array<int, 3>;   // (nx, ny, nz)

// A linear scalar on the world frame: a block mean of it is exactly its value
// at the block centre, so a misplaced reduced sample shows up as a mismatch.
double linearField(double x, double y, double z)
{
    return 3.0 + 0.7 * x - 1.3 * y + 2.1 * z;
}

// The backend's _block_average on an x-fastest grid: crop each axis to a
// multiple of its factor, then average each f_x * f_y * f_z block.
std::vector<double> blockAverage(const std::vector<double> &a, const Shape &n, const Shape &f,
                                 Shape &out)
{
    out = { n[0] / f[0], n[1] / f[1], n[2] / f[2] };
    std::vector<double> r(std::size_t(out[0]) * out[1] * out[2], 0.0);
    for (int k = 0; k < out[2]; ++k)
        for (int j = 0; j < out[1]; ++j)
            for (int i = 0; i < out[0]; ++i) {
                double sum = 0.0;
                for (int dk = 0; dk < f[2]; ++dk)
                    for (int dj = 0; dj < f[1]; ++dj)
                        for (int di = 0; di < f[0]; ++di) {
                            const int x = i * f[0] + di, y = j * f[1] + dj, z = k * f[2] + dk;
                            sum += a[(std::size_t(z) * n[1] + y) * n[0] + x];
                        }
                r[(std::size_t(k) * out[1] + j) * out[0] + i] = sum / (f[0] * f[1] * f[2]);
            }
    return r;
}

// Reduce a linear field sampled on the full grid and check every reduced
// sample against the field at the position the viewer gives it.
void checkLinearField(const Shape &full, int maxDim, double boxMpc)
{
    double cell[3], first[3];
    for (int a = 0; a < 3; ++a) {
        cell[a] = velocitylod::fullCell(full[a], boxMpc);
        first[a] = velocitylod::fullOrigin(full[a], cell[a]);
    }
    std::vector<double> values(std::size_t(full[0]) * full[1] * full[2]);
    for (int z = 0; z < full[2]; ++z)
        for (int y = 0; y < full[1]; ++y)
            for (int x = 0; x < full[0]; ++x)
                values[(std::size_t(z) * full[1] + y) * full[0] + x] = linearField(
                        first[0] + x * cell[0], first[1] + y * cell[1], first[2] + z * cell[2]);

    const Shape f = velocitylod::lodFactors(full, maxDim);
    Shape n;
    const std::vector<double> reduced = blockAverage(values, full, f, n);
    Axis ax[3];
    for (int a = 0; a < 3; ++a)
        ax[a] = velocitylod::reducedAxis(f[a], cell[a], first[a]);

    for (int k = 0; k < n[2]; ++k)
        for (int j = 0; j < n[1]; ++j)
            for (int i = 0; i < n[0]; ++i) {
                const double want = linearField(ax[0].origin + i * ax[0].spacing,
                                                ax[1].origin + j * ax[1].spacing,
                                                ax[2].origin + k * ax[2].spacing);
                const double got = reduced[(std::size_t(k) * n[1] + j) * n[0] + i];
                if (std::abs(got - want) > 1e-9 * (1.0 + std::abs(want)))
                    QFAIL(qPrintable(QStringLiteral("sample (%1,%2,%3): %4 vs %5")
                                             .arg(i).arg(j).arg(k).arg(got).arg(want)));
            }
}

bool near(double a, double b)
{
    return std::abs(a - b) <= 1e-12 * (1.0 + std::abs(a) + std::abs(b));
}

/// Answers one POST with an int32 label body and the given extra headers, and
/// keeps the request body so a test can read what the client sent.
class BasinsServer : public QObject
{
public:
    BasinsServer(QByteArray extraHeaders, QByteArray labels)
        : extra(std::move(extraHeaders)), body(std::move(labels))
    {
        server.listen(QHostAddress::LocalHost);
        QObject::connect(&server, &QTcpServer::newConnection, this, [this]() {
            while (QTcpSocket *s = server.nextPendingConnection()) {
                QObject::connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
                // One buffer per connection: the client may mint a session on
                // another connection first.
                auto received = std::make_shared<QByteArray>();
                QObject::connect(s, &QTcpSocket::readyRead, this, [this, s, received]() {
                    *received += s->readAll();
                    const qsizetype end = received->indexOf("\r\n\r\n");
                    if (end < 0)
                        return;
                    // Wait for the whole JSON body before answering.
                    qsizetype length = 0;
                    for (const QByteArray &line : received->left(end).split('\n')) {
                        if (line.toLower().startsWith("content-length:"))
                            length = line.mid(15).trimmed().toLongLong();
                    }
                    if (received->size() < end + 4 + length)
                        return;
                    if (received->startsWith("POST /v1/velocity/"))
                        requestBody = received->mid(end + 4, length);
                    s->write("HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
                             + extra + "Content-Length: " + QByteArray::number(body.size())
                             + "\r\nConnection: close\r\n\r\n" + body);
                    s->disconnectFromHost();
                });
            }
        });
    }
    QString url() const { return QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort()); }

    QTcpServer server;
    QByteArray extra;
    QByteArray body;
    QByteArray requestBody;   // the /v1/velocity/ request's JSON (not the session mint)
};

// A 2 x 2 x 1 label grid, little-endian int32.
QByteArray fourLabels()
{
    QByteArray b;
    for (qint32 v : { 1, 1, 2, 2 })
        b.append(reinterpret_cast<const char *>(&v), sizeof(v));
    return b;
}

const QByteArray kBasinShape = "X-Visivo-Width: 2\r\nX-Visivo-Height: 2\r\nX-Visivo-Depth: 1\r\n"
                               "X-Visivo-N-Basins: 2\r\n";

} // namespace

void TestVelocityLodGeometry::factorsMatchTheBackend()
{
    // Values from backend/app/velocity_field.py lod_factors, (x, y, z) order.
    QCOMPARE(velocitylod::lodFactors({ 512, 512, 512 }, 128), (Shape{ 4, 4, 4 }));
    QCOMPARE(velocitylod::lodFactors({ 129, 129, 129 }, 128), (Shape{ 2, 2, 2 }));
    QCOMPARE(velocitylod::lodFactors({ 128, 128, 128 }, 128), (Shape{ 1, 1, 1 }));
    QCOMPARE(velocitylod::lodFactors({ 300, 300, 5 }, 64), (Shape{ 5, 5, 2 }));
    QCOMPARE(velocitylod::lodFactors({ 300, 300, 300 }, 0), (Shape{ 1, 1, 1 }));
}

void TestVelocityLodGeometry::linearFieldReducedValuesSitAtTheirWorldPositions()
{
    checkLinearField({ 17, 12, 9 }, 5, 0.0);       // grid units, cropped axes
    checkLinearField({ 17, 12, 9 }, 5, 250.0);     // box in Mpc
    checkLinearField({ 16, 16, 16 }, 4, 100.0);    // exact multiples
    checkLinearField({ 20, 20, 3 }, 6, 80.0);      // thin z axis
}

void TestVelocityLodGeometry::reportedGeometryIsRescaledToTheViewerBox()
{
    // The backend opened the field without a box (unit cells); the viewer shows
    // it in a 200 Mpc box. 100 samples at factor 2: reported origin -49.5 + 0.5.
    const int full = 100, f = 2;
    const double backendCell = 1.0;
    const double reportedOrigin = velocitylod::fullOrigin(full, backendCell) + 0.5 * (f - 1);
    const double reportedSpacing = f * backendCell;

    const double cell = velocitylod::fullCell(full, 200.0);
    const double first = velocitylod::fullOrigin(full, cell);
    const Axis fromReport =
            velocitylod::reducedAxisFromReport(f, reportedOrigin, reportedSpacing, cell, first);
    const Axis fromFactor = velocitylod::reducedAxis(f, cell, first);
    QVERIFY(near(fromReport.origin, fromFactor.origin));
    QVERIFY(near(fromReport.spacing, fromFactor.spacing));
    QVERIFY(near(fromReport.origin, -100.0 + 0.5 * cell));

    // An unusable report falls back to the factors.
    const Axis bad = velocitylod::reducedAxisFromReport(f, reportedOrigin, 0.0, cell, first);
    QVERIFY(near(bad.origin, fromFactor.origin));
    QVERIFY(near(bad.spacing, fromFactor.spacing));
    const Axis nan = velocitylod::reducedAxisFromReport(f, std::nan(""), 1.0, cell, first);
    QVERIFY(near(nan.origin, fromFactor.origin));
}

void TestVelocityLodGeometry::thinGridKeepsItsShortAxis()
{
    // A (nz, ny, nx) = (2, 128, 128) grid: z cannot be reduced, x and y are.
    const Shape full{ 128, 128, 2 };
    const Shape f = velocitylod::lodFactors(full, 64);
    QCOMPARE(f, (Shape{ 2, 2, 1 }));
    const double cellZ = velocitylod::fullCell(2, 10.0);
    const Axis z = velocitylod::reducedAxis(f[2], cellZ, velocitylod::fullOrigin(2, cellZ));
    QVERIFY(near(z.spacing, 10.0));      // the two slices stay a box apart,
    QVERIFY(near(z.origin, -5.0));       // at -box/2 and +box/2
    checkLinearField(full, 64, 10.0);

    // The reviewer's case: (2, 512, 512) at the viewer's default 128 used to be
    // drawn with the z slices four full cells apart.
    QCOMPARE(velocitylod::lodFactors({ 512, 512, 2 }, 128), (Shape{ 4, 4, 1 }));
}

void TestVelocityLodGeometry::basinAndDensityGridsArePlacedAtBlockCentres()
{
    // A 100-sample axis segmented at 64: 50 basin samples at factor 2. They sit
    // at block centres, -49 .. +49, not stretched over -49.5 .. +49.5.
    const int full = 100, n = 50;
    const int f = velocitylod::inferAxisFactor(full, n, velocitylod::lodFactors({ full, full, full }, 64)[0]);
    QCOMPARE(f, 2);
    const double cell = velocitylod::fullCell(full, 0.0);
    const Axis a = velocitylod::reducedAxis(f, cell, velocitylod::fullOrigin(full, cell));
    QVERIFY(near(a.origin, -49.0));
    QVERIFY(near(a.spacing, 2.0));
    QVERIFY(near(a.origin + (n - 1) * a.spacing, 49.0));

    // The same axis as a density grid at 256 is not reduced: it lines up with
    // the full-resolution samples.
    const int fd = velocitylod::inferAxisFactor(full, full, velocitylod::lodFactors({ full, full, full }, 256)[0]);
    QCOMPARE(fd, 1);
    const Axis d = velocitylod::reducedAxis(fd, cell, velocitylod::fullOrigin(full, cell));
    QVERIFY(near(d.origin, -49.5));
    QVERIFY(near(d.spacing, 1.0));

    // A basin grid and a display grid of the same field share block centres
    // where their blocks coincide: factor 4 starts at -48 = the centre of two
    // factor-2 blocks.
    const Axis display4 = velocitylod::reducedAxis(4, cell, velocitylod::fullOrigin(full, cell));
    QVERIFY(near(display4.origin, 0.5 * (a.origin + a.origin + a.spacing)));
}

void TestVelocityLodGeometry::olderBackendSingleFactorIsInferredPerAxis()
{
    // An older backend sent one factor; a thin axis it did not reduce keeps 1.
    QCOMPARE(velocitylod::inferAxisFactor(512, 128, 4), 4);
    QCOMPARE(velocitylod::inferAxisFactor(2, 2, 4), 1);
    QCOMPARE(velocitylod::inferAxisFactor(130, 32, 4), 4);   // cropped: 130 // 4 = 32
    QCOMPARE(velocitylod::inferAxisFactor(0, 32, 4), 1);
}

void TestVelocityLodGeometry::backendProductsAreRecomputedOnlyWhenTheCellRatioChanges()
{
    // Cubic grid: cells stay equal on every axis whatever the box.
    const Shape cube{ 64, 64, 64 };
    QVERIFY(!velocitylod::cellRatioChanges(cube, 0.0, 200.0));
    QVERIFY(!velocitylod::cellRatioChanges(cube, 200.0, 0.0));
    QVERIFY(!velocitylod::cellRatioChanges(cube, 100.0, 300.0));
    QVERIFY(!velocitylod::cellRatioChanges(cube, 0.0, 0.0));

    // Non-cubic grid: unit cells (no box) and box/(N_i - 1) cells differ in
    // ratio, so the flow direction in cells, and the basins, change.
    const Shape slab{ 128, 128, 33 };
    QVERIFY(velocitylod::cellRatioChanges(slab, 0.0, 200.0));
    QVERIFY(velocitylod::cellRatioChanges(slab, 200.0, 0.0));
    // Between two boxes every cell scales by the same factor: labels survive.
    QVERIFY(!velocitylod::cellRatioChanges(slab, 100.0, 250.0));
    QVERIFY(!velocitylod::cellRatioChanges(slab, 0.0, 0.0));

    // That factor: 2.5 between two boxes, box/(N-1) from unit cells on a cube.
    QVERIFY(near(velocitylod::cellScale(slab, 100.0, 250.0), 2.5));
    QVERIFY(near(velocitylod::cellScale(cube, 0.0, 126.0), 2.0));
    // ∇·v of v = x in cells of 1 is 1; in cells of k it is 1/k. A rescaled
    // backend ∇·v must equal the one computed afresh in the new box.
    const double k = velocitylod::cellScale(cube, 0.0, 126.0);
    QVERIFY(near(1.0 / k, 1.0 / velocitylod::fullCell(64, 126.0)));
}

void TestVelocityLodGeometry::basinsFetchReadsTheGridGeometry()
{
    BasinsServer srv(kBasinShape
                             + "X-Visivo-Spacing: 2.0,2.0,1.0\r\n"
                               "X-Visivo-Origin: -49.0,-49.0,-0.5\r\n"
                               "X-Visivo-Downsample-Factors: 2,2,1\r\n",
                     fourLabels());
    QVERIFY(srv.server.isListening());
    BackendClient client(srv.url(), QStringLiteral("t"));
    const BackendVelocityBasinsResult r = client.fetchVelocityBasins(QStringLiteral("d"), 64, 1, 150.0);
    QVERIFY2(r.valid, qPrintable(r.error));
    QVERIFY(r.hasGeometry);
    QCOMPARE(r.factors[0], 2);
    QCOMPARE(r.factors[2], 1);
    QCOMPARE(r.spacing[0], 2.0);
    QCOMPARE(r.origin[0], -49.0);
    QCOMPARE(r.origin[2], -0.5);
    // The viewer's box goes with the request.
    QVERIFY(!srv.requestBody.isEmpty());
    QCOMPARE(QJsonDocument::fromJson(srv.requestBody).object().value(u"box_mpc").toDouble(), 150.0);
}

void TestVelocityLodGeometry::basinsFetchOfAnOlderBackendHasNoGeometry()
{
    // No geometry headers, or only some: the viewer must work the factors out.
    BasinsServer none(kBasinShape, fourLabels());
    BackendClient c1(none.url(), QStringLiteral("t"));
    const BackendVelocityBasinsResult r1 = c1.fetchVelocityBasins(QStringLiteral("d"));
    QVERIFY2(r1.valid, qPrintable(r1.error));
    QVERIFY(!r1.hasGeometry);
    // Box 0 is sent too: an omitted box would mean the dataset's own box.
    QVERIFY(!none.requestBody.isEmpty());
    const QJsonObject sent = QJsonDocument::fromJson(none.requestBody).object();
    QVERIFY(sent.contains(u"box_mpc"));
    QCOMPARE(sent.value(u"box_mpc").toDouble(-1.0), 0.0);

    BasinsServer partial(kBasinShape + "X-Visivo-Spacing: 2,2,1\r\n", fourLabels());
    BackendClient c2(partial.url(), QStringLiteral("t"));
    const BackendVelocityBasinsResult r2 = c2.fetchVelocityBasins(QStringLiteral("d"));
    QVERIFY(r2.valid);
    QVERIFY(!r2.hasGeometry);
}

void TestVelocityLodGeometry::densityFetchSendsTheViewerBox()
{
    // Four float32 values are as many bytes as four int32 labels.
    const QByteArray shape = "X-Visivo-Width: 2\r\nX-Visivo-Height: 2\r\nX-Visivo-Depth: 1\r\n";
    BasinsServer srv(shape + "X-Visivo-Spacing: 1,1,1\r\nX-Visivo-Origin: -0.5,-0.5,0\r\n",
                     fourLabels());
    BackendClient client(srv.url(), QStringLiteral("t"));
    const BackendVelocityDensityResult r =
            client.fetchVelocityDensity(QStringLiteral("d"), 256, 80.0);
    QVERIFY2(r.valid, qPrintable(r.error));
    QCOMPARE(QJsonDocument::fromJson(srv.requestBody).object().value(u"box_mpc").toDouble(), 80.0);

    // Box 0 is sent as 0, not left out.
    BasinsServer zero(shape, fourLabels());
    BackendClient c0(zero.url(), QStringLiteral("t"));
    QVERIFY(c0.fetchVelocityDensity(QStringLiteral("d"), 256, 0.0).valid);
    const QJsonObject sent = QJsonDocument::fromJson(zero.requestBody).object();
    QVERIFY(sent.contains(u"box_mpc"));
    QCOMPARE(sent.value(u"box_mpc").toDouble(-1.0), 0.0);
}

void TestVelocityLodGeometry::staleDensityReplyIsDroppedAfterTwoBoxChanges()
{
    using velocitylod::DensityReply;
    using velocitylod::densityReplyAction;
    // The viewer's rule: each box change bumps the generation, and so does
    // each ∇·v request. Box 100 asks (A); while A waits, box 200 asks (B).
    int latest = 0;
    ++latest;                       // box change to 100
    const int a = ++latest;         // request A
    ++latest;                       // box change to 200
    const int b = ++latest;         // request B

    // B's reply comes first (its nested event loop sits inside A's) and is
    // installed; A's delayed reply, good or not, is dropped: no install at
    // the new box's placement, and no local fallback over B's grid.
    QCOMPARE(densityReplyAction(b, latest, true), DensityReply::Install);
    QCOMPARE(densityReplyAction(a, latest, true), DensityReply::Drop);
    QCOMPARE(densityReplyAction(a, latest, false), DensityReply::Drop);

    // The latest request failing falls back to the local computation.
    QCOMPARE(densityReplyAction(b, latest, false), DensityReply::ComputeLocally);

    // A box change with the overlays off asks nothing, yet still supersedes
    // the request in flight: its reply is for the old box.
    const int c = ++latest;         // request C
    ++latest;                       // box change, density hidden
    QCOMPARE(densityReplyAction(c, latest, true), DensityReply::Drop);
}
