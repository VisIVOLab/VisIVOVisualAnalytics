#ifndef TestOrbitPath_h
#define TestOrbitPath_h

#include <QObject>

class TestOrbitPath : public QObject
{
    Q_OBJECT

private slots:
    void aTurnCirclesTheFocalPointAtItsDistance();
    void anArcEndsOnItsAngle();
    void theOtherFieldsAreKept();
    void turningKeepsTheDistanceAndTiltsTheUp();
    void dollyAndPan();
};

#endif // TestOrbitPath_h
