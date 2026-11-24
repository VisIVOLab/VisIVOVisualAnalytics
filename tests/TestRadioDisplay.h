#pragma once
// TestRadioDisplay.h
// QTest class for the display rules of issue #502: beam and angular-size
// formatting, the words for a local backend, the Channel Maps panel layout and
// the PV drawing hint. All header-only logic; no window.

#include <QObject>

class TestRadioDisplay : public QObject
{
    Q_OBJECT

private slots:
    void aMilliarcsecondBeamIsNotZero();
    void aBeamPairSharesOneUnit();
    void sizesPickTheirUnit();
    void threeSignificantDigitsSurviveRounding();
    void beamSummaryUsesTheFormatter();

    void aLocalBackendIsNotCalledRemote();

    void columnsAskedForFitTheViewport();
    void aPanelKeepsTheImageAspect();
    void panelsOfAColumnShareTheirMargins();

    void theDrawingHintSaysHowToFinish();
};
