#pragma once
// TestSedPlotStyle.h
// QTest class for how the SED window draws its photometry (SedPlotStyle.h):
// band to survey, the palette's contrast, the axis ranges, the links of the
// band-merged tree and the error bars on a log axis.
// Header-only logic; no window.

#include <QObject>

class TestSedPlotStyle : public QObject
{
    Q_OBJECT

private slots:
    void bandTagsMapToSurveys();
    void paletteReadsOnBothThemes();
    void fluxRangeComesFromPhotometryOnly();
    void linearRangeIsProportional();
    void singleBandHasARange();
    void upperLimitsAndErrorsWidenTheRange();
    void wavelengthRangeCoversTheBands();
    void treeLinksFollowTheBranches();
    void collapsedSedIsOneLine();
    void theTreeIsRebuiltFromTheViewRows();
    void logErrorBarsStopAtTheFloor();
};
