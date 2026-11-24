// test_contour_settings.cpp
// See TestContourSettings.h.

#include "TestContourSettings.h"

#include "ContourSettings.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace vc = visivo::contours;

void TestContourSettings::countLevelsHaveBothEnds()
{
    vc::Settings s;
    s.count = 5;
    s.lower = 1.0;
    s.upper = 9.0;
    const auto lv = vc::levels(s);
    QCOMPARE(lv.size(), std::size_t(5));
    QCOMPARE(lv.front(), 1.0);
    QCOMPARE(lv.back(), 9.0);
    QVERIFY(std::abs(lv[2] - 5.0) < 1e-12);

    // The range of the data upside down is a mistake to report, not to guess at.
    s.lower = 9.0;
    s.upper = 1.0;
    QString why;
    QVERIFY(vc::levels(s, &why).empty());
    QVERIFY(!why.isEmpty());
}

void TestContourSettings::aSingleLevelIsTheLowerBound()
{
    vc::Settings s;
    s.count = 1;
    s.lower = 2.5;
    s.upper = 7.0;
    const auto lv = vc::levels(s);
    QCOMPARE(lv.size(), std::size_t(1));
    QCOMPARE(lv.front(), 2.5);
}

void TestContourSettings::sigmaLevelsAreMultiplesOfTheNoise()
{
    vc::Settings s;
    s.levelMode = vc::LevelMode::Sigma;
    s.sigma = 0.002;
    s.multiples = QStringLiteral("3, 5, 10, 20");
    const auto lv = vc::levels(s);
    QCOMPARE(lv.size(), std::size_t(4));
    QVERIFY(std::abs(lv[0] - 0.006) < 1e-12);
    QVERIFY(std::abs(lv[1] - 0.010) < 1e-12);
    QVERIFY(std::abs(lv[2] - 0.020) < 1e-12);
    QVERIFY(std::abs(lv[3] - 0.040) < 1e-12);
}

void TestContourSettings::negativeSigmaLevelsAreMirrored()
{
    vc::Settings s;
    s.levelMode = vc::LevelMode::Sigma;
    s.sigma = 1.0;
    s.multiples = QStringLiteral("3 5");
    s.negative = true;
    const auto lv = vc::levels(s);
    QCOMPARE(lv.size(), std::size_t(4));
    QCOMPARE(lv[0], -5.0);
    QCOMPARE(lv[1], -3.0);
    QCOMPARE(lv[2], 3.0);
    QCOMPARE(lv[3], 5.0);
}

void TestContourSettings::sigmaNeedsTheNoise()
{
    vc::Settings s;
    s.levelMode = vc::LevelMode::Sigma;
    s.sigma = 0.0;
    QString why;
    QVERIFY(vc::levels(s, &why).empty());
    QVERIFY(why.contains(QStringLiteral("noise")));

    s.sigma = 1.0;
    s.multiples = QStringLiteral("abc");
    QVERIFY(vc::levels(s, &why).empty());
    QVERIFY(!why.isEmpty());
}

void TestContourSettings::logarithmicLevelsAreAGeometricProgression()
{
    vc::Settings s;
    s.levelMode = vc::LevelMode::Log;
    s.count = 4;
    s.lower = 1.0;
    s.upper = 1000.0;
    const auto lv = vc::levels(s);
    QCOMPARE(lv.size(), std::size_t(4));
    QVERIFY(std::abs(lv[0] - 1.0) < 1e-9);
    QVERIFY(std::abs(lv[1] - 10.0) < 1e-9);
    QVERIFY(std::abs(lv[2] - 100.0) < 1e-9);
    QVERIFY(std::abs(lv[3] - 1000.0) < 1e-9);
}

void TestContourSettings::logarithmicLevelsNeedAPositiveLowerBound()
{
    vc::Settings s;
    s.levelMode = vc::LevelMode::Log;
    s.count = 5;
    s.lower = 0.0;
    s.upper = 10.0;
    QString why;
    QVERIFY(vc::levels(s, &why).empty());
    QVERIFY(!why.isEmpty());
    s.lower = -1.0;
    QVERIFY(vc::levels(s).empty());
}

void TestContourSettings::aListIsSortedAndWithoutRepeats()
{
    vc::Settings s;
    s.levelMode = vc::LevelMode::List;
    s.list = QStringLiteral("5, 0.1; 2  0.1, -1");
    const auto lv = vc::levels(s);
    QCOMPARE(lv.size(), std::size_t(4));
    QCOMPARE(lv[0], -1.0);
    QCOMPARE(lv[1], 0.1);
    QCOMPARE(lv[2], 2.0);
    QCOMPARE(lv[3], 5.0);
}

void TestContourSettings::aBadListIsAProblem()
{
    vc::Settings s;
    s.levelMode = vc::LevelMode::List;
    s.list = QString();
    QString why;
    QVERIFY(vc::levels(s, &why).empty());
    QVERIFY(!why.isEmpty());

    bool ok = true;
    const auto typed = vc::parseNumberList(QStringLiteral("1, two, 3, nan"), &ok);
    QVERIFY(!ok);
    QCOMPARE(typed.size(), 2);
}

void TestContourSettings::noiseIsTheMadAndIgnoresBrightSources()
{
    // Gaussian-looking noise from a deterministic generator, sigma = 2, with a
    // few percent of very bright pixels on top: the MAD stays at the noise.
    std::vector<double> v;
    unsigned state = 12345u;
    const auto uniform = [&state]() {
        state = state * 1664525u + 1013904223u;
        return (state >> 8) / double(1u << 24);
    };
    for (int i = 0; i < 40000; ++i) {
        double g = 0.0;
        for (int k = 0; k < 12; ++k)
            g += uniform();
        v.push_back((g - 6.0) * 2.0);   // sum of 12 uniforms: variance 1
    }
    const double clean = vc::robustSigma(v);
    QVERIFY(std::abs(clean - 2.0) < 0.1);
    for (std::size_t i = 0; i < v.size(); i += 40)
        v[i] = 1000.0;   // 2.5 % sources
    const double withSources = vc::robustSigma(v);
    QVERIFY(std::abs(withSources - 2.0) < 0.15);

    // NaN pixels are not data; too few pixels give no noise.
    v.push_back(std::numeric_limits<double>::quiet_NaN());
    QVERIFY(vc::robustSigma(v) > 0.0);
    QCOMPARE(vc::robustSigma(std::vector<double>{ 1.0 }), 0.0);
    const std::vector<float> flat(100, 3.0f);
    QCOMPARE(vc::robustSigma(flat.data(), flat.size()), 0.0);
}

void TestContourSettings::nearestLevelPicksTheClosest()
{
    const std::vector<double> lv{ -2.0, 1.0, 4.0 };
    QCOMPARE(vc::nearestLevel(lv, -3.0), 0);
    QCOMPARE(vc::nearestLevel(lv, -2.0000001), 0);
    QCOMPARE(vc::nearestLevel(lv, 1.2), 1);
    QCOMPARE(vc::nearestLevel(lv, 3.0), 2);
    QCOMPARE(vc::nearestLevel(lv, 99.0), 2);
    QCOMPARE(vc::nearestLevel({}, 1.0), -1);
}

void TestContourSettings::smoothingBlursAndKeepsTheFlux()
{
    constexpr int n = 41;
    std::vector<float> in(n * n, 0.0f), out(n * n, -1.0f);
    in[20 * n + 20] = 100.0f;
    vc::smoothImage(in.data(), n, n, 2.0, 2.0, out.data());
    // The peak is lower, the neighbours are lifted, the total is the same.
    QVERIFY(out[20 * n + 20] < 20.0f);
    QVERIFY(out[20 * n + 21] > 1.0f);
    QVERIFY(out[20 * n + 20] > out[20 * n + 23]);
    double sum = 0.0;
    for (float v : out)
        sum += v;
    QVERIFY(std::abs(sum - 100.0) < 0.5);
    // A Gaussian of sigma 2: the peak is 100 / (2 pi sigma^2) = 3.98.
    QVERIFY(std::abs(out[20 * n + 20] - 3.98f) < 0.15f);
}

void TestContourSettings::smoothingOfAConstantIsTheConstant()
{
    // Also at the borders, where the kernel is cut: the weights are normalised.
    constexpr int w = 17, h = 11;
    std::vector<float> in(w * h, 7.0f), out(w * h, 0.0f);
    vc::smoothImage(in.data(), w, h, 3.0, 1.5, out.data());
    for (float v : out)
        QVERIFY(std::abs(v - 7.0f) < 1e-4f);
}

void TestContourSettings::smoothingKeepsBlanksAndIgnoresThemInNeighbours()
{
    constexpr int w = 9, h = 9;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    std::vector<float> in(w * h, 5.0f), out(w * h, 0.0f);
    in[4 * w + 4] = nan;
    vc::smoothImage(in.data(), w, h, 1.5, 1.5, out.data());
    QVERIFY(std::isnan(out[4 * w + 4]));            // a blank stays blank
    QVERIFY(std::abs(out[4 * w + 5] - 5.0f) < 1e-4f);   // and does not darken its neighbour
}

void TestContourSettings::noSmoothingIsACopy()
{
    std::vector<float> in{ 1.0f, 2.0f, 3.0f, 4.0f }, out(4, 0.0f);
    vc::smoothImage(in.data(), 2, 2, 0.0, 0.0, out.data());
    QCOMPARE(out, in);
}

void TestContourSettings::closedLinesArePolygonsAndOpenOnesSegments()
{
    vc::Line closed;
    closed.level = 1.0;
    closed.closed = true;
    closed.colour = QStringLiteral("#ff0000");
    closed.pixels = { { 0, 0 }, { 4, 0 }, { 4, 4 }, { 0, 4 }, { 0, 0 } };   // first repeated
    vc::Line open;
    open.level = 2.0;
    open.closed = false;
    open.colour = QStringLiteral("#00ff00");
    open.pixels = { { 0, 0 }, { 1, 1 }, { 2, 1 } };
    vc::Line tiny;   // too short to be a polygon: not written
    tiny.closed = true;
    tiny.pixels = { { 0, 0 }, { 1, 1 } };

    const QJsonArray regions = vc::canonicalRegions({ closed, open, tiny }, 2.0);
    QCOMPARE(regions.size(), 2);
    const QJsonObject poly = regions.at(0).toObject();
    QCOMPARE(poly.value(QStringLiteral("shape")).toString(), QStringLiteral("polygon"));
    QCOMPARE(poly.value(QStringLiteral("frame")).toString(), QStringLiteral("image"));
    // The repeated closing vertex is dropped, and the pixels are 1-based.
    const QJsonArray pts = poly.value(QStringLiteral("points")).toArray();
    QCOMPARE(pts.size(), 4);
    QCOMPARE(pts.at(0).toArray().at(0).toDouble(), 1.0);
    QCOMPARE(pts.at(2).toArray().at(1).toDouble(), 5.0);
    QCOMPARE(regions.at(1).toObject().value(QStringLiteral("shape")).toString(),
             QStringLiteral("line"));
}

void TestContourSettings::regionsCarryColourWidthDashAndLevel()
{
    vc::Line l;
    l.level = -0.0125;
    l.closed = true;
    l.dashed = true;
    l.colour = QStringLiteral("#3366ff");
    l.pixels = { { 0, 0 }, { 3, 0 }, { 3, 3 } };
    const QJsonObject r = vc::canonicalRegions({ l }, 1.5).at(0).toObject();
    const QJsonObject props = r.value(QStringLiteral("props")).toObject();
    QCOMPARE(props.value(QStringLiteral("color")).toString(), QStringLiteral("#3366ff"));
    QCOMPARE(props.value(QStringLiteral("width")).toDouble(), 1.5);
    QVERIFY(props.value(QStringLiteral("dash")).toBool());
    QCOMPARE(props.value(QStringLiteral("tag")).toArray().at(0).toString(),
             QStringLiteral("level=-0.0125"));

    l.dashed = false;
    QVERIFY(!vc::canonicalRegions({ l }, 1.0).at(0).toObject().value(QStringLiteral("props"))
                     .toObject().contains(QStringLiteral("dash")));
}

void TestContourSettings::verticesTableFollowsTheRegions()
{
    vc::Line a;
    a.level = 0.5;
    a.closed = true;
    a.colour = QStringLiteral("#ffffff");
    a.pixels = { { 0, 0 }, { 2, 0 }, { 2, 2 } };
    vc::Line b;
    b.level = 1.5;
    b.closed = false;
    b.colour = QStringLiteral("#ffffff");
    b.pixels = { { 5, 5 }, { 6, 6 } };
    const QString csv = vc::verticesCsv(vc::canonicalRegions({ a, b }, 1.0),
                                        QStringLiteral("x"), QStringLiteral("y"));
    const QStringList rows = csv.trimmed().split(QLatin1Char('\n'));
    QCOMPARE(rows.size(), 1 + 3 + 2);
    QCOMPARE(rows.at(0), QStringLiteral("level,line,closed,vertex,x,y"));
    QCOMPARE(rows.at(1), QStringLiteral("0.5,1,1,1,1.00000000,1.00000000"));
    QCOMPARE(rows.at(4), QStringLiteral("1.5,2,0,1,6.00000000,6.00000000"));
}

namespace {

const double kNaN = std::numeric_limits<double>::quiet_NaN();

/// Every number in the regions' points, to see that none is null.
bool allPointsAreNumbers(const QJsonArray &regions)
{
    for (const QJsonValue &r : regions)
        for (const QJsonValue &p : r.toObject().value(QStringLiteral("points")).toArray()) {
            const QJsonArray xy = p.toArray();
            if (xy.size() != 2 || !xy.at(0).isDouble() || !xy.at(1).isDouble())
                return false;
        }
    return true;
}

}   // namespace

void TestContourSettings::verticesThatAreNotNumbersCutTheLines()
{
    // A contour along a blanked edge: Qt writes the NaN as JSON null, which the
    // backend refuses ("Unprocessable Entity" / "Internal Server Error").
    vc::Line polygon;
    polygon.closed = true;
    polygon.colour = QStringLiteral("#ff0000");
    polygon.pixels = { { 0, 0 }, { 4, 0 }, { kNaN, 4 }, { 0, 4 }, { 0, 1 } };
    vc::Line open;
    open.colour = QStringLiteral("#00ff00");
    open.pixels = { { 0, 0 }, { 1, 1 }, { 2, kNaN }, { 3, 3 }, { 4, 4 }, { 5, 5 } };
    const QJsonArray regions = vc::canonicalRegions({ polygon, open }, 1.0);
    QVERIFY(allPointsAreNumbers(regions));
    // The polygon is cut at the bad vertex into (0,0)(4,0) and (0,4)(0,1), the
    // open line into (0,0)(1,1) and (3,3)(4,4)(5,5): four open lines. A line that
    // was cut is not joined again.
    QCOMPARE(regions.size(), 4);
    for (const QJsonValue &r : regions)
        QCOMPARE(r.toObject().value(QStringLiteral("shape")).toString(), QStringLiteral("line"));
    // The same through the JSON text the request is made of: no null.
    QVERIFY(!QJsonDocument(regions).toJson().contains("null"));
}

void TestContourSettings::linesWithTooFewFinitePointsAreSkipped()
{
    vc::Line polyline;
    polyline.pixels = { { 0, 0 }, { kNaN, 1 }, { 2, 2 }, { kNaN, kNaN }, { 4, 4 } };
    vc::Line polygon;
    polygon.closed = true;
    polygon.pixels = { { 0, 0 }, { 1, 1 }, { kNaN, 3 } };
    vc::Line allBad;
    allBad.pixels = { { kNaN, kNaN }, { kNaN, 1 } };
    const QJsonArray regions = vc::canonicalRegions({ polyline, polygon, allBad }, 1.0);
    // Only the polygon's two finite vertices make a line; nothing else has two in a row.
    QCOMPARE(regions.size(), 1);
    QVERIFY(allPointsAreNumbers(regions));
}

void TestContourSettings::sigmaIsRecordedInTheRegions()
{
    vc::Line line;
    line.level = -0.0003;
    line.pixels = { { 0, 0 }, { 1, 1 } };
    const QJsonArray regions = vc::canonicalRegions({ line }, 1.0, 0.0001);
    const QJsonArray tags = regions.at(0).toObject().value(QStringLiteral("props")).toObject()
                                    .value(QStringLiteral("tag")).toArray();
    QVERIFY(tags.contains(QStringLiteral("sigma=0.0001")));
    QVERIFY(tags.contains(QStringLiteral("n_sigma=-3")));
    // Not in sigma: only the level.
    const QJsonArray plain = vc::canonicalRegions({ line }, 1.0);
    QCOMPARE(plain.at(0).toObject().value(QStringLiteral("props")).toObject()
                     .value(QStringLiteral("tag")).toArray().size(), 1);
}

void TestContourSettings::segmentsAreJoinedIntoLines()
{
    // Points 0-1-2-3 an open line; 4-5-6-7 a square that closes. The segments
    // are given in no order and some the wrong way round.
    const std::vector<std::int64_t> segs = { 6, 5, 2, 3, 4, 5, 0, 1, 7, 4, 1, 2, 6, 7 };
    const vc::Chains c = vc::chainSegments(segs.data(), segs.size() / 2, 8);
    QCOMPARE(c.count(), std::size_t(2));
    QCOMPARE(c.cancelled, false);
    std::size_t open = 0, loops = 0;
    for (std::size_t i = 0; i < c.count(); ++i) {
        const std::size_t n = c.start[i + 1] - c.start[i];
        if (c.closed[i]) {
            ++loops;
            QCOMPARE(n, std::size_t(5));   // four corners, the first again
            QCOMPARE(c.ids[c.start[i]], c.ids[c.start[i + 1] - 1]);
        } else {
            ++open;
            QCOMPARE(n, std::size_t(4));
            const auto a = c.ids[c.start[i]], b = c.ids[c.start[i + 1] - 1];
            QVERIFY((a == 0 && b == 3) || (a == 3 && b == 0));
        }
    }
    QCOMPARE(open, std::size_t(1));
    QCOMPARE(loops, std::size_t(1));
}

void TestContourSettings::segmentsWithABadEndSplitTheLine()
{
    const std::vector<std::int64_t> segs = { 0, 1, 1, 2, 2, 3, 3, 4 };
    const std::vector<unsigned char> ok = { 1, 1, 0, 1, 1 };   // point 2 is not a number
    const vc::Chains c = vc::chainSegments(segs.data(), 4, 5, ok.data());
    // 0-1 and 3-4 are left; 1-2 and 2-3 are dropped with point 2.
    QCOMPARE(c.count(), std::size_t(2));
    QCOMPARE(c.ids.size(), std::size_t(4));
    // Cancelled before it starts: nothing, and it says so.
    std::atomic<bool> stop{ true };
    const vc::Chains none = vc::chainSegments(segs.data(), 4, 5, nullptr, &stop);
    QVERIFY(none.cancelled);
    QCOMPARE(none.count(), std::size_t(0));
}

void TestContourSettings::joinedLinesCarryLevelColourAndDash()
{
    vc::LineSource src;
    src.points = { { 0, 0 }, { 1, 0 }, { 2, 0 }, { 0, 5 }, { 1, 5 }, { kNaN, 5 }, { 3, 5 } };
    src.level = { -1, -1, -1, 2, 2, 2, 2 };
    src.segments = { 0, 1, 1, 2, 3, 4, 4, 5, 5, 6 };   // the last two touch the NaN point
    src.levels = { -1, 2 };
    src.colours = { QStringLiteral("#0000ff"), QStringLiteral("#ff0000") };
    src.dashNegative = true;
    const std::vector<vc::Line> lines = vc::joinLines(src);
    QCOMPARE(lines.size(), std::size_t(2));
    const vc::Line &neg = lines.at(0).level < 0 ? lines.at(0) : lines.at(1);
    const vc::Line &pos = lines.at(0).level < 0 ? lines.at(1) : lines.at(0);
    QCOMPARE(neg.pixels.size(), std::size_t(3));
    QVERIFY(neg.dashed);
    QCOMPARE(neg.colour, QStringLiteral("#0000ff"));
    QVERIFY(!pos.dashed);
    QCOMPARE(pos.pixels.size(), std::size_t(2));   // cut at the NaN: 3-4 only
    src.dashNegative = false;
    for (const vc::Line &l : vc::joinLines(src))
        QVERIFY(!l.dashed);
}

void TestContourSettings::dashesFollowTheLengthAndRoundCorners()
{
    // 10 long, 2 drawn and 1 blank: dashes at 0-2, 3-5, 6-8 and 9-10.
    std::vector<std::vector<std::array<double, 2>>> pieces;
    vc::dashPolyline({ { 0, 0 }, { 10, 0 } }, 2.0, 1.0,
                     [&](const std::vector<std::array<double, 2>> &p) { pieces.push_back(p); });
    QCOMPARE(pieces.size(), std::size_t(4));
    QCOMPARE(pieces[1].front()[0], 3.0);
    QCOMPARE(pieces[1].back()[0], 5.0);
    QCOMPARE(pieces[3].front()[0], 9.0);
    QCOMPARE(pieces[3].back()[0], 10.0);
    // A corner inside a dash is kept in it: 3 along x, then 3 up, 4 drawn / 2 blank.
    pieces.clear();
    vc::dashPolyline({ { 0, 0 }, { 3, 0 }, { 3, 3 } }, 4.0, 2.0,
                     [&](const std::vector<std::array<double, 2>> &p) { pieces.push_back(p); });
    QCOMPARE(pieces.size(), std::size_t(1));
    QCOMPARE(pieces[0].size(), std::size_t(3));
    QCOMPARE(pieces[0].back()[1], 1.0);
    // No pattern: the line comes out whole.
    pieces.clear();
    vc::dashPolyline({ { 0, 0 }, { 5, 0 } }, 0.0, 1.0,
                     [&](const std::vector<std::array<double, 2>> &p) { pieces.push_back(p); });
    QCOMPARE(pieces.size(), std::size_t(1));
}

void TestContourSettings::largeImagesAreThinned()
{
    QCOMPARE(vc::thinStride(1500, 1500, 4'000'000), 1);
    QCOMPARE(vc::thinStride(2000, 2000, 4'000'000), 1);
    const int s = vc::thinStride(10368, 10368, 4'000'000);
    QVERIFY(s >= 6);
    QVERIFY(((10368 + s - 1) / s) * ((10368 + s - 1) / s) <= 4'000'000);
    QVERIFY(vc::thinStride(5184, 5184, 4'000'000) >= 3);
    QCOMPARE(vc::thinStride(0, 10, 100), 1);
}

void TestContourSettings::thinningIsSaidInTheNote()
{
    // The SDP9 continuum as shown: 5184 x 5184, thinned 1 in 3.
    const QString note = vc::thinningNote(5184, 5184, 4'000'000);
    QVERIFY(note.contains(QStringLiteral("1 pixel in 3")));
    QVERIFY(note.contains(QStringLiteral("27 Mpx")));
    QVERIFY(vc::thinningNote(2000, 2000, 4'000'000).isEmpty());
    QVERIFY(vc::thinningNote(0, 0, 4'000'000).isEmpty());
}

void TestContourSettings::signedMultiplesGiveNegativeLevels()
{
    vc::Settings s;
    s.levelMode = vc::LevelMode::Sigma;
    s.sigma = 2.0;
    s.multiples = QStringLiteral("-3, 3, 5");
    const std::vector<double> lv = vc::levels(s);
    QCOMPARE(lv.size(), std::size_t(3));
    QCOMPARE(lv.at(0), -6.0);
    QCOMPARE(lv.at(2), 10.0);
}

namespace {

/// A noise image of sigma 1 around zero with a bright source (a radio map's look).
vc::DataStats noiseImageStats()
{
    std::vector<double> sample;
    unsigned state = 12345;
    for (int i = 0; i < 20000; ++i) {
        // A sum of uniforms: bell-shaped enough, and reproducible.
        double v = 0.0;
        for (int k = 0; k < 12; ++k) {
            state = state * 1664525u + 1013904223u;
            v += double(state >> 8) / double(1u << 24);
        }
        sample.push_back(v - 6.0);
    }
    sample.push_back(60.0);   // a source
    return vc::dataStats(sample);
}

}   // namespace

void TestContourSettings::defaultLevelsAreThreeFiveTenTwentySigma()
{
    const vc::DataStats st = noiseImageStats();
    QVERIFY(st.sigma > 0.8 && st.sigma < 1.2);
    QString note = QStringLiteral("stale");
    const vc::Settings s = vc::defaultSettings(st, /*radio=*/false, &note);
    QCOMPARE(s.levelMode, vc::LevelMode::Sigma);
    QVERIFY(note.isEmpty());
    QCOMPARE(s.colourMode, vc::ColourMode::Single);
    const std::vector<double> lv = vc::levels(s);
    QCOMPARE(lv.size(), std::size_t(4));
    QCOMPARE(lv.at(0), 3.0 * st.sigma);
    QCOMPARE(lv.at(1), 5.0 * st.sigma);
    QCOMPARE(lv.at(2), 10.0 * st.sigma);
    QCOMPARE(lv.at(3), 20.0 * st.sigma);
}

void TestContourSettings::radioImagesAlsoGetMinusThreeSigmaDashed()
{
    QVERIFY(vc::isRadioUnit(QStringLiteral("Jy/beam")));
    QVERIFY(vc::isRadioUnit(QStringLiteral("JY/BEAM")));
    QVERIFY(vc::isRadioUnit(QStringLiteral("mJy beam-1")));
    QVERIFY(!vc::isRadioUnit(QStringLiteral("count")));
    const vc::DataStats st = noiseImageStats();
    const vc::Settings s = vc::defaultSettings(st, /*radio=*/true);
    const std::vector<double> lv = vc::levels(s);
    QCOMPARE(lv.size(), std::size_t(5));
    QCOMPARE(lv.front(), -3.0 * st.sigma);
    QVERIFY(s.dashNegative);   // a level below zero is drawn dashed
}

void TestContourSettings::aMaskedMomentMapFallsBackToLogLevels()
{
    // Zero (and NaN) outside the mask, a few hundred positive values inside:
    // the MAD is zero, and sigma levels would be 0 x n.
    std::vector<double> sample(10000, 0.0);
    for (int i = 0; i < 2000; ++i)
        sample.push_back(kNaN);
    for (int i = 0; i < 300; ++i)
        sample.push_back(0.1 + 0.05 * i);   // 0.1 .. 15
    const vc::DataStats st = vc::dataStats(sample);
    QCOMPARE(st.sigma, 0.0);
    QString note;
    const vc::Settings s = vc::defaultSettings(st, /*radio=*/true, &note);
    QCOMPARE(s.levelMode, vc::LevelMode::Log);
    QVERIFY(note.contains(QStringLiteral("not meaningful")));
    QVERIFY(note.contains(QStringLiteral("logarithmic")));
    QVERIFY(s.lower > 0.0 && s.lower < s.upper);
    QVERIFY(s.upper < st.max);   // the 99.5th percentile, not the maximum
    QVERIFY(s.lower >= 0.1);
    QString problem;
    const std::vector<double> lv = vc::levels(s, &problem);
    QVERIFY(problem.isEmpty());
    QCOMPARE(lv.size(), std::size_t(8));
    QCOMPARE(lv.front(), s.lower);
    QCOMPARE(lv.back(), s.upper);
}

void TestContourSettings::aSimulationFieldFallsBackToLogLevels()
{
    // A density spread over seven decades, all positive (the range of a PLUTO
    // field): its MAD is a spread, not a noise, and 3 x it is of no meaning.
    std::vector<double> sample;
    for (int i = 0; i < 5000; ++i)
        sample.push_back(1.83e-12 * std::pow(2.11593e-5 / 1.83e-12, i / 4999.0));
    const vc::DataStats st = vc::dataStats(sample);
    QVERIFY(st.sigma > 0.0);
    QString note;
    const vc::Settings s = vc::defaultSettings(st, /*radio=*/false, &note);
    QCOMPARE(s.levelMode, vc::LevelMode::Log);
    QVERIFY(note.contains(QStringLiteral("not meaningful")));
    QVERIFY(s.lower > 1.83e-12 && s.upper < 2.11593e-5);
    // The levels are a geometric progression inside the data's range.
    const std::vector<double> lv = vc::levels(s);
    QCOMPARE(lv.size(), std::size_t(8));
    QVERIFY(std::abs(lv[1] / lv[0] - lv[7] / lv[6]) < 1e-9 * (lv[1] / lv[0]));
    // Only negative or zero values: no log levels, an even spread says so.
    const vc::Settings flat =
            vc::defaultSettings(vc::dataStats({ -3.0, -2.0, -1.0, 0.0 }), false, &note);
    QCOMPARE(flat.levelMode, vc::LevelMode::Count);
    QVERIFY(note.contains(QStringLiteral("evenly")));
}

void TestContourSettings::theLevelsInUseAreDescribed()
{
    vc::Settings s;
    s.levelMode = vc::LevelMode::Sigma;
    s.sigma = 0.00041;
    s.multiples = QStringLiteral("-3, 3, 5");
    const QString text = vc::describeLevels(s, vc::levels(s));
    QVERIFY(text.contains(QStringLiteral("σ = 0.00041")));
    QVERIFY(text.contains(QStringLiteral("3 level(s)")));
    QVERIFY(text.contains(QStringLiteral("-0.00123")));
    s.levelMode = vc::LevelMode::Count;
    s.count = 20;
    s.lower = 0;
    s.upper = 19;
    const QString many = vc::describeLevels(s, vc::levels(s));
    QVERIFY(many.contains(QStringLiteral("20 level(s)")));
    QVERIFY(many.contains(QStringLiteral("…")));
    QVERIFY(vc::describeLevels(s, {}).isEmpty());
}
