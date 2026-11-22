#ifndef TestRaytraceViewRules_h
#define TestRaytraceViewRules_h

#include <QObject>

// The ray-traced GPU view's widget-free rules (issue #447): how a particle's
// colour value lands in the table (ScalarColourRange.h) and what the loading
// overlay shows (RaytraceLoadingState.h).
class TestRaytraceViewRules : public QObject
{
    Q_OBJECT

private slots:
    void nanTakesTheNanColour();
    void cgsDensitiesKeepTheirRange();
    void aLogScaleNeedsAPositiveLowEnd();
    void aFlatRangeTakesTheLowestColour();
    void theFirstLoadCoversTheView();
    void aRefetchOverAFrameIsABadge();
    void errorsNeverLeaveASpinner();
    void aGpuFailureCoversUntilTheBackendsFrame();
};

#endif
