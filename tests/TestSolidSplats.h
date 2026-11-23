#ifndef TestSolidSplats_h
#define TestSolidSplats_h

#include <QObject>

class TestSolidSplats : public QObject
{
    Q_OBJECT

private slots:
    void staleNaNScalarsAreClearedWhenGoingSolid();
    void theTableMapsEverythingNaNIncludedToTheSolidColour();
    void gaussianDefaultsAvoidImmediateSaturation();
    void theVr8BitRuleKeepsItsSteps();
    void gaussianOpacityIsUnquantisedWithNoFloor();
    void theSplatShaderMatchesThePointSession();
    void theRenderStrideKeepsTheBudget();
    void splatLightDoesNotDependOnTheRows();
    void splatSizeDefaultDoesNotCarryOverToPoints();
};

#endif
