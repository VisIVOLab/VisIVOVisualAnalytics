// test_contour_settings.cpp
// See TestContourSettings.h.

#include "TestContourSettings.h"

#include "ContourSettings.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QTest>

#include <cmath>
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
