#pragma once
// TestProductPaneDisplay.h
// QTest class for what a 2-D product pane shows about itself (issue #404):
// the Linear / Log / Sqrt control, the values down its colour bar
// (LutScale::valueAt), the log floor, and the status-line summary of how the
// product was made (ProductPaneDisplay.h). Header-only logic; no window.

#include <QObject>

class TestProductPaneDisplay : public QObject
{
    Q_OBJECT

private slots:
    void segmentsRoundTrip();
    void editorOnlyStretchesShowAsLinear();
    void linearBarIsEven();
    void logBarIsLogSpaced();
    void logBarOnSignedDataStartsAtTheFloor();
    void sqrtBarInvertsTheStretch();
    void percentileFloorTakesTheFaintEnd();
    void percentileFloorFollowsPhysicalUnits();
    void momentSummary();
    void maskedMomentSummary();
    void linewidthSummaryNamesTheMethodOnce();
    void linewidthSummaryIsShort();
    void pvAndRmSummaries();
    void bareLabelWithoutRecipe();
};
