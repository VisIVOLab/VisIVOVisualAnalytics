#pragma once
// TestCatalogueSelection.h
// QTest class for a 3-D catalogue window's selection logic (issue #448):
// region names, the applied-filter snapshot Selection Diagnostics follows,
// and the camera that frames a SubFind target. No window, no VTK.

#include <QObject>

class TestCatalogueSelection : public QObject
{
    Q_OBJECT

private slots:
    void regionNamesAreOfferedInTurn();
    void aRegionNameInUseIsRefused();
    void editingARegionKeepsItsOwnName();
    void diagnosticsFollowTheAppliedFilters();
    void aFailedQueryPutsThePreviousFiltersBack();
    void loadAllKeepsTheAppliedFilters();
    void aPeerOpenedMidApplyTakesTheSettledFilters();
    void theFramingDistanceGrowsWithTheRadius();
    void framingKeepsTheViewDirection();
    void aRecentreKeepsTheDistance();
    void anUnmovedViewIsTheSameView();
    void theSizedRadiusIsPreferred();
    void radiiSayWhatTheyAreDefinedAgainst();
    void cameraJsonRoundTrips();
};
