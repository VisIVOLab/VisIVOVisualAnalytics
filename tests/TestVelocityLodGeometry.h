#ifndef TestVelocityLodGeometry_h
#define TestVelocityLodGeometry_h

#include <QObject>

class TestVelocityLodGeometry : public QObject
{
    Q_OBJECT

private slots:
    void factorsMatchTheBackend();
    void linearFieldReducedValuesSitAtTheirWorldPositions();
    void reportedGeometryIsRescaledToTheViewerBox();
    void thinGridKeepsItsShortAxis();
    void basinAndDensityGridsArePlacedAtBlockCentres();
    void olderBackendSingleFactorIsInferredPerAxis();
    void backendProductsAreRecomputedOnlyWhenTheCellRatioChanges();
    void basinsFetchReadsTheGridGeometry();
    void basinsFetchOfAnOlderBackendHasNoGeometry();
    void densityFetchSendsTheViewerBox();
    void staleDensityReplyIsDroppedAfterTwoBoxChanges();
};

#endif
