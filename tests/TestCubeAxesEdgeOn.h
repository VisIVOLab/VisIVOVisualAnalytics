#ifndef TestCubeAxesEdgeOn_h
#define TestCubeAxesEdgeOn_h

#include <QObject>

/// The rule that leaves an end-on axis of the labelled box out (issue #569).
class TestCubeAxesEdgeOn : public QObject
{
    Q_OBJECT

private slots:
    void aViewAlongAnAxisDropsOnlyThatAxis();
    void anObliqueViewKeepsEveryAxis();
    void theAngleDecidesNotTheDistance();
    void aParallelViewFollowsItsFocalPoint();
    void anEyeInTheMiddleOfTheBoxKeepsEveryAxis();
};

#endif // TestCubeAxesEdgeOn_h
