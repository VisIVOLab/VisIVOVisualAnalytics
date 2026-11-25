// test_simulation_volume.cpp — see TestSimulationVolume.h.

#include "TestSimulationVolume.h"
#include "app/SimulationVolume.h"
#include "app/PhysicalUnits.h"

#include <QJsonDocument>
#include <QTest>

using namespace visivo::simvol;
using namespace visivo::units;

void TestSimulationVolume::planeCount_followsTheAxis()
{
    QCOMPARE(planeCount(SliceAxis::X, 20, 16, 12), 20);
    QCOMPARE(planeCount(SliceAxis::Y, 20, 16, 12), 16);
    QCOMPARE(planeCount(SliceAxis::Z, 20, 16, 12), 12);
    QCOMPARE(planeCount(SliceAxis::Z, 20, 16, 0), 1);
}

void TestSimulationVolume::planeAxes_areTheTwoAxesLeft()
{
    // The order the backend serves the plane in: z planes are x by y, y planes
    // x by z, x planes y by z.
    QCOMPARE(planeAxes(SliceAxis::Z), (std::array<int, 2>{ 0, 1 }));
    QCOMPARE(planeAxes(SliceAxis::Y), (std::array<int, 2>{ 0, 2 }));
    QCOMPARE(planeAxes(SliceAxis::X), (std::array<int, 2>{ 1, 2 }));
    QCOMPARE(axisFromIndex(1), SliceAxis::Y);
    QCOMPARE(axisLetter(SliceAxis::X), QStringLiteral("x"));
}

void TestSimulationVolume::nearestPlane_picksTheClosestAndFlagsTheOffGrid()
{
    // x cells centred at -0.95 .. 0.95, step 0.1 (the first at pixel 1).
    bool inside = false;
    QCOMPARE(nearestPlane(0.04, -0.95, 1.0, 0.1, 20, &inside), 10);
    QVERIFY(inside);
    QCOMPARE(nearestPlane(-0.95, -0.95, 1.0, 0.1, 20, &inside), 0);
    QCOMPARE(nearestPlane(0.95, -0.95, 1.0, 0.1, 20, &inside), 19);
    QVERIFY(inside);
    // Half a plane beyond the last is still it; a whole plane is off the grid and clamps.
    QCOMPARE(nearestPlane(1.0, -0.95, 1.0, 0.1, 20, &inside), 19);
    QVERIFY(inside);
    QCOMPARE(nearestPlane(1.2, -0.95, 1.0, 0.1, 20, &inside), 19);
    QVERIFY(!inside);
    QCOMPARE(nearestPlane(-5.0, -0.95, 1.0, 0.1, 20, &inside), 0);
    QVERIFY(!inside);
    // axisWorld is its inverse.
    QCOMPARE(nearestPlane(axisWorld(-0.95, 1.0, 0.1, 7), -0.95, 1.0, 0.1, 20), 7);
}

void TestSimulationVolume::nearestPlane_handlesADecreasingAxisAndNoSpacing()
{
    bool inside = true;
    QCOMPARE(nearestPlane(9.0, 10.0, 1.0, -1.0, 5, &inside), 1);
    QVERIFY(inside);
    QCOMPARE(nearestPlane(1.0, 0.0, 1.0, 0.0, 5, &inside), -1);
    QVERIFY(!inside);
    QCOMPARE(nearestPlane(std::numeric_limits<double>::quiet_NaN(), 0.0, 1.0, 1.0, 5), -1);
}

void TestSimulationVolume::labels_nameTheAxisAndTheUnit()
{
    QCOMPARE(coordinateLabel(SliceAxis::X, 0.0, QStringLiteral("pc")), QStringLiteral("x = 0 pc"));
    QCOMPARE(coordinateLabel(SliceAxis::Z, 0.15, QString()), QStringLiteral("z = 0.15"));
    QCOMPARE(coordinateLabel(SliceAxis::Y, 1.5e18, QStringLiteral(" cm ")),
             QStringLiteral("y = 1.5e+18 cm"));
    QCOMPARE(formatNumber(0.123456789), QStringLiteral("0.12346"));
    QCOMPARE(formatNumber(std::numeric_limits<double>::infinity()), QStringLiteral("-"));
}

namespace {
QJsonObject sample()
{
    return QJsonDocument::fromJson(R"({
        "active": true, "source": "definitions", "origin": "definitions.h", "variable": "rho",
        "unit_density": 1.67e-24, "unit_length": 3.0857e18, "unit_velocity": 1.0e5,
        "value_quantity": "density", "time_code": 2.0,
        "columns": {
          "value": {"factor": 1.67e-24, "unit": "g/cm^3", "source": "definitions"},
          "x": {"factor": 3.0857e18, "unit": "cm", "source": "definitions"},
          "y": {"factor": 3.0857e18, "unit": "cm", "source": "definitions"},
          "z": {"factor": 3.0857e18, "unit": "cm", "source": "definitions"},
          "time": {"factor": 3.0857e13, "unit": "s", "source": "definitions"}}})").object();
}
}   // namespace

void TestSimulationVolume::quantityLabel_namesTheFieldAndItsUnit()
{
    // Every field's colour bar reads the same way: the main field's used to be
    // titled with the unit alone while the added ones carried their names.
    QCOMPARE(quantityLabel(QStringLiteral("density"), QStringLiteral("g/cm^3")),
             QStringLiteral("density (g/cm^3)"));
    QCOMPARE(quantityLabel(QStringLiteral("density"), QString()), QStringLiteral("density"));
    QCOMPARE(quantityLabel(QString(), QStringLiteral("g/cm^3")), QStringLiteral("g/cm^3"));
    QCOMPARE(quantityLabel(QString(), QString()), QString());
}

void TestSimulationVolume::physicalUnits_parseWhatTheBackendSends()
{
    const PhysicalUnits u = parsePhysicalUnits(sample());
    QVERIFY(u.active);
    QCOMPARE(u.columns.value(valueColumn()).unit, QStringLiteral("g/cm^3"));
    QCOMPARE(u.valueQuantity, QStringLiteral("density"));
    QCOMPARE(u.unitVelocity, 1.0e5);
    QVERIFY(u.hasValue() && u.hasLength() && u.hasTime());
    // A backend without the field, or a dataset with none.
    const PhysicalUnits none = parsePhysicalUnits(QJsonObject());
    QVERIFY(!none.active && !none.hasValue() && !none.hasLength() && !none.hasTime());
    QVERIFY(std::isnan(none.unitDensity));
}

void TestSimulationVolume::physicalUnits_scaleOnlyWhenOnAndKnown()
{
    const PhysicalUnits u = parsePhysicalUnits(sample());
    QCOMPARE(columnScale(u, valueColumn(), true), 1.67e-24);
    QCOMPARE(columnScale(u, valueColumn(), false), 1.0);
    QCOMPARE(columnScale(u, axisColumn(1), true), 3.0857e18);
    QCOMPARE(columnScale(u, axisColumn(1), false), 1.0);
    QCOMPARE(columnUnitLabel(u, valueColumn(), true, QString()), QStringLiteral("g/cm^3"));
    QCOMPARE(columnUnitLabel(u, valueColumn(), false, QStringLiteral("code")),
             QStringLiteral("code"));
    QCOMPARE(columnUnitLabel(u, axisColumn(2), true, QString()), QStringLiteral("cm"));
    // A variable the run gives nothing for (a tracer): no column, code units, factor 1.
    const PhysicalUnits tracer = parsePhysicalUnits(QJsonDocument::fromJson(
            R"({"active": true, "columns": {"x": {"factor": 2.0, "unit": "pc"}}})").object());
    QVERIFY(!tracer.hasValue() && tracer.hasLength());
    QCOMPARE(columnScale(tracer, valueColumn(), true), 1.0);
    QCOMPARE(columnUnitLabel(tracer, valueColumn(), true, QStringLiteral("")), QString());
}

void TestSimulationVolume::timeLabel_isCodeOrPhysical()
{
    const PhysicalUnits u = parsePhysicalUnits(sample());
    QCOMPARE(timeLabel(u, false), QStringLiteral("t = 2 (code units)"));
    QCOMPARE(timeLabel(u, true), QStringLiteral("t = 6.1714e+13 s"));
    QCOMPARE(timeLabel(parsePhysicalUnits(QJsonObject()), true), QString());
}

void TestSimulationVolume::unitsRequest_sendsOnlyWhatIsSet()
{
    UnitsRequest r;
    r.unitDensity = 1.0e-22;
    r.columns.insert(axisColumn(0), { 1.0, QStringLiteral("pc"), QString() });
    const QJsonObject body = unitsRequestBody(QStringLiteral("ds_1"), r);
    QCOMPARE(body.value("dataset_id").toString(), QStringLiteral("ds_1"));
    QCOMPARE(body.value("unit_density").toDouble(), 1.0e-22);
    QVERIFY(!body.contains("unit_length"));
    QVERIFY(!body.value("columns").toObject().contains("value"));
    QCOMPARE(body.value("columns").toObject().value("x").toObject().value("unit").toString(),
             QStringLiteral("pc"));
    QCOMPARE(body.value("clear").toBool(), false);
    UnitsRequest clear;
    clear.clear = true;
    QCOMPARE(unitsRequestBody(QStringLiteral("d"), clear).value("clear").toBool(), true);
}

void TestSimulationVolume::columns_serveAnyReaderThroughTheSameTable()
{
    // A Gadget snapshot's columns arrive the same way a volume's do, with the
    // reader's own table (empty source) or what the user typed.
    const PhysicalUnits u = parsePhysicalUnits(QJsonDocument::fromJson(R"({
        "columns": {"mass": {"factor": 1.0e10, "unit": "Msun", "source": "user"},
                    "hsml": {"factor": 0.5, "unit": "kpc"},
                    "junk": {"factor": 0, "unit": "x"}}})").object());
    QCOMPARE(columnScale(u, QStringLiteral("mass"), true), 1.0e10);
    QCOMPARE(columnScale(u, QStringLiteral("mass"), false), 1.0);
    QCOMPARE(columnScale(u, QStringLiteral("nope"), true), 1.0);
    QCOMPARE(columnUnitLabel(u, QStringLiteral("hsml"), true, QStringLiteral("code")),
             QStringLiteral("kpc"));
    QCOMPARE(columnUnitLabel(u, QStringLiteral("hsml"), false, QStringLiteral("code")),
             QStringLiteral("code"));
    QVERIFY(!u.columns.contains(QStringLiteral("junk")));
    UnitsRequest r;
    r.columns.insert(QStringLiteral("mass"), { 2.0, QStringLiteral("Msun"), QString() });
    r.columns.insert(QStringLiteral("bad"), { -1.0, QString(), QString() });
    const QJsonObject columns = unitsRequestBody(QStringLiteral("d"), r).value("columns").toObject();
    QCOMPARE(columns.value("mass").toObject().value("unit").toString(), QStringLiteral("Msun"));
    QVERIFY(!columns.contains("bad"));
}
