#pragma once
// TestLayerCardGeometry.h
// QTest class for the layer card's layout arithmetic — the click target and the
// gradient strip. Header-only geometry; no widgets, no painting.

#include <QObject>

class TestLayerCardGeometry : public QObject
{
    Q_OBJECT

private slots:
    void theCheckboxSitsInsideTheRow();
    void theClickTargetCoversTheWholeLeftGutter();
    void theClickTargetStopsBeforeTheName();
    void theStripLeavesRoomForTheDetailText();
    void theStripNeverInvertsInANarrowDock();
    void theStripSitsBelowTheCheckbox();
};
