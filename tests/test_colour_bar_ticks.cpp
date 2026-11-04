#include "TestColourBarTicks.h"

#include "gui/ColourBarTicks.h"

#include <QtTest>

using namespace Qt::Literals::StringLiterals;

void TestColourBarTicks::smallValuesKeepThreeDigits()
{
    // A radio cube's Jy/beam: as before.
    QCOMPARE(visivo::tickLabels({ -0.0108, 0.0092, 0.0292, 0.0492, 0.0692 }),
             QStringList({ u"-0.0108"_s, u"0.0092"_s, u"0.0292"_s, u"0.0492"_s, u"0.0692"_s }));
}

void TestColourBarTicks::aVelocityIsWrittenOut()
{
    // km/s, not "1.23e+03": whole numbers when the ticks are tens apart, a
    // decimal when they are a few apart.
    QCOMPARE(visivo::tickLabels({ 976.0, 1039.5, 1103.0, 1166.5, 1230.0 }),
             QStringList({ u"976"_s, u"1040"_s, u"1103"_s, u"1167"_s, u"1230"_s }));
    QCOMPARE(visivo::tickLabels({ 1000.0, 1002.5, 1005.0 }),
             QStringList({ u"1000.0"_s, u"1002.5"_s, u"1005.0"_s }));
}

void TestColourBarTicks::farApartValuesTakeExponents()
{
    QCOMPARE(visivo::tickLabels({ 5.31e-6, 0.000183 }),
             QStringList({ u"5.31e-06"_s, u"1.83e-04"_s }));
    QCOMPARE(visivo::tickLabels({ 1e4, 1e7 }), QStringList({ u"1.00e+04"_s, u"1.00e+07"_s }));
    // A log bar over many decades reaches a thousand, but written out its
    // small ticks would read 0; ticks a hair apart would read alike.
    QCOMPARE(visivo::tickLabels({ 1e-8, 1e-5, 0.01, 10.0, 10000.0 }),
             QStringList({ u"1.00e-08"_s, u"1.00e-05"_s, u"1.00e-02"_s, u"1.00e+01"_s,
                           u"1.00e+04"_s }));
    QCOMPARE(visivo::tickLabels({ 1000.0, 1000.00001, 1000.00002 }).first(), u"1.00e+03"_s);
}
