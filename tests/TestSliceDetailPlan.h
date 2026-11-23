#pragma once
// TestSliceDetailPlan.h
// QTest class for SliceDetailPlan.h: the stride of a remote channel and the
// detail asked for a zoomed 2-D view (issue #381). Header-only, GUI/VTK-free.

#include <QObject>

class TestSliceDetailPlan : public QObject
{
    Q_OBJECT

private slots:
    void stride_matchesTheBackend();
    void visible_isClampedToThePlane();
    void visible_missingThePlaneIsEmpty();
    void detail_noneWhenNotFinerThanTheChannel();
    void detail_zoomedInAsksForThePartOnScreen();
    void detail_fullResolutionOnceThePartFits();
    void covers_onlyWhenInsideAndFineEnough();
};
