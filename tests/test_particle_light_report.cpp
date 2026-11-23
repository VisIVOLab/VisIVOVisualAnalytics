#include "TestParticleLightReport.h"

#include "gui/ParticleLightReport.h"

#include <QJsonObject>
#include <QtTest>

using namespace Qt::StringLiterals;
using namespace visivo;

void TestParticleLightReport::headersAndTaskResultsAreRead()
{
    ParticleLightReport r = lightReportFromHeaders("12", "539519", 3931546);
    QCOMPARE(r.dropped, 12);
    QCOMPARE(r.clipped, 539519);
    QCOMPARE(r.particles, 3931546);
    QVERIFY(!std::isfinite(r.spanDecades));
    QVERIFY(r.any());
    // An older backend sends neither header.
    r = lightReportFromHeaders(QByteArray(), QByteArray(), 10);
    QCOMPARE(r.dropped, -1);
    QCOMPARE(r.clipped, -1);
    QVERIFY(!r.any());
    QCOMPARE(lightReportFromHeaders("x", "-3", 0).dropped, -1);
    QCOMPARE(lightReportFromHeaders("x", "-3", 0).clipped, -1);

    // The task result (backend/app/routers/particle_render.py).
    r = lightReportFromResult(QJsonObject{ { u"dropped_rows"_s, 0 },
                                           { u"clipped_rows"_s, 539519 },
                                           { u"light_span_decades"_s, 38.4 },
                                           { u"particles"_s, 3931546 } });
    QCOMPARE(r.dropped, 0);
    QCOMPARE(r.clipped, 539519);
    QCOMPARE(r.spanDecades, 38.4);
    QCOMPARE(r.particles, 3931546);
    r = lightReportFromResult(QJsonObject{ { u"particles"_s, 5 } });
    QCOMPARE(r.dropped, -1);
    QVERIFY(!r.any());
}

void TestParticleLightReport::theLineAndTooltipSayWhatWasLeftOut()
{
    const QLocale c(QLocale::English, QLocale::UnitedStates);
    ParticleLightReport r;
    QVERIFY(lightReportText(r, c).isEmpty());
    r.dropped = 0;
    r.clipped = 0;
    QVERIFY(lightReportText(r, c).isEmpty());
    r.dropped = 1204;
    QCOMPARE(lightReportText(r, c), u"1,204 rows dropped (no light)"_s);
    r.clipped = 539519;
    QCOMPARE(lightReportText(r, c), u"1,204 rows dropped (no light) · 539,519 rows clipped"_s);
    r.dropped = 0;
    QCOMPARE(lightReportText(r, c), u"539,519 rows clipped"_s);
    QVERIFY(lightReportTooltip(r).contains(u"Clipped"_s));
    QVERIFY(!lightReportTooltip(r).contains(u"Dropped"_s));
    r.spanDecades = 38.43;
    QVERIFY(lightReportTooltip(r).contains(u"38.4 decades"_s));
}

void TestParticleLightReport::aTypeColumnIsFound()
{
    QCOMPARE(particleTypeColumn({ u"x"_s, u"y"_s, u"z"_s, u"Type"_s }), u"Type"_s);
    QCOMPARE(particleTypeColumn({ u"x"_s, u"ptype"_s }), u"ptype"_s);
    QCOMPARE(particleTypeColumn({ u"PartType"_s }), u"PartType"_s);
    QCOMPARE(particleTypeColumn({ u"particle_type"_s }), u"particle_type"_s);
    QVERIFY(particleTypeColumn({ u"x"_s, u"y"_s, u"z"_s, u"LogMass"_s, u"LogD"_s }).isEmpty());
    QVERIFY(particleTypeColumn({ u"spectral_type"_s, u"typeface"_s }).isEmpty());
}

void TestParticleLightReport::gasOnlyIsSuggestedWhenTwoPopulationsShareTheLight()
{
    ParticleLightReport r;
    r.clipped = 539519;
    r.particles = 3931546;
    r.spanDecades = 38.4;
    QVERIFY(suggestTypeFilter(r, u"type"_s));
    QVERIFY(!suggestTypeFilter(r, QString()));   // nothing to filter on
    r.spanDecades = 12.0;                        // one population, capped for float32
    QVERIFY(!suggestTypeFilter(r, u"type"_s));
    // Without the span (a GPU view's particles): by the share clipped.
    r.spanDecades = std::numeric_limits<double>::quiet_NaN();
    QVERIFY(suggestTypeFilter(r, u"type"_s));
    r.clipped = 10;
    QVERIFY(!suggestTypeFilter(r, u"type"_s));
    // A backend frame does not count its particles.
    r.particles = 0;
    QVERIFY(!suggestTypeFilter(r, u"type"_s));
    r.clipped = 5000;
    QVERIFY(suggestTypeFilter(r, u"type"_s));
    r.clipped = 0;
    QVERIFY(!suggestTypeFilter(r, u"type"_s));
    QCOMPARE(kGasType, 0);
}

void TestParticleLightReport::periodIsSentOnlyWhenTheUserTurnsItOff()
{
    // A box is periodic as the backend takes it: stated, or a comoving run.
    const auto meta = [](double box, const QJsonObject &units) {
        return QJsonObject{ { u"box_size"_s, box }, { u"units"_s, units } };
    };
    QVERIFY(periodicBoxKnown(meta(100.0, { { u"comoving"_s, true } })));
    QVERIFY(!periodicBoxKnown(meta(100.0, { { u"comoving"_s, false } })));
    QVERIFY(!periodicBoxKnown(meta(100.0, { { u"comoving"_s, true }, { u"periodic"_s, false } })));
    QVERIFY(periodicBoxKnown(meta(100.0, { { u"comoving"_s, false }, { u"periodic"_s, true } })));
    QVERIFY(!periodicBoxKnown(meta(0.0, { { u"comoving"_s, true } })));
    QVERIFY(!periodicBoxKnown(QJsonObject()));

    QJsonObject request;
    insertPeriod(request, true, true);   // the table's own box: null, left out
    QVERIFY(!request.contains(u"period"_s));
    insertPeriod(request, false, false);   // no box known: nothing to turn off
    QVERIFY(!request.contains(u"period"_s));
    insertPeriod(request, true, false);
    QVERIFY(request.value(u"period"_s).isDouble());
    QCOMPARE(request.value(u"period"_s).toDouble(), 0.0);
}
