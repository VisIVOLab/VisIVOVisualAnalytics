#pragma once
// TestSequentialColors.h
// QTest class for the white-to-hue colour maps (SequentialColorTables.h): the
// ends of each table, one direction of lightness, the reversed read, and the
// window/level drag arithmetic (WindowLevelDrag.h). Header-only logic; no VTK.

#include <QObject>

class TestSequentialColors : public QObject
{
    Q_OBJECT

private slots:
    void everyMapStartsLightAndEndsDark();
    void lightnessNeverRisesAlongAMap();
    void aReversedMapReadsTheTableFromTheTop();
    void theSixMapsAreNamedAsTheRegistryNamesThem();
    void aRightDragWidensTheWindowAndALeftDragNarrowsIt();
    void aVerticalDragMovesTheLevelAndKeepsTheWindow();
    void aDragBackUndoesADragForward();
    void anEmptyStartRangeStillGivesAFiniteRange();
};
