// test_point_table_display.cpp
// See TestPointTableDisplay.h.

#include "TestPointTableDisplay.h"

#include "app/PointTableDisplay.h"
#include "gui/CatalogueFormat.h"

#include <QMap>
#include <QtTest>

#include <functional>

using namespace Qt::StringLiterals;
using namespace visivo::pointtable;

void TestPointTableDisplay::anAxisNamesTheUnitTheTableStatesForIt()
{
    // A Gadget snapshot's lengths, as the backend's component.units says them:
    // the label is the stored unit, never the Mpc it used to say for all.
    QCOMPARE(axisTitle(u"X"_s, u"kpc/h comoving"_s, u"Mpc"_s), u"X (kpc/h comoving)"_s);
    QCOMPARE(axisTitle(u"Z"_s, u"box units"_s, u"Mpc"_s), u"Z (box units)"_s);
}

void TestPointTableDisplay::aNoteAfterTheUnitIsLeftOffTheTitle()
{
    // RAMSES: the numbers run 0..1 over the box.
    QCOMPARE(shortUnit(u"box units (fraction of the box)"_s), u"box units"_s);
    QCOMPARE(axisTitle(u"X"_s, u"box units (fraction of the box)"_s, u"Mpc"_s),
             u"X (box units)"_s);
    // A unit that is only parenthesised is kept whole.
    QCOMPARE(shortUnit(u"(km/s)"_s), u"(km/s)"_s);
}

void TestPointTableDisplay::aTableThatStatesNothingKeepsMpcOnItsComputedPositions()
{
    // A VBT's header carries no unit; a sky catalogue's positions are computed in Mpc.
    QCOMPARE(axisTitle(u"Y"_s, QString(), u"Mpc"_s), u"Y (Mpc)"_s);
    QCOMPARE(axisTitle(u"Y"_s, u"  "_s, u"Mpc"_s), u"Y (Mpc)"_s);
}

void TestPointTableDisplay::aMappedColumnWithoutAUnitHasNoneToName()
{
    QCOMPARE(axisTitle(u"X"_s, QString(), QString()), u"X"_s);
}

void TestPointTableDisplay::aRamsesOutputIsNamedRamsesNotVbt()
{
    const QString ref = u"/sim/output_00185::ramses-family=dm"_s;
    QCOMPARE(visivo::particleFormatLabel(u"ramses"_s, ref), u"RAMSES"_s);
    QCOMPARE(visivo::particleFormatLabel(u"gadget"_s, u"/sim/snap_010.0::gadget-type=1"_s),
             u"GADGET"_s);
}

void TestPointTableDisplay::aVbtAndAnOlderBackendStayVbt()
{
    QCOMPARE(visivo::particleFormatLabel(u"vbt"_s, u"/sim/points.head"_s), u"VBT"_s);
    QCOMPARE(visivo::particleFormatLabel(QString(), u"/sim/anything.csv"_s), u"VBT"_s);
    // The file says VBT whatever the format reads.
    QCOMPARE(visivo::particleFormatLabel(u"csv"_s, u"/sim/points.bin"_s), u"VBT"_s);
}

namespace {
// The spreads of a table's columns by name; a column not listed has no value yet.
struct Spreads
{
    QMap<QString, Spread> by;
    QStringList names;
    Spread operator()(int i) const { return by.value(names.at(i), Spread::Unknown); }
};
}   // namespace

void TestPointTableDisplay::thePreferredColumnIsKeptWhenItVaries()
{
    const QStringList names{ u"id"_s, u"mass"_s, u"rho"_s };
    const Spreads s{ { { u"rho"_s, Spread::Varies }, { u"mass"_s, Spread::Constant } }, names };
    const ColourChoice c = chooseDefaultColour(names, { u"rho"_s, u"mass"_s }, s);
    QCOMPARE(c.index, 2);
    QVERIFY(!c.pending);
}

void TestPointTableDisplay::theFirstColumnIsKeptWhenNoneIsPreferred()
{
    // As before: a table's first column when nothing is preferred, even though
    // the rows loaded have no value for it yet.
    const QStringList names{ u"alpha"_s, u"beta"_s };
    const Spreads s{ {}, names };
    const ColourChoice c = chooseDefaultColour(names, { u"rho"_s }, s);
    QCOMPARE(c.index, 0);
    QVERIFY(!c.pending);
}

void TestPointTableDisplay::aConstantMassGivesWayToAColumnThatVaries()
{
    const QStringList names{ u"mass"_s, u"metallicity"_s, u"birth_time"_s };
    const Spreads s{ { { u"mass"_s, Spread::Constant }, { u"metallicity"_s, Spread::Constant },
                       { u"birth_time"_s, Spread::Varies } },
                     names };
    const ColourChoice c = chooseDefaultColour(names, { u"rho"_s, u"mass"_s }, s);
    QCOMPARE(c.index, 2);
}

void TestPointTableDisplay::noColumnIsChosenWhenEveryoneIsConstant()
{
    const QStringList names{ u"mass"_s, u"level"_s };
    const Spreads s{ { { u"mass"_s, Spread::Constant }, { u"level"_s, Spread::Constant } },
                     names };
    const ColourChoice c = chooseDefaultColour(names, { u"mass"_s }, s);
    QCOMPARE(c.index, -1);
    QVERIFY(!c.pending);
}

void TestPointTableDisplay::theChoiceWaitsForColumnsNotLoadedYet()
{
    // The first paint holds the positions and the one preferred column.
    const QStringList names{ u"mass"_s, u"metallicity"_s };
    const Spreads s{ { { u"mass"_s, Spread::Constant } }, names };
    const ColourChoice c = chooseDefaultColour(names, { u"mass"_s }, s);
    QCOMPARE(c.index, -1);
    QVERIFY(c.pending);
}

void TestPointTableDisplay::anIdentifierIsNeverTheDefault()
{
    const QStringList names{ u"mass"_s, u"id"_s, u"family"_s };
    const Spreads s{ { { u"mass"_s, Spread::Constant }, { u"id"_s, Spread::Varies },
                       { u"family"_s, Spread::Varies } },
                     names };
    const ColourChoice c = chooseDefaultColour(names, { u"mass"_s }, s);
    QCOMPARE(c.index, -1);
    QVERIFY(!c.pending);
}

void TestPointTableDisplay::aConstantColumnHasOneValueNotAMadeUpRange()
{
    // The mass of every dark-matter particle of output_00185.
    const ColourSpan span = colourSpan(4.07e-7, 4.07e-7);
    QVERIFY(span.constant.has_value());
    QCOMPARE(*span.constant, 4.07e-7);
    // A scale still exists around it, positive so a Log scale works, with the
    // value in the middle; not -0.5..+0.5.
    QVERIFY(span.lo < span.hi);
    QVERIFY(span.lo > 0.0);
    QVERIFY(qFuzzyCompare(0.5 * (span.lo + span.hi), 4.07e-7));
    QVERIFY(span.hi < 1e-6);
    // Zero has no relative range to make.
    const ColourSpan zero = colourSpan(0.0, 0.0);
    QVERIFY(zero.constant.has_value());
    QVERIFY(zero.lo < zero.hi);
}

void TestPointTableDisplay::aColumnThatVariesKeepsItsRange()
{
    const ColourSpan span = colourSpan(1.0, 3.0);
    QVERIFY(!span.constant.has_value());
    QCOMPARE(span.lo, 1.0);
    QCOMPARE(span.hi, 3.0);
}

void TestPointTableDisplay::aConstantBarIsOneValueAndItsOwnColour()
{
    QCOMPARE(constantBarTitle(QStringLiteral("mass")), QStringLiteral("mass (constant)"));
    // The value of a constant column sits in the middle of the span made
    // around it: the middle colour, linear; on a log scale it is further up.
    const ColourSpan span = colourSpan(4.07e-7, 4.07e-7);
    QVERIFY(qAbs(fractionAlong(4.07e-7, span.lo, span.hi, false) - 0.5) < 1e-9);
    const double onLog = fractionAlong(4.07e-7, span.lo, span.hi, true);
    QVERIFY(onLog > 0.5 && onLog < 1.0);
    // A value outside the range takes the end colour; a flat range, the middle.
    QCOMPARE(fractionAlong(9.0, 1.0, 2.0, false), 1.0);
    QCOMPARE(fractionAlong(0.0, 1.0, 2.0, false), 0.0);
    QCOMPARE(fractionAlong(1.0, 2.0, 2.0, false), 0.5);
    // What the backend reports for a constant column (lo == hi) goes through
    // the same rule as the rows loaded here.
    const ColourSpan reported = colourSpan(4.07169e-7, 4.07169e-7);
    QVERIFY(reported.constant.has_value());
    QVERIFY(reported.hi < 1e-6);
}
