#ifndef TestParticleFrame_h
#define TestParticleFrame_h

#include <QObject>

class TestParticleFrame : public QObject
{
    Q_OBJECT

private slots:
    void theHeadersGiveTheFrame();
    void aMissingOrBadFrameIsTheIdentity();
    void particlesFarFromZeroStayApartInTheTable();
    void theCameraIsMovedIntoTheFrameInDouble();
    void aBoxFarFromZeroIsCentred();
};

#endif // TestParticleFrame_h
