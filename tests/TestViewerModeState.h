#pragma once
// TestViewerModeState.h
// QTest class for the image viewer's mode precedence — which of the five
// mutually exclusive modes the strip shows. Header-only logic; no window.

#include <QObject>

class TestViewerModeState : public QObject
{
    Q_OBJECT

private slots:
    void nothingArmedIsPan();
    void eachModeIsReportedOnItsOwn();
    void theModeHoldingTheMouseWins();
    void probeYieldsToEveryTool();
};
